// SPDX-License-Identifier: Apache-2.0
// Ridge Rally: a gravel stage through autumn pines at sunset, against the
// clock. The road is made as you drive (curves and hills), the car slides on
// gravel and slows off the road, rivals are there to pass, and checkpoints
// buy time. A shake (or a tap) is a short boost. Everything is drawn with the
// shared engine: the road's quads, the pines, rocks, arches and cars as meshes,
// the dust as particles.
#include <stdio.h>
#include <string.h>

#include <deque>
#include <vector>

#include "games_3d.h"

namespace games {
namespace {

constexpr float SEG_L = 2.0f, HALF = 3.2f;  // a road segment's length; half the road's width
constexpr int AHEAD = 88, CHECK_EVERY = 230;
constexpr float V_ROAD = 34, V_OFF = 15;
constexpr uint32_t HAZE = 0xE8A07A, SKY_TOP = 0x3A2A6A;

struct Segment {
    V3 c;          // the road's middle at the segment's start
    float heading;  // its direction (radians, 0 = +z)
    float curve;    // heading change across it
    int index;      // from the stage's start
    uint8_t left, right;  // what stands beside it: 0 nothing, 1 a pine, 2 a rock
    float lpos, rpos;     // how far out (road half-widths)
};

// a rally car: a wedge of a hatchback (body, glass) and four dark wheels
const V3 CAR_V[16] = {
    {-0.8f, 0.2f, 1.7f}, {0.8f, 0.2f, 1.7f}, {0.85f, 0.2f, -1.7f}, {-0.85f, 0.2f, -1.7f},   // body bottom
    {-0.8f, 0.7f, 1.6f}, {0.8f, 0.7f, 1.6f}, {0.85f, 0.75f, -1.7f}, {-0.85f, 0.75f, -1.7f},  // body top
    {-0.65f, 1.25f, 0.4f}, {0.65f, 1.25f, 0.4f}, {0.7f, 1.25f, -1.2f}, {-0.7f, 1.25f, -1.2f},  // roof
    {-0.95f, 0, 1.1f}, {0.95f, 0, 1.1f}, {0.95f, 0, -1.1f}, {-0.95f, 0, -1.1f},               // wheel points
};
const uint8_t CAR_T[22][3] = {
    {4, 5, 9}, {4, 9, 8},           // windscreen
    {7, 11, 10}, {7, 10, 6},         // rear window
    {8, 9, 10}, {8, 10, 11},         // roof
    {4, 8, 11}, {4, 11, 7}, {5, 6, 10}, {5, 10, 9},  // side windows
    {0, 1, 5}, {0, 5, 4},            // nose
    {3, 7, 6}, {3, 6, 2},            // tail
    {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},  // flanks
    {12, 0, 3}, {12, 3, 15}, {13, 2, 1}, {13, 14, 2},  // wheel arches (dark)
};
uint32_t car_colors(int i, uint32_t paint)
{
    if (i >= 18) {
        return 0x141414;  // wheels
    }
    if (i < 4 || (i >= 6 && i < 10)) {
        return 0x24344C;  // glass all round
    }
    return paint;  // the roof and the body
}
const uint32_t PAINTS[4] = {0xE83A2A, 0x2A6AE8, 0xE8C020, 0x30A050};  // yours red; rivals blue, yellow, green
uint32_t CAR_C[4][22];

// a pine: two cones and a trunk
const V3 PINE_V[14] = {
    {0, 4.2f, 0}, {0.9f, 2.2f, 0}, {0.45f, 2.2f, 0.78f}, {-0.45f, 2.2f, 0.78f}, {-0.9f, 2.2f, 0}, {-0.45f, 2.2f, -0.78f}, {0.45f, 2.2f, -0.78f},
    {1.2f, 0.8f, 0}, {0.6f, 0.8f, 1.04f}, {-0.6f, 0.8f, 1.04f}, {-1.2f, 0.8f, 0}, {-0.6f, 0.8f, -1.04f}, {0.6f, 0.8f, -1.04f}, {0, 2.4f, 0},
};
const uint8_t PINE_T[12][3] = {
    {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 6}, {0, 6, 1},
    {13, 7, 8}, {13, 8, 9}, {13, 9, 10}, {13, 10, 11}, {13, 11, 12}, {13, 12, 7},
};
uint32_t PINE_C[3][12];
const uint32_t LEAVES[3][2] = {{0xE8782A, 0xB04A18}, {0xC83A20, 0x8A2410}, {0x3A7A3A, 0x245A28}};
// a rock: a lumpy wedge
const V3 ROCK_V[6] = {{-0.9f, 0, -0.6f}, {0.9f, 0, -0.7f}, {1.0f, 0, 0.6f}, {-0.8f, 0, 0.7f}, {-0.2f, 1.0f, 0.1f}, {0.3f, 0.8f, -0.1f}};
const uint8_t ROCK_T[6][3] = {{0, 1, 5}, {0, 5, 4}, {1, 2, 5}, {2, 3, 4}, {2, 4, 5}, {3, 0, 4}};
const uint32_t ROCK_C[6] = {0x8A8078, 0x7A7068, 0x9A9088, 0x6A6058, 0xA8A098, 0x807870};

struct Rival {
    float s, d, v;
    int paint;
};

class Rally final : public Game {
public:
    Rally()
    {
        for (int p = 0; p < 4; ++p)
            for (int i = 0; i < 22; ++i) {
                CAR_C[p][i] = car_colors(i, PAINTS[p]);
            }
        for (int k = 0; k < 3; ++k)
            for (int i = 0; i < 12; ++i) {
                PINE_C[k][i] = i < 6 ? LEAVES[k][0] : LEAVES[k][1];
            }
    }

