// SPDX-License-Identifier: Apache-2.0
// Lunar Flip: a moonbase pinball table seen from the player's end. Pop
// bumpers, slingshots, three top lanes that raise the multiplier, a bank of
// drop targets and a crater target; a ball save after each launch. Flippers
// on the screen's halves (or the Interaction module's buttons), a tap
// launches, a shake nudges (too many and it tilts). Physics in table units
// (10 wide, 20 long, y up the table), drawn with the shared engine.
#include <stdio.h>
#include <string.h>

#include <vector>

#include "games_3d.h"

namespace games {
namespace {

constexpr float BALL_R = 0.27f, GRAVITY = 15, WALL_H = 0.38f, FLIP_L = 1.75f, FLIP_R = 0.14f;
constexpr float CX = 4.65f;  // the playfield's middle (the shooter lane is right of 8.9)
constexpr int SUB = 8;       // physics steps a frame

struct Seg {
    P2 a, b;
    float kick;    // pushes the ball off (slingshots)
    int target;    // a drop target's number, or -1 (-2: a one-way gate, felt only by a falling ball)
};
struct Post {
    P2 c;
    float r, kick;
    int kind;  // 0 bumper, 1 crater, 2 lane post
};
struct Flipper {
    P2 pivot;
    float base, sign, a, w;  // direction = base + sign * a; a from REST (down) to UP; w its speed
    bool up;
};
constexpr float REST = -0.5f, UP = 0.5f;

class Pinball final : public Game {
public:
    Pinball()
    {
        build_table();
        Rng r(11);
        for (auto &s : stars_) {
            s = {r.range(0.7f, 8.6f), r.range(3, 18.5f)};
        }
    }

    void reset(uint32_t seed, bool demo) override
    {
        rng_ = Rng(seed);
        demo_ = demo, over_ = false, score_ = 0, events_ = 0;
        balls_ = 3, mult_ = 1, tilt_ = false, time_ = 0, save_ = 0, nudges_ = 0, nudge_clock_ = 0;
        for (bool &l : lanes_) {
            l = false;
        }
        for (bool &d : drops_) {
            d = false;
        }
        drops_reset_ = 0, crater_hits_ = 0;
        for (float &f : flash_) {
            f = 0;
        }
        flips_[0] = {{CX - 2.25f, 2.6f}, 0, 1, REST, 0, false};
        flips_[1] = {{CX + 2.25f, 2.6f}, PI, -1, REST, 0, false};
        serve();
        sparks_.clear();
    }

    void step(float dt, const Input &input) override
    {
        if (over_) {
            return;
        }
        dt = std::min(dt, 0.05f);
        time_ += dt;
        const Input in = demo_ ? autopilot() : input;
        flips_[0].up = in.left && !tilt_;
        flips_[1].up = in.right && !tilt_;
        if (in_lane_ && (in.action || demo_)) {  // the plunger
            launch_wait_ -= demo_ ? dt : 99;
            if (launch_wait_ <= 0) {
                in_lane_ = false;
                v_ = {0, 24.2f + rng_.range(0, 1.2f)};  // enough to round the top arc
                save_ = 7;
                events_ |= EV_POWER;
            }
        } else if (in.shake && !in_lane_ && !tilt_) {  // a nudge; three in a few seconds tilt the table
            v_.x += rng_.range(-1.6f, 1.6f), v_.y += rng_.range(1.5f, 3.0f);
            nudge_clock_ = time_;
            if (++nudges_ >= 3) {
                tilt_ = true;
                events_ |= EV_WARN;
            }
        }
        if (time_ - nudge_clock_ > 6) {
            nudges_ = 0;
        }
        save_ = std::max(0.0f, save_ - dt);
        for (float &f : flash_) {
            f = std::max(0.0f, f - dt);
        }
        if (drops_reset_ > 0 && (drops_reset_ -= dt) <= 0) {
            for (bool &d : drops_) {
                d = false;
            }
        }
        const float h = dt / SUB;
        for (int s = 0; s < SUB; ++s) {
            for (Flipper &f : flips_) {
                const float to = f.up ? UP : REST, speed = f.up ? 20.0f : 13.0f;
                const float before = f.a;
                f.a += clampf(to - f.a, -speed * h, speed * h);
                f.w = (f.a - before) / h;
            }
            if (!in_lane_) {
                physics(h);
            }
        }
        sparks_.step(dt, {0, -6, 0});
        // a weak launch that rolled back: ready to launch again
        if (!in_lane_ && p_.x > 8.9f && p_.y < 1.2f && fabsf(v_.y) < 0.5f) {
            serve();
        }
        // a ball that sits still somewhere gets a kick (a table's ball search)
        still_ = (!in_lane_ && hypotf(v_.x, v_.y) < 0.4f) ? still_ + dt : 0;
        if (still_ > 3) {
            v_ = {rng_.range(-3, 3), 6};
            still_ = 0;
        }
        if (p_.y < -0.7f) {  // drained
            if (save_ > 0 && !tilt_) {
                events_ |= EV_POWER;  // saved
            } else {
                --balls_;
                events_ |= balls_ > 0 ? EV_HIT : EV_OVER;
                if (balls_ <= 0) {
                    if (demo_) {
                        reset(rng_.next(), true);
                        return;
                    }
                    over_ = true;
                    return;
                }
            }
            tilt_ = false, nudges_ = 0;
            serve();
        }
    }

