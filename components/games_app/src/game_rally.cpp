// SPDX-License-Identifier: Apache-2.0
// Ridge Rally: a gravel stage through autumn pines at sunset, against the
// clock. The road is made as you drive (curves and hills), the car slides on
// gravel and slows off the road, and rivals are there to pass. Every
// checkpoint starts a new stage: less time added, more and sharper bends,
// then puddles that slide the car, rocks and fallen logs. Driving off the
// road and every knock damage the car (a checkpoint mends a little); a wreck,
// or the clock running out, ends the run. A shake (or a tap) is a short
// boost. Everything is drawn with the shared engine: the road's quads, the
// pines, rocks, logs, arches and cars as meshes, the dust as particles.
#include <stdarg.h>
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
constexpr float CAR = 0.27f;                // the car's half width, in road half widths
constexpr float CARRY = 8;                  // seconds the clock carries past a checkpoint, at most
constexpr float CLOCK_FULL = 33;            // the time meter's full scale
constexpr uint32_t HAZE = 0xE8A07A, SKY_TOP = 0x3A2A6A;

enum Hazard : uint8_t { NONE, PUDDLE, ROCK, LOG };

// What a stage asks of the driver. Stage 1 is the gentle start; the curve is
// tuned (games_sim) so a good driver lasts a few minutes, not for ever.
struct Tuning {
    float straight;          // the chance a stretch is straight
    float bend_lo, bend_hi;  // a bend's sharpness (heading change a segment)
    float hairpin;           // the chance a bend is a hairpin
    int len_lo, len_hi;      // a stretch's length, in segments
    float hazard_gap;        // segments from one hazard to the next, on average (0: none)
    int kinds;               // puddles; and rocks; and logs
    float allowance;         // seconds a checkpoint adds
    float rival_v;           // the rivals' best speed
};

Tuning tuning(int stage)
{
    const float d = float(std::clamp(stage, 1, 12) - 1);
    Tuning t;
    t.straight = std::max(0.12f, 0.38f - 0.03f * d);
    t.bend_lo = 0.015f + 0.002f * d, t.bend_hi = std::min(0.085f, 0.045f + 0.005f * d);
    t.hairpin = d < 3 ? 0 : std::min(0.3f, 0.1f + 0.03f * (d - 3));
    t.len_lo = std::max(10, 20 - int(d)), t.len_hi = std::max(30, 60 - 3 * int(d));
    t.hazard_gap = d < 1 ? 0 : std::max(16.0f, 70 - 8 * (d - 1));
    t.kinds = std::min(3, int(d));
    t.allowance = std::max(15.0f, 25 - 1.2f * (d - 1));
    t.rival_v = std::min(30.0f, 25 + 0.5f * d);
    return t;
}

// What a new stage brings, for the HUD.
const char *stage_news(int stage)
{
    switch (stage) {
    case 2:
        return "puddles";
    case 3:
        return "rocks on the road";
    case 4:
        return "fallen logs, hairpins";
    default:
        return "less time, tighter bends";
    }
}