    void reset(uint32_t seed, bool demo) override
    {
        rng_ = Rng(seed);
        demo_ = demo, over_ = false, score_ = 0, events_ = 0;
        road_.clear();
        made_ = 0, bend_ = 0, bend_left_ = 0, hill_phase_ = rng_.range(0, TAU);
        add_segment({0, 0, 0}, 0);
        while (int(road_.size()) < AHEAD + 20) {
            grow();
        }
        s_ = 2, d_ = 0, vd_ = 0, v_ = 0, slip_ = 0, time_left_ = 45, clock_ = 0, boost_ = 0, boosts_ = 3, bump_ = 0;
        checkpoints_ = 0, passed_ = 0, metres_ = 0;
        rivals_.clear();
        for (int i = 0; i < 5; ++i) {
            rivals_.push_back({18.0f + float(i) * 40, rng_.range(-0.5f, 0.5f), rng_.range(20, 27), 1 + i % 3});
        }
        dust_.clear();
    }

    void step(float dt, const Input &input) override
    {
        if (over_) {
            return;
        }
        dt = std::min(dt, 0.1f);
        clock_ += dt;
        const Input in = demo_ ? autopilot() : input;
        const Segment &here = seg_at(s_);
        const bool off = fabsf(d_) > 1.05f;
        // throttle and brake: cruising by itself, faster held back, slower pushed
        if (in.action && boosts_ > 0 && boost_ <= 0) {
            --boosts_;
            boost_ = 2.5f;
            events_ |= EV_POWER;
        }
        boost_ = std::max(0.0f, boost_ - dt);
        const float top = (off ? V_OFF : V_ROAD) * (boost_ > 0 ? 1.35f : 1.0f);
        const float want = in.y < -0.15f ? 0 : top * (0.78f + 0.22f * clampf(in.y, 0, 1));
        const float rate = want < v_ ? (in.y < -0.15f ? 28.0f : off ? 22.0f : 6.0f) : 12.0f;
        v_ += clampf(want - v_, -rate * dt, rate * dt);
        // steering on gravel: the car slides, the bend pushes it wide
        const float grip = off ? 2.5f : 4.2f;
        const float push = here.curve / SEG_L * v_ * v_ * 0.11f;
        vd_ += (in.x * 3.6f * clampf(v_ / 18, 0, 1) - push) * dt;
        vd_ *= expf(-dt * grip);
        d_ += vd_ * dt;
        slip_ += (vd_ * 0.18f - slip_) * (1 - expf(-dt * 6));
        if (fabsf(d_) > 2.2f) {  // into the trees: a thump and a stop
            d_ = d_ > 0 ? 2.2f : -2.2f;
            vd_ = -vd_ * 0.3f;
            v_ *= 0.4f;
            if (bump_ <= 0) {
                events_ |= EV_HIT;
                bump_ = 1;
            }
        }
        bump_ = std::max(0.0f, bump_ - dt);
        s_ += v_ * dt / SEG_L;
        metres_ += v_ * dt;
        score_ = int(metres_ * 0.5f) + checkpoints_ * 500 + passed_ * 150;
        if (off && v_ > 6) {
            Segment p = seg_at(s_);
            const V3 at = car_world(p) + V3{0, 0.3f, 0};
            dust_.burst(at, 2, 1.5f, hex(0xC8A070), 0.8f, 0.5f, rng_, {0, 1.2f, 0});
        } else if (v_ > 20 && fabsf(slip_) > 0.2f) {
            const V3 at = car_world(seg_at(s_)) + V3{0, 0.2f, 0};
            dust_.burst(at, 1, 1, hex(0xB08860), 0.6f, 0.4f, rng_, {0, 1, 0});
        }
        dust_.step(dt, {0, 0.6f, 0});
        // rivals: driven, passed, bumped
        for (Rival &r : rivals_) {
            const float before = r.s - s_;
            r.s += r.v * dt / SEG_L;
            r.d += (sinf(r.s * 0.05f) * 0.45f - r.d) * (1 - expf(-dt));
            const float gap = (r.s - s_) * SEG_L;
            if (before > 0 && r.s - s_ <= 0) {
                ++passed_;
                events_ |= EV_PICKUP;
            }
            if (fabsf(gap) < 3.2f && fabsf(r.d - d_) < 0.42f && bump_ <= 0) {
                v_ *= 0.55f;
                vd_ += (d_ > r.d ? 1.5f : -1.5f);
                events_ |= EV_HIT;
                bump_ = 1;
            }
            if (gap < -30) {  // far behind: it comes round again ahead
                r.s = s_ + float(AHEAD) * 0.8f + rng_.range(0, 20);
                r.v = rng_.range(19, 27) + float(checkpoints_);
                r.paint = 1 + rng_.below(3);
            }
        }
        // checkpoints buy time
        const int seg = int(s_);
        if (seg / CHECK_EVERY > checkpoints_) {
            ++checkpoints_;
            time_left_ += std::max(14.0f, 26 - float(checkpoints_) * 1.5f);
            events_ |= EV_LEVEL;
        }
        while (!road_.empty() && road_.front().index < seg - 6) {
            road_.pop_front();
        }
        while (int(road_.size()) < AHEAD + 20) {
            grow();
        }
        time_left_ -= demo_ ? 0 : dt;
        warn_ -= dt;
        if (time_left_ < 8 && warn_ <= 0) {
            events_ |= EV_WARN;
            warn_ = 2;
        }
        if (time_left_ <= 0) {
            time_left_ = 0;
            over_ = true;
            events_ |= EV_OVER;
        }
    }