    void render(uint16_t *fb) override;

    void hud(Hud *h) const override
    {
        h->score = score_;
        h->meter = float(balls_) / 3;
        h->meter_color = tilt_ ? 0xFF4A4A : 0x9AB8FF;
        h->meter_name = "Balls";
        if (tilt_) {
            snprintf(h->line, sizeof(h->line), "TILT · ball %d", 4 - balls_);
        } else if (in_lane_) {
            snprintf(h->line, sizeof(h->line), "Ball %d · tap to launch · x%d", 4 - balls_, mult_);
        } else {
            snprintf(h->line, sizeof(h->line), "Ball %d · x%d%s", 4 - balls_, mult_, save_ > 0 ? " · saved" : "");
        }
    }
    const char *ending() const override
    {
        return "Last ball drained";
    }
    void profile(char *out, int capacity) override
    {
        prof_.report(out, capacity);
    }

private:
    void add(float x1, float y1, float x2, float y2, float kick = 0, int target = -1)
    {
        segs_.push_back({{x1, y1}, {x2, y2}, kick, target});
    }

    void build_table()
    {
        // the outline: left wall, the rounded top, the right wall past the shooter lane
        add(0.4f, 2.9f, 0.4f, 16.4f);
        P2 prev{0.4f, 16.4f};
        for (int i = 1; i <= 12; ++i) {
            const float a = PI - float(i) / 12 * PI;
            const P2 q{5.0f + 4.6f * cosf(a), 16.4f + 3.2f * sinf(a)};
            add(prev.x, prev.y, q.x, q.y);
            prev = q;
        }
        add(9.6f, 16.4f, 9.6f, 0.2f);
        add(8.9f, 0.2f, 8.9f, 13.6f);  // the shooter lane's inner wall
        add(8.9f, 0.2f, 9.6f, 0.2f);   // the plunger's floor
        add(9.6f, 14.9f, 8.9f, 14.2f, 0, -2);  // the lane's one-way gate: out, never back in
        // guides down to the flippers
        // (they run into the flippers' pivots: no notch for a ball to sit in)
        add(0.4f, 5.4f, CX - 2.25f, 2.6f);
        add(8.9f, 5.4f, CX + 2.25f, 2.6f);
        add(0.4f, 2.9f, 0.4f, 5.4f);
        // slingshots: their inner faces kick
        // (clear of the guides below them: the inlanes let the ball roll to the flippers)
        add(1.55f, 5.1f, 1.55f, 6.7f);
        add(1.55f, 6.7f, 2.65f, 4.35f, 5.5f);
        add(2.65f, 4.35f, 1.55f, 5.1f);
        add(7.75f, 5.1f, 7.75f, 6.7f);
        add(7.75f, 6.7f, 6.65f, 4.35f, 5.5f);
        add(6.65f, 4.35f, 7.75f, 5.1f);
        // top lane guides
        for (float x : {
                    2.1f, 3.75f, 5.55f, 7.2f
                }) {
            add(x, 16.5f, x, 17.6f);
        }
        // three drop targets on the left
        for (int i = 0; i < 3; ++i) {
            add(0.55f, 8.9f + float(i) * 0.85f, 0.55f, 9.55f + float(i) * 0.85f, 0, i);
        }
        posts_ = {{{3.25f, 13.1f}, 0.66f, 6.5f, 0}, {{6.05f, 13.1f}, 0.66f, 6.5f, 0}, {{CX, 15.1f}, 0.66f, 6.5f, 0},
            {{CX, 9.3f}, 0.45f, 3.0f, 1}
        };
    }