struct Segment {
    V3 c;          // the road's middle at the segment's start
    float heading;  // its direction (radians, 0 = +z)
    float curve;    // heading change across it
    int index;      // from the stage's start
    uint8_t left, right;  // what stands beside it: 0 nothing, 1 a pine, 2 a rock
    float lpos, rpos;     // how far out (road half-widths)
    Hazard hazard;        // what lies on it
    bool struck;          // hit or passed: it counts once
    float hpos;           // where across (road half-widths; a log: the side it lies on)
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
constexpr float ROCK_SCALE = 0.75f, ROCK_R = 0.21f;  // a rock on the road, and its half width (road half widths)
// a fallen log across half the road: a long box, its cut ends pale
constexpr float LOG_LEN = 1.3f;  // from the middle of the road past its edge (road half widths)
const V3 LOG_V[8] = {
    {-2.08f, 0, -0.3f}, {2.08f, 0, -0.3f}, {2.08f, 0, 0.3f}, {-2.08f, 0, 0.3f},
    {-2.08f, 0.55f, -0.3f}, {2.08f, 0.55f, -0.3f}, {2.08f, 0.55f, 0.3f}, {-2.08f, 0.55f, 0.3f},
};
const uint8_t LOG_T[12][3] = {
    {4, 5, 6}, {4, 6, 7},  // top
    {0, 1, 5}, {0, 5, 4}, {3, 7, 6}, {3, 6, 2},  // sides
    {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},  // the cut ends
    {0, 3, 2}, {0, 2, 1},  // underneath
};
const uint32_t LOG_C[12] = {0x9A6A3A, 0x9A6A3A, 0x7A4E28, 0x7A4E28, 0x7A4E28, 0x7A4E28,
                            0xE0B47A, 0xE0B47A, 0xE0B47A, 0xE0B47A, 0x3A2414, 0x3A2414
                           };

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
        fx_rng_ = Rng(seed ^ 0x5A5Au);
        demo_ = demo, over_ = false, score_ = 0, events_ = 0;
        road_.clear();
        made_ = 0, bend_ = 0, bend_left_ = 0, bend_side_ = 1, next_hazard_ = 0, puddle_left_ = 0;
        hill_phase_ = rng_.range(0, TAU);
        add_segment({0, 0, 0}, 0);
        while (int(road_.size()) < AHEAD + 20) {
            grow();
        }
        stage_ = 1, tune_ = tuning(1);
        s_ = 2, d_ = 0, vd_ = 0, v_ = 0, slip_ = 0, time_left_ = 28, clock_ = 0, boost_ = 0, boosts_ = 3, bump_ = 0;
        slide_ = 0, damage_ = 0, smoke_ = 0, points_ = 0, callout_t_ = 0;
        checkpoints_ = 0, passed_ = 0, clean_ = true;
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
        callout_t_ = std::max(0.0f, callout_t_ - dt);
        const Input in = demo_ || bot_ ? autopilot() : input;
        const Segment &here = seg_at(s_);
        const bool off = fabsf(d_) > 1.05f;
        // throttle and brake: cruising by itself, faster held back, slower
        // pushed; a damaged car is slower
        if (in.action && boosts_ > 0 && boost_ <= 0) {
            --boosts_;
            boost_ = 2.5f;
            events_ |= EV_POWER;
        }
        boost_ = std::max(0.0f, boost_ - dt);
        const float top = (off ? V_OFF : V_ROAD) * (boost_ > 0 ? 1.35f : 1.0f) * (1 - 0.18f * damage_);
        const float want = in.y < -0.15f ? 0 : top * (0.78f + 0.22f * clampf(in.y, 0, 1));
        const float rate = want < v_ ? (in.y < -0.15f ? 28.0f : off ? 22.0f : 6.0f) : 12.0f;
        v_ += clampf(want - v_, -rate * dt, rate * dt);
        // steering on gravel: the car slides, the bend pushes it wide, a puddle more so
        slide_ = std::max(0.0f, slide_ - dt);
        const float grip = off ? 2.5f : slide_ > 0 ? 1.4f : 4.2f;
        const float push = here.curve / SEG_L * v_ * v_ * 0.11f;
        vd_ += (in.x * 3.6f * clampf(v_ / 18, 0, 1) - push) * dt;
        vd_ *= expf(-dt * grip);
        d_ += vd_ * dt;
        slip_ += (vd_ * 0.18f - slip_) * (1 - expf(-dt * 6));
        if (fabsf(d_) > 2.2f) {  // into the trees: a thump, a stop and a dent
            d_ = d_ > 0 ? 2.2f : -2.2f;
            vd_ = -vd_ * 0.3f;
            if (bump_ <= 0) {
                hurt(0.06f + 0.006f * v_);
                bump_ = 1;
            }
            v_ *= 0.4f;
        }
        if (off && v_ > 4) {  // rough ground: the car takes a beating
            hurt(0.035f * clampf(v_ / V_OFF, 0, 1) * dt, false);
        }
        bump_ = std::max(0.0f, bump_ - dt);
        const float s0 = s_;
        s_ += v_ * dt / SEG_L;
        hazards(s0);
        // points: distance, more each stage; a slide through a bend
        points_ += v_ * dt * 0.5f * (1 + 0.25f * float(stage_ - 1));
        if (!off && v_ > 22 && fabsf(slip_) > 0.22f) {
            points_ += 40.0f * float(stage_) * dt;
        }
        const int whole = int(points_);
        score_ += whole, points_ -= float(whole);
        if (off && v_ > 6) {
            Segment p = seg_at(s_);
            const V3 at = car_world(p) + V3{0, 0.3f, 0};
            dust_.burst(at, 2, 1.5f, hex(0xC8A070), 0.8f, 0.5f, fx_rng_, {0, 1.2f, 0});
        } else if (v_ > 20 && fabsf(slip_) > 0.2f) {
            const V3 at = car_world(seg_at(s_)) + V3{0, 0.2f, 0};
            dust_.burst(at, 1, 1, hex(0xB08860), 0.6f, 0.4f, fx_rng_, {0, 1, 0});
        }
        smoke_ -= dt;
        if (damage_ > 0.5f && smoke_ <= 0) {  // a damaged car smokes
            dust_.burst(car_world(seg_at(s_)) + V3{0, 0.9f, 0}, 1, 0.6f, hex(0x5A5450), 1.2f, 0.7f, fx_rng_, {0, 2.2f, 0});
            smoke_ = 0.25f - 0.15f * damage_;
        }
        dust_.step(dt, {0, 0.6f, 0});
        // rivals: driven, passed, bumped
        for (Rival &r : rivals_) {
            const float before = r.s - s_;
            // one just behind holds back (it never brakes for a bend, and
            // being hit from behind, unseen, is no fault of the driver's)
            const float ahead = before * SEG_L;
            const float v = ahead < 0 && ahead > -12 && fabsf(r.d - d_) < 0.6f ? std::min(r.v, v_ * 0.9f) : r.v;
            r.s += v * dt / SEG_L;
            r.d += (sinf(r.s * 0.05f) * 0.45f - r.d) * (1 - expf(-dt));
            const float gap = (r.s - s_) * SEG_L;
            if (before > 0 && r.s - s_ <= 0) {
                ++passed_;
                score_ += 150 * stage_;
                events_ |= EV_PICKUP;
            }
            if (gap > -1.5f && gap < 3.2f && fabsf(r.d - d_) < 0.42f && bump_ <= 0) {
                v_ *= 0.55f;
                vd_ += (d_ > r.d ? 1.5f : -1.5f);
                hurt(0.06f);
                bump_ = 1;
            }
            if (gap < -30) {  // far behind: it comes round again ahead
                r.s = s_ + float(AHEAD) * 0.8f + rng_.range(0, 20);
                r.v = rng_.range(tune_.rival_v - 7, tune_.rival_v);
                r.paint = 1 + rng_.below(3);
            }
        }
        // a checkpoint: the next stage, time added, the car mended a little
        const int seg = int(s_);
        if (seg / CHECK_EVERY > checkpoints_) {
            checkpoint();
        }
        while (!road_.empty() && road_.front().index < seg - 6) {
            road_.pop_front();
        }
        while (int(road_.size()) < AHEAD + 20) {
            grow();
        }
        time_left_ -= demo_ ? 0 : dt;
        warn_ -= dt;
        if (!demo_ && (time_left_ < 8 || damage_ > 0.75f) && warn_ <= 0) {
            events_ |= EV_WARN;
            warn_ = 2;
        }
        if (damage_ >= 1) {
            damage_ = 1;
            end("Wrecked");
        } else if (time_left_ <= 0) {
            time_left_ = 0;
            end("Out of time");
        }
    }

    void render(uint16_t *fb) override;

    void hud(Hud *h) const override
    {
        h->score = score_;
        h->meter = time_left_ / CLOCK_FULL;
        h->meter_color = time_left_ < 8 ? 0xFF5A3C : 0xFFB066;
        h->meter_name = "Time";
        h->meter2 = 1 - damage_;
        h->meter2_color = damage_ > 0.75f ? 0xFF4A3A : damage_ > 0.5f ? 0xFFC040 : 0x8ADC6A;
        h->meter2_name = "Car";
        h->stage = stage_;
        if (callout_t_ > 0) {
            snprintf(h->line, sizeof(h->line), "%s", callout_);
        } else {
            snprintf(h->line, sizeof(h->line), "Stage %d · %d km/h · %d %s", stage_, int(v_ * 4.2f), boosts_, boosts_ == 1 ? "boost" : "boosts");
        }
    }
    const char *ending() const override
    {
        return ending_;
    }
    void profile(char *out, int capacity) override
    {
        prof_.report(out, capacity);
    }