    void render(uint16_t *fb) override;

    void hud(Hud *h) const override
    {
        h->score = score_;
        h->meter = time_left_ / 60;
        h->meter_color = time_left_ < 8 ? 0xFF5A3C : 0xFFB066;
        h->meter_name = "Time";
        snprintf(h->line, sizeof(h->line), "%d km/h · stage %d · %d %s", int(v_ * 4.2f), checkpoints_ + 1, boosts_, boosts_ == 1 ? "boost" : "boosts");
    }
    const char *ending() const override
    {
        return "Out of time";
    }
    void profile(char *out, int capacity) override
    {
        prof_.report(out, capacity);
    }

private:
    void add_segment(V3 c, float heading)
    {
        Segment g{};
        g.c = c, g.heading = heading, g.curve = 0, g.index = made_++;
        road_.push_back(g);
    }

    // The road goes on: straights and bends of random length and sharpness,
    // rolling hills, pines and rocks beside it.
    void grow()
    {
        Segment &last = road_.back();
        if (bend_left_ <= 0) {
            const bool straight = rng_.f() < 0.35f;
            bend_ = straight ? 0 : rng_.range(0.015f, 0.05f) * (rng_.f() < 0.5f ? -1 : 1) * std::min(1.4f, 1 + float(made_) / 4000);
            bend_left_ = 20 + rng_.below(40);
        }
        --bend_left_;
        last.curve = bend_;
        const float h = last.heading + bend_;
        V3 c = last.c + V3{sinf(last.heading) * SEG_L, 0, cosf(last.heading) * SEG_L};
        const float i = float(made_);
        c.y = 4.5f * sinf(i * 0.021f + hill_phase_) + 2.5f * sinf(i * 0.053f + 1.3f);
        Segment g{};
        g.c = c, g.heading = h, g.index = made_++;
        const uint32_t r = uint32_t(g.index) * 2654435761u;
        g.left = (r >> 3) % 3 == 0 ? uint8_t(1 + ((r >> 9) % 4 == 0)) : 0;
        g.right = (r >> 13) % 3 == 0 ? uint8_t(1 + ((r >> 19) % 4 == 0)) : 0;
        g.lpos = 1.8f + float((r >> 21) % 100) / 100 * 3.5f;
        g.rpos = 1.8f + float((r >> 25) % 100) / 100 * 3.5f;
        road_.push_back(g);
    }