    void serve()
    {
        in_lane_ = true;
        launch_wait_ = 0.7f;
        p_ = {9.25f, 0.2f + BALL_R + 0.02f};
        v_ = {0, 0};
    }

    void award(int points)
    {
        score_ += points * mult_;
    }

    // One small step: gravity, walls, posts, flippers.
    void physics(float h)
    {
        v_.y -= GRAVITY * h;
        const float speed = hypotf(v_.x, v_.y);
        if (speed > 26) {
            v_.x *= 26 / speed, v_.y *= 26 / speed;
        }
        p_.x += v_.x * h, p_.y += v_.y * h;
        for (const Seg &s : segs_) {
            if ((s.target >= 0 && drops_[s.target]) || (s.target == -2 && v_.y > 0)) {
                continue;  // dropped, or a gate the ball passes going up
            }
            P2 n;
            float depth;
            if (!touch_segment(s.a, s.b, BALL_R, &n, &depth)) {
                continue;
            }
            p_.x += n.x * depth, p_.y += n.y * depth;
            bounce(n, {0, 0}, 0.45f);
            if (s.kick > 0) {
                v_.x += n.x * s.kick, v_.y += n.y * s.kick;
                award(10);
                spark(0xFFB040);
            }
            if (s.target >= 0) {
                drops_[s.target] = true;
                award(500);
                events_ |= EV_PICKUP;
                spark(0xFF6060);
                if (drops_[0] && drops_[1] && drops_[2]) {
                    award(5000);
                    events_ |= EV_LEVEL;
                    drops_reset_ = 2;
                }
            }
        }
        for (size_t i = 0; i < posts_.size(); ++i) {
            const Post &c = posts_[i];
            const float dx = p_.x - c.c.x, dy = p_.y - c.c.y, d = hypotf(dx, dy), reach = BALL_R + c.r;
            if (d >= reach || d < 1e-4f) {
                continue;
            }
            const P2 n{dx / d, dy / d};
            p_.x = c.c.x + n.x * reach, p_.y = c.c.y + n.y * reach;
            bounce(n, {0, 0}, 0.5f);
            v_.x += n.x * c.kick, v_.y += n.y * c.kick;
            if (flash_[i] <= 0.05f) {
                if (c.kind == 0) {
                    award(100);
                    events_ |= EV_PICKUP;
                    spark(0x9AB8FF);
                } else {
                    award(1000);
                    events_ |= EV_PICKUP;
                    spark(0xFFE070);
                    if (++crater_hits_ % 4 == 0) {
                        mult_ = std::min(6, mult_ + 1);
                        events_ |= EV_LEVEL;
                    }
                }
            }
            flash_[i] = 0.18f;
        }
        for (Flipper &f : flips_) {
            const float dir = f.base + f.sign * f.a;
            const P2 tip{f.pivot.x + cosf(dir) * FLIP_L, f.pivot.y + sinf(dir) * FLIP_L};
            P2 n;
            float depth;
            if (!touch_segment(f.pivot, tip, BALL_R + FLIP_R, &n, &depth)) {
                continue;
            }
            p_.x += n.x * depth, p_.y += n.y * depth;
            // the flipper's surface moves where the ball touches it
            const float w = f.sign * f.w, rx = p_.x - f.pivot.x, ry = p_.y - f.pivot.y;
            bounce(n, {-w * ry, w * rx}, 0.3f);
        }
        // the top lanes: a ball passing through lights its lane; all three raise the multiplier
        if (p_.y > 16.7f && p_.y < 17.4f && v_.y > 0) {
            const int lane = p_.x < 3.75f ? 0 : p_.x < 5.55f ? 1 : p_.x < 7.2f ? 2 : -1;
            if (lane >= 0 && p_.x > 2.1f && !lanes_[lane]) {
                lanes_[lane] = true;
                award(250);
                events_ |= EV_PICKUP;
                if (lanes_[0] && lanes_[1] && lanes_[2]) {
                    mult_ = std::min(6, mult_ + 1);
                    lanes_[0] = lanes_[1] = lanes_[2] = false;
                    events_ |= EV_LEVEL;
                }
            }
        }
    }