private:
    void say(const char *format, ...)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(callout_, sizeof(callout_), format, args);
        va_end(args);
        callout_t_ = 1.8f;
    }

    void end(const char *why)
    {
        over_ = true;
        events_ |= EV_OVER;
        snprintf(ending_, sizeof(ending_), "%s on stage %d", why, stage_);
    }

    // Damage: a knock (a buzz), or rough ground a little at a time. The title
    // screen's car takes none.
    void hurt(float amount, bool knock = true)
    {
        if (knock) {
            events_ |= EV_HIT;
        }
        if (demo_) {
            return;
        }
        damage_ = std::min(1.0f, damage_ + amount);
        clean_ = false;
    }

    void checkpoint()
    {
        ++checkpoints_;
        const int done = stage_;
        if (!demo_) {
            score_ += (500 + 25 * int(time_left_)) * done + (clean_ ? 300 * done : 0);
        }
        stage_ = demo_ ? std::min(3, checkpoints_ + 1) : checkpoints_ + 1;
        tune_ = tuning(stage_);
        time_left_ = std::min(CARRY, time_left_) + tune_.allowance;
        damage_ = std::max(0.0f, damage_ - 0.2f);
        if (checkpoints_ % 2 == 0 && boosts_ < 3) {
            ++boosts_;
        }
        events_ |= EV_LEVEL;
        say(clean_ ? "Clean! +%d s · stage %d · %s" : "Checkpoint +%d s · stage %d · %s", int(tune_.allowance), stage_, stage_news(stage_));
        clean_ = true;
    }

    // What lies on the road: a rock or a log hit (or passed close: points),
    // a puddle that slides the car.
    void hazards(float s0)
    {
        for (Segment &g : road_) {
            if (float(g.index) > s_ + 1) {
                break;
            }
            if (!g.hazard || g.struck) {
                continue;
            }
            if (g.hazard == PUDDLE) {
                if (int(s_) == g.index && fabsf(d_ - g.hpos) < 0.45f + CAR) {
                    g.struck = true;
                    if (slide_ < 0.3f) {
                        vd_ += (fx_rng_.f() < 0.5f ? -1.0f : 1.0f) * fx_rng_.range(1.0f, 2.0f) * clampf(v_ / 30, 0.3f, 1.2f);
                        v_ *= 0.9f;
                    }
                    slide_ = 0.9f;
                    dust_.burst(car_world(seg_at(s_)) + V3{0, 0.2f, 0}, 6, 3, hex(0x9AB4D0), 0.5f, 0.5f, fx_rng_, {0, 2, 0});
                }
                continue;
            }
            const float at = float(g.index) + 0.5f;
            if (s0 >= at || s_ < at) {
                continue;
            }
            g.struck = true;
            float gap;
            if (g.hazard == ROCK) {
                gap = fabsf(d_ - g.hpos) - (CAR + ROCK_R);
            } else {
                const float lo = g.hpos > 0 ? 0 : -LOG_LEN, hi = g.hpos > 0 ? LOG_LEN : 0;
                gap = std::max(lo - (d_ + CAR), (d_ - CAR) - hi);
            }
            if (gap < 0) {
                hurt((g.hazard == ROCK ? 0.2f : 0.15f) + 0.004f * v_);
                vd_ += d_ > g.hpos ? 1.8f : -1.8f;
                v_ *= g.hazard == ROCK ? 0.35f : 0.4f;
                bump_ = 1;
                dust_.burst(car_world(seg_at(s_)) + V3{0, 0.5f, 0}, 10, 4, hex(g.hazard == ROCK ? 0x9A9088 : 0xB08050), 0.7f, 0.5f, fx_rng_,
                {0, 1.5f, 0});
            } else if (gap < 0.3f && !demo_) {
                const int points = 100 * stage_;
                score_ += points;
                say("Close shave +%d", points);
            }
        }
    }

    void add_segment(V3 c, float heading)
    {
        Segment g{};
        g.c = c, g.heading = heading, g.curve = 0, g.index = made_++;
        road_.push_back(g);
    }

    // The road goes on: straights and bends of random length and sharpness
    // (more, sharper and shorter each stage, and hairpins), rolling hills,
    // pines and rocks beside it, and, from stage 2, what lies on it.
    void grow()
    {
        const int stage = 1 + made_ / CHECK_EVERY;
        const Tuning t = tuning(demo_ ? std::min(stage, 3) : stage);
        Segment &last = road_.back();
        if (bend_left_ <= 0) {
            if (rng_.f() < t.straight) {
                bend_ = 0;
                bend_left_ = t.len_lo + rng_.below(t.len_hi - t.len_lo);
            } else {
                // mostly the other way from the last bend: a road that keeps
                // turning one way loops back over itself
                bend_side_ = rng_.f() < 0.7f ? -bend_side_ : bend_side_;
                const bool hairpin = rng_.f() < t.hairpin;
                bend_ = (hairpin ? rng_.range(0.08f, 0.11f) : rng_.range(t.bend_lo, t.bend_hi)) * float(bend_side_);
                bend_left_ = hairpin ? 14 + rng_.below(10) : t.len_lo + rng_.below(t.len_hi - t.len_lo);
            }
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
        // hazards: none just after a checkpoint's arch or right before one
        const int within = g.index % CHECK_EVERY;
        if (puddle_left_ > 0) {  // a puddle is three segments long
            --puddle_left_;
            g.hazard = PUDDLE, g.hpos = road_.back().hpos;
        } else if (t.hazard_gap > 0 && g.index >= next_hazard_ && within > 16 && within < CHECK_EVERY - 6) {
            g.hazard = Hazard(1 + rng_.below(t.kinds));
            if (g.hazard == PUDDLE) {
                g.hpos = rng_.range(-0.5f, 0.5f);
                puddle_left_ = 2;
            } else if (g.hazard == ROCK) {
                g.hpos = rng_.range(-0.7f, 0.7f);
            } else {
                g.hpos = rng_.f() < 0.5f ? -1.0f : 1.0f;
            }
            next_hazard_ = g.index + int(t.hazard_gap * rng_.range(0.6f, 1.4f));
        }
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

    // The title screen's driver (and the tests'): round what lies on the
    // road and the rivals, braking for the sharpest bend coming up.
    Input autopilot() const
    {
        Input in;
        in.absolute = true;
        float sharp = 0;
        for (int k = 2; k <= 18; ++k) {
            sharp = std::max(sharp, fabsf(seg_at(s_ + float(k)).curve));
        }
        const float safe = sqrtf(3.0f / std::max(1e-4f, sharp / SEG_L * 0.11f));  // where the bend pushes as hard as the tyres hold
        // where to be across the road: the place nearest the middle (and
        // where the car is) that is clear of what lies ahead and the rivals
        // (nearer ones count for more: the car needs a second and a half to cross the road)
        struct Span {
            float lo, hi, weight;
        } blocked[8];
        int n = 0;
        for (const Segment &g : road_) {
            if (float(g.index) > s_ + 44 || n == 8) {
                break;
            }
            if (float(g.index) + 1 < s_ || !g.hazard || g.struck) {
                continue;
            }
            const float m = 0.12f, weight = 10 - 8 * clampf((float(g.index) - s_) / 44, 0, 1);  // to spare
            if (g.hazard == LOG) {
                blocked[n++] = g.hpos > 0 ? Span{-CAR - m, LOG_LEN + CAR, weight} : Span{-LOG_LEN - CAR, CAR + m, weight};
            } else {
                const float w = (g.hazard == ROCK ? ROCK_R : 0.45f) + CAR + m;
                blocked[n++] = {g.hpos - w, g.hpos + w, weight};
            }
        }
        for (const Rival &r : rivals_) {
            const float gap = (r.s - s_) * SEG_L;
            if (gap > -2 && gap < 30 && n < 8) {
                blocked[n++] = {r.d - 0.58f, r.d + 0.58f, 10 - 8 * clampf(gap / 30, 0, 1)};
            }
        }
        float target = d_, best = 1e9f;
        for (int i = -9; i <= 9; ++i) {
            const float t = float(i) * 0.1f;
            float cost = fabsf(t) * 0.3f + fabsf(t - d_) * 0.5f;
            for (int k = 0; k < n; ++k) {
                cost += t > blocked[k].lo && t < blocked[k].hi ? blocked[k].weight : 0;
            }
            if (cost < best) {
                best = cost, target = t;
            }
        }
        const Segment &here = seg_at(s_);
        const float push = here.curve / SEG_L * v_ * v_ * 0.11f, authority = 3.6f * clampf(v_ / 18, 0.2f, 1);
        in.x = clampf((target - d_) * 2.5f - vd_ * 0.5f + push / authority, -1, 1);
        // and slower again to move across in a bend: the bend takes the grip
        const float bend = std::max(fabsf(here.curve), fabsf(seg_at(s_ + 6).curve));
        const float room = fabsf(target - d_) > 0.25f ? sqrtf(1.6f / std::max(1e-4f, bend / SEG_L * 0.11f)) : 1e9f;
        in.y = v_ > std::min(safe, room) + 1 ? -0.6f : bot_ ? 1.0f : 0.6f;
        in.action = bot_ && time_left_ < 10 && sharp < 0.03f;
        return in;
    }

    Rng rng_, fx_rng_;
    std::deque<Segment> road_;
    int made_ = 0, bend_left_ = 0, bend_side_ = 1, next_hazard_ = 0, puddle_left_ = 0;
    int stage_ = 1, checkpoints_ = 0, passed_ = 0, boosts_ = 3;
    Tuning tune_ = tuning(1);
    float bend_ = 0, hill_phase_ = 0;
    float s_ = 2, d_ = 0, vd_ = 0, v_ = 0, slip_ = 0, time_left_ = 28, clock_ = 0, boost_ = 0, bump_ = 0, warn_ = 0;
    float slide_ = 0, damage_ = 0, smoke_ = 0, points_ = 0, callout_t_ = 0;
    bool clean_ = true;  // no damage since the last checkpoint
    char callout_[64] = "", ending_[48] = "";
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
        if (a.hazard == PUDDLE && dist < 70) {  // dark water with the sky in it
            const float w0 = (a.hpos - 0.45f) * HALF, w1 = (a.hpos + 0.45f) * HALF, lift = 0.03f;
            const V3 q[4] = {a.c + ra * w0 + V3{0, lift, 0}, a.c + ra * w1 + V3{0, lift, 0}, b.c + rb * w1 + V3{0, lift, 0}, b.c + rb * w0 + V3{0, lift, 0}};
            cam_.poly_world(fb, q, 4, mix565(hex(0x3A4A66), hex(HAZE), fog));
            const float m0 = (a.hpos - 0.12f) * HALF, m1 = (a.hpos + 0.2f) * HALF;
            const V3 glint[4] = {a.c + ra * m0 + V3{0, lift, 0}, a.c + ra * m1 + V3{0, lift, 0}, b.c + rb * m1 + V3{0, lift, 0}, b.c + rb * m0 + V3{0, lift, 0}};
            cam_.poly_world(fb, glint, 4, mix565(hex(0x8A9AC0), hex(HAZE), fog));
        }
        if ((a.hazard == ROCK || a.hazard == LOG) && dist < 70) {
            Pose pose;
            const V3 fwd_a{sinf(a.heading), 0, cosf(a.heading)};
            MeshStyle style;
            style.ambient = 0.5f;
            if (a.hazard == ROCK) {
                pose.pos = a.c + ra * (a.hpos * HALF) + fwd_a * (SEG_L / 2);
                pose.yaw = float(a.index) * 1.3f, pose.scale = ROCK_SCALE;
                const Mesh rock = {ROCK_V, 6, ROCK_T, ROCK_C, 6};
                draw_mesh(fb, cam_, rock, pose, style);
            } else {
                pose.pos = a.c + ra * (a.hpos * LOG_LEN / 2 * HALF) + fwd_a * (SEG_L / 2);
                pose.yaw = a.heading + (a.hpos > 0 ? 0.08f : -0.08f);
                const Mesh log = {LOG_V, 8, LOG_T, LOG_C, 12};
                draw_mesh(fb, cam_, log, pose, style);
            }
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
