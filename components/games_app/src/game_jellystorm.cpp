// SPDX-License-Identifier: Apache-2.0
// Jellystorm: a wave shooter over a neon grid. Glowing space jellies drift in
// in formations, swoop at the ship and drop spores; the ship fires by itself,
// grows its guns with pearls, and a pulse bomb clears the air. Every fourth
// wave a mother jelly comes. Drawn with the shared engine (games_3d.h).
#include <stdio.h>
#include <string.h>

#include <vector>

#include "games_3d.h"

namespace games {
namespace {

constexpr float FIELD = 5.0f, SHIP_Z = 0.8f, FLY = 0.6f, TOP = 26;  // half the field's width; the ship's depth; flight height
constexpr uint32_t SPACE = 0x080418;

// a jelly's bell: a crown, a shoulder ring and a wider rim
const V3 BELL_V[13] = {
    {0, 0.62f, 0},
    {0.42f, 0.42f, 0}, {0.21f, 0.42f, 0.36f}, {-0.21f, 0.42f, 0.36f}, {-0.42f, 0.42f, 0}, {-0.21f, 0.42f, -0.36f}, {0.21f, 0.42f, -0.36f},
    {0.62f, 0, 0}, {0.31f, 0, 0.54f}, {-0.31f, 0, 0.54f}, {-0.62f, 0, 0}, {-0.31f, 0, -0.54f}, {0.31f, 0, -0.54f},
};
const uint8_t BELL_T[18][3] = {
    {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 6}, {0, 6, 1},
    {1, 7, 8}, {1, 8, 2}, {2, 8, 9}, {2, 9, 3}, {3, 9, 10}, {3, 10, 4}, {4, 10, 11}, {4, 11, 5}, {5, 11, 12}, {5, 12, 6}, {6, 12, 7}, {6, 7, 1},
};
// four kinds by row: rose, aqua, lime, violet (the crown lighter than the rim)
uint32_t BELL_C[4][18];
const uint32_t KIND_HI[4] = {0xFFA8D8, 0x9AF4FF, 0xC8FF9A, 0xD0A8FF};
const uint32_t KIND_LO[4] = {0xC03A86, 0x2AA0C8, 0x58B830, 0x7A40D8};
const uint32_t KIND_GLOW[4] = {0xFF6EC7, 0x46E6FF, 0x9CFF5A, 0xB080FF};

// the ship: a little arrowhead fighter
const V3 SHIP_V[7] = {{0, 0, 0.9f}, {-0.7f, 0, -0.45f}, {0.7f, 0, -0.45f}, {0, 0.28f, -0.2f}, {0, -0.12f, -0.3f}, {-0.25f, 0.05f, -0.55f}, {0.25f, 0.05f, -0.55f}};
const uint8_t SHIP_T[8][3] = {{0, 1, 3}, {0, 3, 2}, {1, 5, 3}, {3, 6, 2}, {5, 6, 3}, {0, 4, 1}, {0, 2, 4}, {1, 4, 2}};
const uint32_t SHIP_C[8] = {0xE8F0FF, 0xC8D4F0, 0x9AA8D0, 0x9AA8D0, 0x6A78A8, 0x3A4468, 0x3A4468, 0x2A3050};

struct Jelly {
    float x, z, sx, sz, t;  // where it is; its slot in the formation; its swim phase
    int hp, kind;
    bool alive, diving;
    float dive_t, dive_x0;
};
struct Shot {
    float x, z, vx;
};
struct Spore {
    float x, z, vx, vz;
};
struct Pickup {
    float x, z;
    int kind;  // 0 a pearl (more guns), 1 a star (a bomb)
};

class Jellystorm final : public Game {
public:
    Jellystorm()
    {
        for (int k = 0; k < 4; ++k)
            for (int t = 0; t < 18; ++t) {
                BELL_C[k][t] = t < 6 ? KIND_HI[k] : KIND_LO[k];
            }
        Rng r(77);
        for (auto &s : stars_) {
            s = {r.range(0, W), r.range(0, H * 0.55f), r.range(0.3f, 1)};
        }
    }