    // The segment at distance s (fractional), its middle and direction blended.
    Segment seg_at(float s) const
    {
        const int base = road_.front().index;
        const int i = std::clamp(int(s) - base, 0, int(road_.size()) - 2);
        const Segment &a = road_[size_t(i)], &b = road_[size_t(i + 1)];
        const float t = clampf(s - float(a.index), 0, 1);
        Segment out = a;
        out.c = a.c + (b.c - a.c) * t;
        out.heading = a.heading + (b.heading - a.heading) * t;
        return out;
    }
    static V3 right_of(float heading)
    {
        return {cosf(heading), 0, -sinf(heading)};
    }
    V3 car_world(const Segment &p) const
    {
        return p.c + right_of(p.heading) * (d_ * HALF);
    }

    // For the title screen: along the middle, braking for sharp bends.
    Input autopilot() const
    {
        Input in;
        in.absolute = true;
        const Segment &ahead = seg_at(s_ + 6);
        in.x = clampf(-d_ * 1.2f + ahead.curve * 22 - vd_ * 0.3f, -1, 1);
        in.y = fabsf(ahead.curve) > 0.04f && v_ > 26 ? -0.5f : 0.6f;
        for (const Rival &r : rivals_) {  // round a rival ahead
            const float gap = (r.s - s_) * SEG_L;
            if (gap > 0 && gap < 14 && fabsf(r.d - d_) < 0.6f) {
                in.x = clampf(in.x + (d_ > r.d ? 0.8f : -0.8f), -1, 1);
            }
        }
        return in;
    }