    bool touch_segment(P2 a, P2 b, float reach, P2 *n, float *depth) const
    {
        const float ex = b.x - a.x, ey = b.y - a.y, len2 = ex * ex + ey * ey;
        const float t = clampf(((p_.x - a.x) * ex + (p_.y - a.y) * ey) / std::max(len2, 1e-6f), 0, 1);
        const float qx = a.x + ex * t, qy = a.y + ey * t, dx = p_.x - qx, dy = p_.y - qy;
        const float d = hypotf(dx, dy);
        if (d >= reach || d < 1e-5f) {
            return false;
        }
        *n = {dx / d, dy / d};
        *depth = reach - d;
        return true;
    }

    void bounce(P2 n, P2 surface, float restitution)
    {
        const float rvx = v_.x - surface.x, rvy = v_.y - surface.y, vn = rvx * n.x + rvy * n.y;
        if (vn < 0) {
            v_.x -= (1 + restitution) * vn * n.x;
            v_.y -= (1 + restitution) * vn * n.y;
        }
    }

    void spark(uint32_t colour)
    {
        sparks_.burst(world(p_, BALL_R), 6, 3, hex(colour), 0.35f, 0.1f, rng_);
    }

    static V3 world(P2 t, float up = 0)
    {
        return {t.x - 5.0f, up, t.y};
    }

    // For the title screen: a flip as the ball comes down onto a flipper; a
    // ball resting on a raised flipper is let roll, then flipped.
    Input autopilot()
    {
        Input in;
        for (int i = 0; i < 2; ++i) {
            const Flipper &f = flips_[i];
            const float dir = f.base + f.sign * REST, dx = p_.x - f.pivot.x, dy = p_.y - f.pivot.y;
            const float along = dx * cosf(dir) + dy * sinf(dir), off = -dx * sinf(dir) + dy * cosf(dir);
            // out along the flipper and close above it, not climbing away
            const bool hit = along > 0.55f && along < FLIP_L + 0.3f && fabsf(off) < 0.9f && v_.y < 2;
            (i == 0 ? in.left : in.right) = hit;
        }
        return in;
    }