    void reset(uint32_t seed, bool demo) override
    {
        rng_ = Rng(seed);
        demo_ = demo, over_ = false, score_ = 0, events_ = 0;
        x_ = 0, vx_ = 0, lives_ = 3, bombs_ = 2, power_ = 1, wave_ = 0, hurt_ = 0, flash_ = 0, time_ = 0;
        fire_ = 0, next_wave_ = 1.0f, dive_wait_ = 3, spore_wait_ = 2, boss_hp_ = boss_max_ = 0;
        jellies_.clear(), shots_.clear(), spores_.clear(), pickups_.clear();
        sparks_.clear();
    }

    void step(float dt, const Input &input) override
    {
        if (over_) {
            return;
        }
        dt = std::min(dt, 0.1f);
        time_ += dt, wave_time_ += dt;
        const Input in = demo_ ? autopilot() : input;
        // the ship: a tilt places it, a drag slides it
        if (in.absolute) {
            const float target = in.x * FIELD, k = 1 - expf(-dt * 8);
            const float nx = x_ + (target - x_) * k;
            vx_ = (nx - x_) / std::max(dt, 0.001f), x_ = nx;
        } else {
            vx_ += (in.x * 10 - vx_) * (1 - expf(-dt * 10));
            x_ += vx_ * dt;
        }
        x_ = clampf(x_, -FIELD, FIELD);
        hurt_ = std::max(0.0f, hurt_ - dt), flash_ = std::max(0.0f, flash_ - dt);
        // guns: one, two side by side, or three spread
        fire_ -= dt;
        if (fire_ <= 0) {
            fire_ = 0.16f;
            if (power_ == 1) {
                shots_.push_back({x_, SHIP_Z + 0.6f, 0});
            } else if (power_ == 2) {
                shots_.push_back({x_ - 0.3f, SHIP_Z + 0.5f, 0}), shots_.push_back({x_ + 0.3f, SHIP_Z + 0.5f, 0});
            } else {
                shots_.push_back({x_, SHIP_Z + 0.6f, 0}), shots_.push_back({x_ - 0.2f, SHIP_Z + 0.5f, -3}),
                                 shots_.push_back({x_ + 0.2f, SHIP_Z + 0.5f, 3});
            }
        }
        // the pulse bomb
        if (in.action && bombs_ > 0) {
            --bombs_;
            flash_ = 0.5f;
            events_ |= EV_POWER;
            spores_.clear();
            for (Jelly &j : jellies_) {
                if (j.alive && j.z < TOP) {
                    j.hp -= j.kind == 9 ? 12 : 2;
                    if (j.hp <= 0) {
                        kill(j);
                    }
                }
            }
        }
        if (jellies_.empty() || none_alive()) {
            next_wave_ -= dt;
            if (next_wave_ <= 0) {
                start_wave();
            }
        }
        swim(dt);
        // shots
        for (Shot &s : shots_) {
            s.z += 26 * dt, s.x += s.vx * dt;
        }
        for (Shot &s : shots_) {
            for (Jelly &j : jellies_) {
                const float rx = j.kind == 9 ? 2.0f : 0.7f, rz = j.kind == 9 ? 1.5f : 0.65f;
                if (!j.alive || fabsf(s.x - j.x) > rx || fabsf(s.z - j.z) > rz) {
                    continue;
                }
                s.z = 99;  // spent
                if (--j.hp <= 0) {
                    kill(j);
                } else {
                    sparks_.burst({s.x, FLY, s.z}, 3, 3, hex(KIND_GLOW[j.kind == 9 ? 0 : j.kind]), 0.3f, 0.12f, rng_);
                }
                break;
            }
        }
        shots_.erase(std::remove_if(shots_.begin(), shots_.end(), [](const Shot & s) {
            return s.z > TOP + 2 || fabsf(s.x) > FIELD + 2;
        }), shots_.end());
        // spores and divers against the ship
        for (Spore &p : spores_) {
            p.x += p.vx * dt, p.z += p.vz * dt;
            if (fabsf(p.x - x_) < 0.45f && fabsf(p.z - SHIP_Z) < 0.45f) {
                p.z = -99;
                hit();
            }
        }
        spores_.erase(std::remove_if(spores_.begin(), spores_.end(), [](const Spore & p) {
            return p.z < -3;
        }), spores_.end());
        for (Jelly &j : jellies_) {
            if (j.alive && j.diving && fabsf(j.x - x_) < 0.7f && fabsf(j.z - SHIP_Z) < 0.6f) {
                j.hp = 0;
                kill(j);
                hit();
            }
        }
        // pickups drift down to the ship
        for (Pickup &k : pickups_) {
            k.z -= 4.5f * dt;
            if (fabsf(k.x - x_) < 0.75f && fabsf(k.z - SHIP_Z) < 0.75f) {
                k.z = -99;
                score_ += 100;
                events_ |= EV_PICKUP;
                if (k.kind == 0) {
                    power_ = std::min(3, power_ + 1);
                } else {
                    bombs_ = std::min(4, bombs_ + 1);
                }
            }
        }
        pickups_.erase(std::remove_if(pickups_.begin(), pickups_.end(), [](const Pickup & k) {
            return k.z < -3;
        }), pickups_.end());
        sparks_.step(dt, {0, -2.5f, 0});
        if (lives_ <= 0) {
            if (demo_) {
                reset(rng_.next(), true);
                return;
            }
            over_ = true;
            events_ |= EV_OVER;
        }
    }