    Rng rng_;
    std::deque<Segment> road_;
    int made_ = 0, bend_left_ = 0, checkpoints_ = 0, passed_ = 0, boosts_ = 3;
    float bend_ = 0, hill_phase_ = 0;
    float s_ = 2, d_ = 0, vd_ = 0, v_ = 0, slip_ = 0, time_left_ = 45, clock_ = 0, boost_ = 0, bump_ = 0, warn_ = 0, metres_ = 0;
    std::vector<Rival> rivals_;
    Particles dust_;
    Camera cam_;
    Profile prof_{{"sky", "road", "scenery", "cars"}};
};

void Rally::render(uint16_t *fb)
{
    prof_.start();
    const Segment me = seg_at(s_);
    const V3 car = car_world(me), fwd{sinf(me.heading), 0, cosf(me.heading)};
    cam_.focal = 150;
    const float shake = bump_ > 0.7f ? (bump_ - 0.7f) * 0.6f : 0;
    cam_.look_at(car - fwd * 6.6f + V3{sinf(clock_ * 60) * shake, 3.0f, 0}, car + fwd * 8 + V3{0, 0.8f, 0}, -slip_ * 0.25f);
    // the sky at sunset, the sun, a far ridge that turns with the road
    for (int y = 0; y < H; ++y) {
        const float t = clampf(float(y) / (H * 0.55f), 0, 1);
        std::fill_n(fb + y * W, W, mix565(hex(SKY_TOP), hex(HAZE), int(t * t * 32)));
    }
    const float cam_heading = atan2f(cam_.fwd.x, cam_.fwd.z);
    {
        const float rel = angle_diff(cam_heading, 0.9f);
        if (fabsf(rel) < 1.0f) {
            glow(fb, float(W) / 2 + tanf(rel) * cam_.focal, 62, 34, hex(0x7A4020), 0);
            glow(fb, float(W) / 2 + tanf(rel) * cam_.focal, 62, 11, hex(0xFFE8B0), 0.7f);
        }
    }
    for (int x = 0; x < W; ++x) {  // the ridge: a silhouette fixed to the world's directions
        const float a = cam_heading + atanf((float(x) - float(W) / 2) / cam_.focal);
        const float hgt = 18 + 10 * sinf(a * 3.1f) + 6 * sinf(a * 7.3f + 1) + 3 * sinf(a * 17.0f);
        const int top = std::max(0, int(cam_.cy - 6 - hgt * 0.9f));
        for (int y = top; y < int(cam_.cy + 8); ++y) {
            fb[y * W + x] = mix565(hex(0x5A3050), hex(HAZE), 12);
        }
    }
    prof_.mark(0);
    // the road, far to near: grass, verges, gravel and its wheel tracks; then
    // what stands beside each segment
    const int first = std::max(int(s_) - 2, road_.front().index);
    const int last = std::min(int(s_) + AHEAD, road_.back().index - 1);
    for (int idx = last; idx >= first; --idx) {
        const Segment &a = road_[size_t(idx - road_.front().index)], &b = road_[size_t(idx + 1 - road_.front().index)];
        const V3 ra = right_of(a.heading), rb = right_of(b.heading);
        const float dist = float(idx) - s_;
        const int fog = int(clampf(dist / float(AHEAD), 0, 1) * 26);
        const bool stripe = (idx / 3) & 1;
        const auto band = [&](float w0, float w1, uint32_t colour) {
            const V3 q[4] = {a.c + ra * w0, a.c + ra * w1, b.c + rb * w1, b.c + rb * w0};
            cam_.poly_world(fb, q, 4, mix565(hex(colour), hex(HAZE), fog));
        };
        band(-40, -HALF * 1.12f, stripe ? 0x5A7A30 : 0x4E6E2A);
        band(HALF * 1.12f, 40, stripe ? 0x5A7A30 : 0x4E6E2A);
        band(-HALF * 1.12f, -HALF, stripe ? 0xC8B490 : 0x8A7458);
        band(HALF, HALF * 1.12f, stripe ? 0xC8B490 : 0x8A7458);
        band(-HALF, HALF, stripe ? 0xA0805A : 0x987852);
        band(-HALF * 0.62f, -HALF * 0.38f, 0x806040);
        band(HALF * 0.38f, HALF * 0.62f, 0x806040);
        if (idx % CHECK_EVERY == 0 && idx > 0) {  // a checkpoint arch
            const V3 l = a.c - ra * (HALF * 1.2f), r = a.c + ra * (HALF * 1.2f);
            cam_.edge_world(fb, l, l + V3{0, 3.4f, 0}, hex(0xF0F0F0));
            cam_.edge_world(fb, r, r + V3{0, 3.4f, 0}, hex(0xF0F0F0));
            const V3 banner[4] = {l + V3{0, 3.4f, 0}, r + V3{0, 3.4f, 0}, r + V3{0, 2.6f, 0}, l + V3{0, 2.6f, 0}};
            cam_.poly_world(fb, banner, 4, mix565(hex(0xE8301E), hex(HAZE), fog));
        }
        if (dist < 50) {
            for (int side = 0; side < 2; ++side) {
                const uint8_t what = side ? a.right : a.left;
                if (!what) {
                    continue;
                }
                const float out = (side ? a.rpos : -a.lpos) * HALF;
                Pose pose;
                pose.pos = a.c + ra * out;
                pose.yaw = float(a.index) * 0.7f;
                MeshStyle style;
                style.ambient = 0.5f;
                if (what == 1) {
                    pose.scale = 1.0f + float(a.index % 5) * 0.12f;
                    const Mesh pine = {PINE_V, 14, PINE_T, PINE_C[a.index % 3], 12};
                    draw_mesh(fb, cam_, pine, pose, style);
                    cam_.edge_world(fb, pose.pos, pose.pos + V3{0, 0.8f * pose.scale, 0}, hex(0x4A2A18));
                } else {
                    pose.scale = 0.8f + float(a.index % 3) * 0.3f;
                    const Mesh rock = {ROCK_V, 6, ROCK_T, ROCK_C, 6};
                    draw_mesh(fb, cam_, rock, pose, style);
                }
            }
        }
        // rivals on this segment
        for (const Rival &r : rivals_) {
            if (int(r.s) != idx) {
                continue;
            }
            const Segment p = seg_at(r.s);
            Pose pose;
            pose.pos = p.c + right_of(p.heading) * (r.d * HALF);
            pose.yaw = p.heading;
            const Mesh rival = {CAR_V, 16, CAR_T, CAR_C[r.paint], 22};
            MeshStyle style;
            style.ambient = 0.45f;
            draw_mesh(fb, cam_, rival, pose, style);
        }
    }
    prof_.mark(1);
    dust_.draw(fb, cam_);
    prof_.mark(2);
    // the car, sliding a little
    Pose pose;
    pose.pos = car;
    pose.yaw = me.heading + slip_;
    pose.roll = -slip_ * 0.4f;
    const Mesh mine = {CAR_V, 16, CAR_T, CAR_C[0], 22};
    MeshStyle style;
    style.ambient = 0.45f;
    draw_mesh(fb, cam_, mine, pose, style);
    // tail lights; the boost's flame
    cam_.glow_at(fb, pose_point(pose, {-0.6f, 0.55f, -1.75f}), 0.18f, hex(0xFF2010), 0.5f, 4);
    cam_.glow_at(fb, pose_point(pose, {0.6f, 0.55f, -1.75f}), 0.18f, hex(0xFF2010), 0.5f, 4);
    if (boost_ > 0) {
        const float f = 0.7f + 0.3f * sinf(clock_ * 70);
        cam_.glow_at(fb, pose_point(pose, {0, 0.35f, -1.95f}), 0.6f * f, hex(0x3AA0FF), 0.2f, 12);
    }
    prof_.mark(3);
}

}  // namespace

std::unique_ptr<Game> make_rally()
{
    return std::make_unique<Rally>();
}

}  // namespace games