    Rng rng_;
    std::vector<Seg> segs_;
    std::vector<Post> posts_;
    Flipper flips_[2];
    P2 p_{9.25f, 0.5f}, v_{0, 0};
    int balls_ = 3, mult_ = 1, nudges_ = 0, crater_hits_ = 0;
    bool lanes_[3] = {}, drops_[3] = {}, tilt_ = false, in_lane_ = true;
    float time_ = 0, save_ = 0, nudge_clock_ = 0, drops_reset_ = 0, launch_wait_ = 0, still_ = 0;
    float flash_[8] = {};
    P2 stars_[50];
    Particles sparks_;
    Camera cam_;
    Profile prof_{{"table", "walls", "ball"}};
};

void Pinball::render(uint16_t *fb)
{
    prof_.start();
    cam_.focal = 262;  // steep enough that the far end is not squashed; the table fills the screen
    cam_.cy = float(H) / 2 + 12;
    cam_.look_at({0.3f, 19, -2.2f}, {0.3f, 0, 9.0f});
    std::fill_n(fb, W * H, hex(0x05060E));
    // the table: its outline filled deep blue, darker towards the far end
    V3 outline[20];
    int n = 0;
    outline[n++] = world({0.4f, 0});
    outline[n++] = world({0.4f, 16.4f});
    for (int i = 1; i < 12; ++i) {
        const float a = PI - float(i) / 12 * PI;
        outline[n++] = world({5.0f + 4.6f * cosf(a), 16.4f + 3.2f * sinf(a)});
    }
    outline[n++] = world({9.6f, 16.4f});
    outline[n++] = world({9.6f, 0});
    cam_.poly_world(fb, outline, n, hex(0x0E1A3A));
    // printed art: stars, a big moon with craters, lane arrows lit or not
    for (const P2 &s : stars_) {
        P2 q;
        if (cam_.screen(world(s), &q)) {
            const int x = int(q.x), y = int(q.y);
            if (unsigned(x) < unsigned(W) && unsigned(y) < unsigned(H)) {
                fb[y * W + x] = hex(0x6A7AB8);
            }
        }
    }
    const auto disc = [&](P2 c, float r, uint16_t colour, float up = 0.001f) {
        V3 pts[16];
        for (int i = 0; i < 16; ++i) {
            const float a = float(i) / 16 * TAU;
            pts[i] = world({c.x + r * cosf(a), c.y + r * sinf(a)}, up);
        }
        cam_.poly_world(fb, pts, 16, colour);
    };
    disc({CX, 9.3f}, 2.5f, hex(0x2A3A68));
    disc({CX - 1.0f, 10.4f}, 0.5f, hex(0x223058));
    disc({CX + 1.3f, 8.4f}, 0.7f, hex(0x223058));
    disc({CX - 0.9f, 7.9f}, 0.35f, hex(0x223058));
    for (int i = 0; i < 3; ++i) {
        const float x = 2.92f + float(i) * 1.73f;
        const V3 tri[3] = {world({x, 15.9f}), world({x - 0.35f, 15.3f}), world({x + 0.35f, 15.3f})};
        cam_.poly_world(fb, tri, 3, lanes_[i] ? hex(0xFFD24A) : hex(0x3A3A20));
    }
    // the multiplier: lights across the bottom of the moon
    for (int i = 0; i < 6; ++i) {
        disc({CX - 1.25f + float(i) * 0.5f, 6.4f}, 0.14f, i < mult_ ? hex(0x9AF4FF) : hex(0x1A2A48));
    }
    prof_.mark(0);
    // walls: a dark side and a bright top edge, far ones first
    struct W2 {
        float z;
        int i;
    } order[48];
    int count = 0;
    for (size_t i = 0; i < segs_.size() && count < 48; ++i) {
        order[count++] = {(segs_[i].a.y + segs_[i].b.y) / 2, int(i)};
    }
    std::sort(order, order + count, [](const W2 & a, const W2 & b) {
        return a.z > b.z;
    });
    for (int k = 0; k < count; ++k) {
        const Seg &s = segs_[size_t(order[k].i)];
        const bool drop = s.target >= 0;
        if ((drop && drops_[s.target]) || s.target == -2) {
            continue;  // dropped; the gate is drawn as a thin rail below
        }
        const float hgt = drop ? 0.55f : WALL_H;
        const V3 side[4] = {world(s.a), world(s.b), world(s.b, hgt), world(s.a, hgt)};
        const uint16_t face = drop ? hex(0xC83040) : s.kick > 0 ? hex(0xD07020) : hex(0x28324E);
        cam_.poly_world(fb, side, 4, face);
        cam_.edge_world(fb, world(s.a, hgt), world(s.b, hgt), drop ? hex(0xFF8090) : s.kick > 0 ? hex(0xFFC070) : hex(0x8CA4E0));
    }
    // posts: bumpers and the crater, as short glowing cylinders
    for (size_t i = 0; i < posts_.size(); ++i) {
        const Post &c = posts_[i];
        const bool lit = flash_[i] > 0;
        const float hgt = c.kind == 0 ? 0.55f : 0.2f;
        const uint16_t top = c.kind == 0 ? (lit ? hex(0xFFFFFF) : hex(0x6A8CFF)) : (lit ? hex(0xFFF0B0) : hex(0xC8A040));
        // the body's front, then the cap
        V3 band[4];
        for (int k = 0; k < 8; ++k) {
            const float a0 = PI + float(k) / 8 * PI, a1 = PI + float(k + 1) / 8 * PI;
            band[0] = world({c.c.x + c.r * cosf(a0), c.c.y + c.r * sinf(a0)});
            band[1] = world({c.c.x + c.r * cosf(a1), c.c.y + c.r * sinf(a1)});
            band[2] = world({c.c.x + c.r * cosf(a1), c.c.y + c.r * sinf(a1)}, hgt);
            band[3] = world({c.c.x + c.r * cosf(a0), c.c.y + c.r * sinf(a0)}, hgt);
            cam_.poly_world(fb, band, 4, c.kind == 0 ? hex(0x2A3A80) : hex(0x6A5020));
        }
        disc(c.c, c.r, top, hgt);
        if (lit) {
            cam_.glow_at(fb, world(c.c, hgt), c.r * 2.2f, scale565(top, 14), 0, 30);
        }
    }
    // flippers: a white blade with a red rubber edge
    for (const Flipper &f : flips_) {
        const float dir = f.base + f.sign * f.a, px = -sinf(dir), py = cosf(dir);
        const P2 tip{f.pivot.x + cosf(dir) * FLIP_L, f.pivot.y + sinf(dir) * FLIP_L};
        const float wb = 0.2f, wt = 0.09f, hgt = 0.3f;
        const V3 top[4] = {world({f.pivot.x + px * wb, f.pivot.y + py * wb}, hgt), world({tip.x + px * wt, tip.y + py * wt}, hgt),
                           world({tip.x - px * wt, tip.y - py * wt}, hgt), world({f.pivot.x - px * wb, f.pivot.y - py * wb}, hgt)
                          };
        const V3 front[4] = {world({f.pivot.x - px * wb, f.pivot.y - py * wb}), world({tip.x - px * wt, tip.y - py * wt}), top[2], top[3]};
        cam_.poly_world(fb, front, 4, hex(0xA02028));
        cam_.poly_world(fb, top, 4, hex(0xF0F0F4));
        disc(f.pivot, 0.12f, hex(0xC8C8D0), hgt + 0.01f);
    }
    // the plunger's spring
    for (int i = 0; i < 6; ++i) {
        const float y = 0.25f + float(i) * 0.12f;
        cam_.edge_world(fb, world({8.95f, y}, 0.1f), world({9.55f, y + 0.06f}, 0.1f), hex(0xA0A0B0));
    }
    prof_.mark(1);
    // the ball: a shadow, then a shiny sphere
    disc({p_.x + 0.12f, p_.y - 0.1f}, BALL_R, hex(0x04060C), 0.002f);
    P2 c;
    float z;
    if (cam_.screen(world(p_, BALL_R), &c, &z)) {
        const float r = cam_.focal * BALL_R / z;
        const int x1 = std::max(0, int(c.x - r)), x2 = std::min(W - 1, int(c.x + r));
        const int y1 = std::max(0, int(c.y - r)), y2 = std::min(H - 1, int(c.y + r));
        for (int y = y1; y <= y2; ++y)
            for (int x = x1; x <= x2; ++x) {
                const float u = (float(x) + 0.5f - c.x) / r, v = (float(y) + 0.5f - c.y) / r, d2 = u * u + v * v;
                if (d2 > 1) {
                    continue;
                }
                const float light = clampf(0.55f - 0.45f * (u + v) * 0.7f, 0.15f, 1), hi = d2 < 1 ? expf(-((u + 0.35f) * (u + 0.35f) + (v + 0.4f) * (v + 0.4f)) * 18) : 0;
                fb[y * W + x] = shade(0xD8DCE8, light, 0xFFFFFF, hi);
            }
    }
    sparks_.draw(fb, cam_);
    if (tilt_) {  // the table is dead: a red wash
        for (int i = 0; i < W * H; ++i) {
            fb[i] = mix565(fb[i], hex(0x400008), 10);
        }
    }
    prof_.mark(2);
}

}  // namespace

std::unique_ptr<Game> make_pinball()
{
    return std::make_unique<Pinball>();
}

}  // namespace games