    void render(uint16_t *fb) override;

    void hud(Hud *h) const override
    {
        h->score = score_;
        h->meter = float(lives_) / 3;
        h->meter_color = lives_ <= 1 ? 0xFF5A6A : 0xFF6EC7;
        h->meter_name = "Shield";
        snprintf(h->line, sizeof(h->line), "Wave %d · %d %s · %s guns", std::max(1, wave_), bombs_, bombs_ == 1 ? "bomb" : "bombs",
                 power_ == 1 ? "single" : power_ == 2 ? "twin" : "triple");
    }
    const char *ending() const override
    {
        return "The swarm got through";
    }
    void profile(char *out, int capacity) override
    {
        prof_.report(out, capacity);
    }

private:
    bool none_alive() const
    {
        for (const Jelly &j : jellies_) {
            if (j.alive) {
                return false;
            }
        }
        return true;
    }

    void start_wave()
    {
        ++wave_;
        wave_time_ = 0;
        jellies_.clear();
        next_wave_ = 1.8f;
        if (wave_ % 4 == 0) {  // a mother jelly
            boss_max_ = boss_hp_ = 40 + 15 * (wave_ / 4);
            jellies_.push_back({0, TOP + 4, 0, 13, 0, boss_hp_, 9, true, false, 0, 0});
            return;
        }
        boss_max_ = 0;
        const int rows = std::min(5, 2 + wave_ / 2), cols = std::min(7, 4 + wave_ / 2);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const float sx = (float(c) - float(cols - 1) / 2) * 1.4f, sz = 9 + float(r) * 1.45f;
                const int kind = (rows - 1 - r) % 4;
                // they swim in from far off, a row at a time
                jellies_.push_back({sx, sz + 14 + float(r) * 2, sx, sz, rng_.range(0, TAU), r >= 3 ? 2 : 1, kind, true, false, 0, 0});
            }
        dive_wait_ = std::max(0.8f, 3.2f - 0.25f * float(wave_));
    }

    void swim(float dt)
    {
        const float sway = sinf(time_ * 0.6f) * 1.4f, sink = std::min(4.0f, time_since_wave() * 0.06f);
        Jelly *boss = nullptr;
        for (Jelly &j : jellies_) {
            if (!j.alive) {
                continue;
            }
            j.t += dt;
            if (j.kind == 9) {
                boss = &j;
                j.x = sinf(time_ * 0.7f) * 3.6f;
                j.z += (j.sz - j.z) * (1 - expf(-dt * 1.5f));
                continue;
            }
            if (j.diving) {  // a swoop at the ship, then round to its slot from behind
                j.dive_t += dt;
                j.z -= (6.5f + 0.3f * float(wave_)) * dt;
                j.x = j.dive_x0 + (x_ - j.dive_x0) * std::min(1.0f, j.dive_t * 0.5f) + sinf(j.dive_t * 4) * 1.2f;
                if (j.z < -2) {
                    j.diving = false;
                    j.z = TOP + 2;
                }
                continue;
            }
            const float tx = j.sx + sway, tz = j.sz - sink;
            const float k = 1 - expf(-dt * 2);
            j.x += (tx - j.x) * k;
            j.z += (tz - j.z) * k;
        }
        // now and then one swoops; spores fall
        dive_wait_ -= dt;
        if (dive_wait_ <= 0 && !boss) {
            dive_wait_ = std::max(0.7f, 3.0f - 0.22f * float(wave_)) * rng_.range(0.6f, 1.3f);
            Jelly *pick = random_alive();
            if (pick && pick->z < TOP - 6) {
                pick->diving = true, pick->dive_t = 0, pick->dive_x0 = pick->x;
            }
        }
        spore_wait_ -= dt;
        if (spore_wait_ <= 0) {
            if (boss) {  // a fan
                spore_wait_ = std::max(0.7f, 1.4f - 0.05f * float(wave_));
                for (int i = -2; i <= 2; ++i) {
                    spores_.push_back({boss->x, boss->z - 1, float(i) * 1.4f, -7.5f});
                }
            } else {
                spore_wait_ = 1.0f / (0.5f + 0.18f * float(wave_)) * rng_.range(0.5f, 1.5f);
                Jelly *from = random_alive();
                if (from && from->z < TOP - 4) {
                    const float vz = -(6.5f + 0.35f * float(wave_)), t = (from->z - SHIP_Z) / -vz;
                    spores_.push_back({from->x, from->z, (x_ - from->x) / std::max(t, 0.5f) * 0.5f, vz});
                }
            }
        }
        boss_hp_ = boss ? boss->hp : 0;
    }

    float time_since_wave() const
    {
        return wave_time_;  // the formation sinks slowly through a wave (capped)
    }

    Jelly *random_alive()
    {
        int count = 0;
        for (const Jelly &j : jellies_) {
            count += j.alive && !j.diving;
        }
        if (!count) {
            return nullptr;
        }
        int pick = rng_.below(count);
        for (Jelly &j : jellies_) {
            if (j.alive && !j.diving && pick-- == 0) {
                return &j;
            }
        }
        return nullptr;
    }

    void kill(Jelly &j)
    {
        j.alive = false;
        const bool boss = j.kind == 9;
        score_ += boss ? 2000 * (wave_ / 4) : (j.diving ? 100 : 50) * (1 + j.kind);
        events_ |= boss ? EV_LEVEL : EV_PICKUP;
        sparks_.burst({j.x, FLY + 0.3f, j.z}, boss ? 60 : 14, boss ? 9 : 5, hex(KIND_GLOW[boss ? 0 : j.kind]), boss ? 1.2f : 0.6f,
                      boss ? 0.3f : 0.16f, rng_);
        const float roll = rng_.f();
        if (roll < 0.09f || boss) {
            pickups_.push_back({j.x, j.z, 0});
        } else if (roll < 0.13f) {
            pickups_.push_back({j.x, j.z, 1});
        }
        if (none_alive()) {
            score_ += 300 * wave_;
            events_ |= EV_LEVEL;
        }
    }

    void hit()
    {
        if (hurt_ > 0) {
            return;
        }
        lives_ -= demo_ ? 0 : 1;
        hurt_ = 1.6f;
        power_ = std::max(1, power_ - 1);
        events_ |= EV_HIT;
        sparks_.burst({x_, FLY, SHIP_Z}, 24, 6, hex(0xFFD0A0), 0.7f, 0.15f, rng_);
    }

    // For the title screen: under the lowest jelly, out of the way of spores, a bomb when crowded.
    Input autopilot()
    {
        Input in;
        in.absolute = true;
        float goal = 0, best = 1e9f;
        for (const Jelly &j : jellies_) {
            if (j.alive && j.z < best) {
                best = j.z, goal = j.x;
            }
        }
        int near = 0;
        for (const Spore &p : spores_) {
            if (p.z < 6 && p.z > 0) {
                ++near;
                if (fabsf(p.x - goal) < 0.9f) {
                    goal += p.x > goal ? -1.6f : 1.6f;
                }
            }
        }
        in.x = clampf(goal / FIELD, -1, 1);
        in.action = near > 5;
        return in;
    }

    Rng rng_;
    float x_ = 0, vx_ = 0, hurt_ = 0, flash_ = 0, time_ = 0, wave_time_ = 0, fire_ = 0, next_wave_ = 1, dive_wait_ = 3, spore_wait_ = 2;
    int lives_ = 3, bombs_ = 2, power_ = 1, wave_ = 0, boss_hp_ = 0, boss_max_ = 0;
    std::vector<Jelly> jellies_;
    std::vector<Shot> shots_;
    std::vector<Spore> spores_;
    std::vector<Pickup> pickups_;
    Particles sparks_;
    struct Star {
        float x, y, b;
    } stars_[60];
    Camera cam_;
    Profile prof_{{"sky", "grid", "swarm", "ship"}};
};

void Jellystorm::render(uint16_t *fb)
{
    prof_.start();
    cam_.focal = 285;  // high and narrow: a near top-down view with a little depth
    cam_.cy = float(H) / 2 + 6;
    cam_.look_at({x_ * 0.1f, 15, -4.5f}, {x_ * 0.08f, 0, 7.5f});
    // deep space: a gradient and slow, twinkling stars
    for (int y = 0; y < H; ++y) {
        const uint16_t c = mix565(hex(SPACE), hex(0x2A0A3A), std::min(32, y * 32 / H));
        std::fill_n(fb + y * W, W, c);
    }
    for (int i = 0; i < 60; ++i) {
        Star &s = stars_[i];
        const float y = fmodf(s.y + time_ * 6 * s.b, H * 0.55f);
        const float tw = 0.6f + 0.4f * sinf(time_ * 3 + float(i));
        const int px = int(s.x), py = int(y);
        fb[py * W + px] = add565(fb[py * W + px], scale565(hex(0xFFFFFF), int(32 * s.b * tw)));
    }
    if (flash_ > 0) {
        const int k = int(flash_ / 0.5f * 20);
        for (int i = 0; i < W * H; ++i) {
            fb[i] = mix565(fb[i], hex(0xFFE0FF), k);
        }
    }
    prof_.mark(0);
    // the neon grid, rolling towards the ship
    const uint16_t bg = hex(0x16082A);
    for (float gx = -7.2f; gx <= 7.3f; gx += 1.2f) {
        cam_.edge_world(fb, {gx, 0, -2}, {gx, 0, TOP}, mix565(hex(0xB03CFF), bg, 14));
    }
    const float roll = fmodf(time_ * 3, 1.6f);
    for (float gz = -2 - roll + 1.6f; gz < TOP; gz += 1.6f) {
        const int fade = int(clampf(gz / TOP, 0, 1) * 26);
        cam_.edge_world(fb, {-7.2f, 0, gz}, {7.2f, 0, gz}, mix565(hex(0xFF3FD0), bg, fade));
    }
    prof_.mark(1);
    // the swarm, far to near; spores, pearls, shots
    std::vector<const Jelly *> order;
    order.reserve(jellies_.size());
    for (const Jelly &j : jellies_) {
        if (j.alive && j.z < TOP + 3) {
            order.push_back(&j);
        }
    }
    std::sort(order.begin(), order.end(), [](const Jelly * a, const Jelly * b) {
        return a->z > b->z;
    });
    for (const Jelly *j : order) {
        const bool boss = j->kind == 9;
        const int kind = boss ? 0 : j->kind;
        const float pulse = 1 + 0.08f * sinf(j->t * 5);
        Pose pose;
        pose.pos = {j->x, FLY + 0.15f * sinf(j->t * 2.2f), j->z};
        pose.scale = (boss ? 3.2f : 1.3f) * pulse;
        pose.yaw = j->t * 0.6f;
        const Mesh bell = {BELL_V, 13, BELL_T, BELL_C[kind], 18};
        MeshStyle style;
        style.ambient = 0.55f;
        draw_mesh(fb, cam_, bell, pose, style);
        // tentacles that trail and wave
        const uint16_t tc = mix565(hex(KIND_GLOW[kind]), bg, int(clampf(j->z / TOP, 0, 1) * 14));
        const float s = pose.scale;
        for (int k = 0; k < 4; ++k) {
            const float a = float(k) * TAU / 4 + j->t * 0.6f;
            V3 p = pose.pos + V3{cosf(a) * 0.4f * s, 0, sinf(a) * 0.4f * s};
            for (int seg = 0; seg < 3; ++seg) {
                const V3 q = p + V3{sinf(j->t * 4 + float(seg + k)) * 0.12f * s, -0.32f * s, 0};
                cam_.edge_world(fb, p, q, tc);
                p = q;
            }
        }
        cam_.glow_at(fb, pose.pos + V3{0, 0.3f * s, 0}, 0.9f * s, scale565(hex(KIND_GLOW[kind]), 12), 0, 30);
    }
    for (const Pickup &k : pickups_) {
        const V3 p{k.x, FLY, k.z};
        const float b = 0.8f + 0.2f * sinf(time_ * 8);
        cam_.glow_at(fb, p, 0.55f, scale565(hex(k.kind == 0 ? 0x9AF4FF : 0xFFE070), int(20 * b)), 0, 16);
        cam_.glow_at(fb, p, 0.2f, hex(0xFFFFFF), 0.6f, 6);
    }
    for (const Spore &p : spores_) {
        const V3 at{p.x, FLY, p.z};
        cam_.glow_at(fb, at, 0.32f, hex(0x7AFF5A), 0.2f, 10);
        cam_.glow_at(fb, at, 0.1f, hex(0xE8FFD0), 0.7f, 3);
    }
    for (const Shot &s : shots_) {
        cam_.edge_world(fb, {s.x, FLY, s.z}, {s.x - s.vx * 0.02f, FLY, s.z - 0.7f}, hex(0xFFF0A0));
        cam_.glow_at(fb, {s.x, FLY, s.z}, 0.14f, hex(0xFFD040), 0.5f, 4);
    }
    sparks_.draw(fb, cam_);
    prof_.mark(2);
    // the ship (blinking while it recovers), its engine
    if (!(hurt_ > 0 && fmodf(hurt_, 0.2f) < 0.08f)) {
        Pose pose;
        pose.pos = {x_, FLY, SHIP_Z};
        pose.roll = clampf(-vx_ * 0.05f, -0.6f, 0.6f), pose.scale = 0.85f;
        MeshStyle style;
        style.outline = true, style.trim = hex(0xFFFFFF), style.trim2 = hex(0xFF6EC7), style.split = 2;
        const Mesh ship = {SHIP_V, 7, SHIP_T, SHIP_C, 8};
        draw_mesh(fb, cam_, ship, pose, style);
        const float flick = 0.8f + 0.2f * sinf(time_ * 50);
        cam_.glow_at(fb, pose_point(pose, {0, 0.05f, -0.6f}), 0.5f * flick, hex(0x3AB4FF), 0, 10);
    }
    // the mother jelly's strength
    if (boss_max_ > 0 && boss_hp_ > 0) {
        const int w = 120 * boss_hp_ / boss_max_;
        for (int y = 46; y < 49; ++y) {
            std::fill_n(fb + y * W + 60, 120, hex(0x301028));
            std::fill_n(fb + y * W + 60, w, hex(0xFF6EC7));
        }
    }
    prof_.mark(3);
}

}  // namespace

std::unique_ptr<Game> make_jellystorm()
{
    return std::make_unique<Jellystorm>();
}

}  // namespace games
