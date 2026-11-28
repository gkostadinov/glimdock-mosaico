// SPDX-License-Identifier: Apache-2.0
// Skyglide: a glider over voxel hills at golden hour. Thread the hoops, ride
// the thermals and keep off the ground. Climbing spends lift, which also
// leaks away with time; hoops and thermals give it back, and with none left
// the glider can only sink. Every 30 seconds is a new stage: faster, with
// smaller hoops further apart and lower down, then sandstone spires,
// crosswinds, spire gates and downdrafts. The land, a spire, or three hoops
// missed in a row (flown past, or not reached in time) end the run.
// The landscape is a height and colour map drawn column by column, front to
// back; the spires are stamped into it, so they are drawn, hidden and hit
// exactly as the land is, for nothing more per frame.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>
#include <vector>

#include "games_3d.h"

namespace games {
namespace {

constexpr int MAP = 256, MASK = MAP - 1, BANDS = 8, LIGHTS = 32, FOGS = 16;
constexpr float HS = 0.3f;              // world height per height step
constexpr float FAR = 270, FOCAL = 140; // view distance; ~81 degrees across
constexpr int WATER = 52;               // height steps
constexpr float G = 9.8f;
constexpr int SKY_ROWS = 151;  // the sky's gradient, horizon to zenith
constexpr uint32_t ZENITH = 0x24306E, HAZE = 0xF2C493, SUN = 0xFFE6B0;
constexpr float STAGE_S = 30;         // seconds a stage lasts
constexpr int MISSES = 3;             // hoops missed in a row that end the run
constexpr int SPIRES = 40, SPAN = 7;  // the spire pool; the cells one may cover (SPAN x SPAN)
constexpr int SPIRE_TOP = 255;        // height steps: as high as the land goes
constexpr int MOTES = 28;             // blown along by a gust, to show it

// What a stage asks of the pilot. Stage 1 is the gentle start; the curve is
// tuned (games_sim) so a good pilot lasts a few minutes, not for ever.
struct Tuning {
    float speed;               // cruising speed
    float ring_r;              // a hoop's radius
    float gap_lo, gap_hi;      // from one hoop to the next
    float spread;              // how far the next may turn from the last (radians)
    float above_lo, above_hi;  // its height over the highest land on the way
    int spires;                // spires beside the way to each hoop
    float margin;              // their distance from a hoop's line, past both radii
    float gate;                // the chance of a spire either side of a hoop
    float sink;                // the glider's own sink
    float drain;               // lift lost a second
    float refill;              // lift a hoop gives back
    float thermal;             // the thermals' strength (1 at the start)
    float wind;                // the strongest crosswind gust (0: none)
    float downdraft;           // a gust's sink (0: none)
    float slack;               // seconds to reach a hoop beyond the flight there
};

Tuning tuning(int stage)
{
    const float d = float(std::clamp(stage, 1, 10) - 1);
    Tuning t;
    t.speed = std::min(30.0f, 18 + 1.6f * d);
    t.ring_r = std::max(4.2f, 7.5f - 0.5f * d);
    t.gap_lo = std::min(100.0f, 55 + 5 * d), t.gap_hi = std::min(130.0f, 78 + 6 * d);
    t.spread = std::min(1.0f, 0.55f + 0.07f * d);
    t.above_lo = std::max(9.0f, 16 - 1.2f * d), t.above_hi = std::max(14.0f, 26 - 2 * d);
    static const int spires[10] = {0, 1, 2, 2, 3, 3, 4, 4, 5, 5};
    t.spires = spires[int(d)];
    t.margin = std::max(2.0f, 9 - d);
    t.gate = d < 3 ? 0 : std::min(0.8f, 0.3f + 0.1f * (d - 3));
    t.sink = 0.6f + 0.3f * d;
    t.drain = 0.010f + 0.004f * d;
    t.refill = std::max(0.10f, 0.20f - 0.012f * d);
    t.thermal = std::max(0.5f, 1 - 0.07f * d);
    t.wind = d < 2 ? 0 : std::min(9.0f, 2.5f + 1.3f * (d - 2));
    t.downdraft = d < 4 ? 0 : std::min(4.0f, 1.5f + 0.5f * (d - 4));
    t.slack = std::max(3.0f, 8 - 0.6f * d);
    return t;
}

// What a new stage brings, for the HUD.
const char *stage_news(int stage)
{
    switch (stage) {
    case 2:
        return "spires";
    case 3:
        return "crosswinds";
    case 4:
        return "spire gates";
    case 5:
        return "downdrafts";
    default:
        return "faster, smaller hoops";
    }
}

struct Ring {
    float x, y, alt, heading, r, gap;  // gap: from the hoop before
    uint32_t seq;
};
struct Thermal {
    float x, y;
};
struct Spire {
    float x, y, r;
    uint32_t ring;  // the hoop it stands by: it goes once that hoop is behind
    bool live, shaved;
    uint16_t saved[SPAN * SPAN];  // the land under it, put back when it goes
};
struct Mote {
    float x, y, z;
};

// A difference on land that repeats every MAP cells.
inline float wrap(float d)
{
    return d - roundf(d / MAP) * MAP;
}

// The distance from a point to a segment, the land repeating.
float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    const float dx = bx - ax, dy = by - ay, l2 = std::max(dx * dx + dy * dy, 1e-3f);
    float best = 1e9f;
    for (int e = 0; e < 2; ++e) {  // the point brought next to either end
        const float ex = e ? bx : ax, ey = e ? by : ay;
        const float qx = ex + wrap(px - ex), qy = ey + wrap(py - ey);
        const float t = clampf(((qx - ax) * dx + (qy - ay) * dy) / l2, 0, 1);
        best = std::min(best, hypotf(qx - ax - dx * t, qy - ay - dy * t));
    }
    return best;
}

class Skyglide final : public Game {
public:
    Skyglide()
    {
        build_world(0x5EED);
    }

    void reset(uint32_t seed, bool demo) override
    {
        clear_spires();  // the land is shared by every run
        rng_ = Rng(seed);
        fx_rng_ = Rng(seed ^ 0xA5A5u);
        demo_ = demo;
        over_ = false;
        score_ = 0;
        events_ = 0;
        stage_ = 1, tune_ = tuning(1);
        x_ = 40, y_ = 40, heading_ = 0.6f, pitch_ = 0, bank_ = 0, speed_ = tune_.speed, climb_ = 0;
        alt_ = ground(x_, y_) + 42;
        lift_ = 1, clock_ = 0, warn_ = 0, dist_points_ = 0, lift_cue_ = 0, low_cue_ = 0, gust_ = 0;
        gusts_ = 2, rings_done_ = 0, misses_ = 0, streak_ = 0, ring_seq_ = 0;
        wind_x_ = wind_y_ = down_now_ = 0, wind_clock_ = 4, gust_t_ = gust_len_ = 0;
        gust_wx_ = gust_wy_ = gust_down_ = 0;
        callout_t_ = 0;
        rings_.clear();
        last_x_ = x_ + cosf(heading_) * 20, last_y_ = y_ + sinf(heading_) * 20, last_h_ = heading_, last_alt_ = alt_;
        while (rings_.size() < 5) {
            add_ring();
        }
        new_target();
        for (Mote &m : motes_) {
            m = {x_, y_, -1000};  // placed when a gust first blows
        }
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
        // a stage every STAGE_S seconds (the title screen's pilot keeps to the first)
        const int stage = demo_ ? 1 : 1 + int(clock_ / STAGE_S);
        if (stage != stage_) {
            stage_ = stage, tune_ = tuning(stage);
            score_ += 200 * (stage - 1);
            events_ |= EV_LEVEL;
            say("Stage %d · %s", stage, stage_news(stage));
        }
        const Tuning &t = tune_;
        // An arcade glider: the stick sets how fast it climbs or dives, while
        // there is lift to climb on. (Climbing that cost speed, and a slow
        // glider that could not climb, left hoops out of reach.) Speed only
        // follows for the feel: a dive is quick, a climb slower; the nose
        // shows the glide's angle.
        const float kb = 1 - expf(-dt * 4), kv = 1 - expf(-dt * 3);
        bank_ += (in.x * 0.85f - bank_) * kb;
        float want_climb = in.y * (in.y > 0 ? 8.0f : 11.0f);
        if (lift_ <= 0) {
            want_climb = std::min(want_climb, 0.0f);
        }
        climb_ += (want_climb - climb_) * kv;
        speed_ += (t.speed - climb_ * 0.7f - speed_) * (1 - expf(-dt * 1.2f));
        speed_ = clampf(speed_, 10, 36);
        pitch_ = atan2f(climb_, speed_);
        // the action: a gust lifts for a moment
        if (in.action && gusts_ > 0 && gust_ <= 0) {
            gusts_ -= demo_ ? 0 : 1;  // the title screen's pilot has as many as it needs
            gust_ = 1.6f;
            events_ |= EV_POWER;
        }
        gust_ = std::max(0.0f, gust_ - dt);
        update_wind(dt);
        const float thermal = thermal_lift(x_, y_) * t.thermal;
        const float stall = lift_ <= 0 && !demo_ ? 1.5f : 0;  // out of lift: it sinks faster
        const float rise = thermal + (gust_ > 0 ? 10 : 0) + (demo_ ? 1.4f : 0);
        alt_ += (climb_ - t.sink - stall - down_now_ + rise) * dt;
        alt_ = std::min(alt_, 230.0f);
        if (!demo_) {  // lift: spent climbing, leaking away, found again in a thermal
            lift_ -= (t.drain + 0.0045f * std::max(0.0f, climb_)) * dt;
            lift_ += thermal / 7.5f * 0.10f * dt;
            lift_ = clampf(lift_, 0, 1);
        }
        heading_ += 3.4f * G * tanf(bank_) / speed_ * dt;  // an arcade turn: tight enough for the hoops
        const float x0 = x_, y0 = y_;
        x_ += cosf(heading_) * speed_ * dt + wind_x_ * dt;
        y_ += sinf(heading_) * speed_ * dt + wind_y_ * dt;
        hoops(x0, y0, dt);
        if (over_) {
            return;
        }
        while (rings_.size() < 5) {
            add_ring();
        }
        free_spires();
        // points: distance, three times over flying low (the risk)
        const float clearance = alt_ - collide_ground(x_, y_);
        const bool low = clearance < 8;
        dist_points_ += speed_ * dt * 0.2f * float(stage_) * (low ? 3 : 1);
        const int whole = int(dist_points_);
        score_ += whole, dist_points_ -= float(whole);
        low_cue_ -= dt;
        if (low && low_cue_ <= 0 && !demo_) {
            say("Low pass · points ×3");
            low_cue_ = 8;
        }
        if (thermal > 0 && lift_cue_ <= 0) {
            events_ |= EV_POWER;
            lift_cue_ = 4;
        }
        lift_cue_ -= dt;
        warn_ -= dt;
        if (!demo_ && (lift_ < 0.2f || clearance < 9 || target_budget_ - target_clock_ < 3) && warn_ <= 0) {
            events_ |= EV_WARN;
            warn_ = 2;
        }
        if (clearance < 1.2f) {
            if (demo_) {
                reset(rng_.next(), true);
                return;
            }
            end(spire_near(1.5f) ? Crash::Spire : ground(x_, y_) <= float(WATER) * HS + 0.1f ? Crash::Lake : Crash::Hills);
            return;
        }
        close_calls();
    }

    void render(uint16_t *fb) override;

    void hud(Hud *h) const override
    {
        h->score = score_;
        h->meter = lift_;
        h->meter_color = lift_ < 0.2f ? 0xFF6A4A : 0x8FD8FF;
        h->meter_name = "Lift";
        h->stage = stage_;
        const int clearance = std::max(0, int(alt_ - ground(x_, y_)));
        if (callout_t_ > 0) {
            snprintf(h->line, sizeof(h->line), "%s", callout_);
        } else if (misses_ > 0) {
            snprintf(h->line, sizeof(h->line), "Stage %d · %d m · missed %d of %d · %d %s", stage_, clearance * 2, misses_, MISSES,
                     gusts_, gusts_ == 1 ? "gust" : "gusts");
        } else {
            snprintf(h->line, sizeof(h->line), "Stage %d · %d m up · %d %s", stage_, clearance * 2, gusts_, gusts_ == 1 ? "gust" : "gusts");
        }
    }

    const char *ending() const override
    {
        return ending_;
    }

private:
    enum class Crash { None, Hills, Lake, Spire, Missed };  // how a run ended

    void say(const char *format, ...)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(callout_, sizeof(callout_), format, args);
        va_end(args);
        callout_t_ = 1.8f;
    }

    void end(Crash why)
    {
        static const char *const words[] = {"", "Down in the hills", "Into the lake", "Into a spire", "Missed three hoops"};
        over_ = true;
        events_ |= EV_OVER;
        snprintf(ending_, sizeof(ending_), "%s on stage %d", words[int(why)], stage_);
    }

    // ---- hoops --------------------------------------------------------------------------------
    // The hoop to make next, and the time to make it in.
    void new_target()
    {
        target_clock_ = 0;
        target_budget_ = rings_.empty() ? 1e9f : rings_.front().gap / tune_.speed + tune_.slack;
    }

    // Through any open hoop (those skipped on the way only break the
    // streak); past the next one, or too long getting there: a miss.
    void hoops(float x0, float y0, float dt)
    {
        for (size_t k = 0; k < rings_.size(); ++k) {
            const Ring &r = rings_[k];
            const float nx = cosf(r.heading), ny = sinf(r.heading);
            const float before = (x0 - r.x) * nx + (y0 - r.y) * ny, after = (x_ - r.x) * nx + (y_ - r.y) * ny;
            if (before >= 0 || after < 0) {
                continue;
            }
            const float side = -(x_ - r.x) * ny + (y_ - r.y) * nx, up = alt_ - r.alt;
            const float off = sqrtf(side * side + up * up);
            if (off < r.r) {
                hit(k, off);
                return;
            }
            if (k == 0 && off < r.r * 3 + 15) {  // past the next one, close by
                miss();
                return;
            }
        }
        target_clock_ += dt;
        const Ring &r = rings_.front();
        const float behind = (x_ - r.x) * cosf(r.heading) + (y_ - r.y) * sinf(r.heading);
        if (behind > 25 || (target_clock_ > target_budget_ && !demo_)) {
            miss();
        }
    }

    void hit(size_t k, float off)
    {
        const Ring r = rings_[k];
        rings_.erase(rings_.begin(), rings_.begin() + long(k) + 1);
        ++rings_done_;
        streak_ = k ? 1 : streak_ + 1;
        misses_ = 0;
        const bool bullseye = off < r.r * 0.4f;
        int points = int(100.0f * float(stage_) * (1 + 0.1f * float(std::min(streak_, 10))));
        if (bullseye) {
            points = points * 3 / 2;
        }
        score_ += points;
        lift_ = std::min(1.0f, lift_ + tune_.refill + (bullseye ? 0.05f : 0));
        if (rings_done_ % 8 == 0 && gusts_ < 3) {
            ++gusts_;
        }
        events_ |= EV_PICKUP;
        if (bullseye) {
            say("Bullseye +%d", points);
        } else if (streak_ >= 3) {
            say("%d in a row +%d", streak_, points);
        }
        new_target();
    }

    void miss()
    {
        rings_.erase(rings_.begin());
        streak_ = 0;
        ++misses_;
        events_ |= EV_HIT;
        new_target();
        if (demo_) {
            misses_ = 0;
        } else if (misses_ >= MISSES) {
            end(Crash::Missed);
        } else {
            say("Missed a hoop · %d of %d", misses_, MISSES);
        }
    }

    // ---- wind -----------------------------------------------------------------------------------
    // Now and then a gust, mostly across the way, rising and falling away;
    // later on some of them push down too.
    void update_wind(float dt)
    {
        const Tuning &t = tune_;
        if (gust_len_ > 0) {
            gust_t_ += dt;
            if (gust_t_ >= gust_len_) {
                gust_len_ = 0;
            }
        }
        wind_clock_ -= dt;
        if (gust_len_ <= 0 && t.wind > 0 && wind_clock_ <= 0) {
            gust_len_ = rng_.range(2.5f, 4.0f), gust_t_ = 0;
            const float side = rng_.f() < 0.5f ? -1.0f : 1.0f;
            const float a = heading_ + side * (PI / 2 + rng_.range(-0.5f, 0.5f)), s = rng_.range(0.5f, 1.0f) * t.wind;
            gust_wx_ = cosf(a) * s, gust_wy_ = sinf(a) * s;
            gust_down_ = t.downdraft > 0 && rng_.f() < 0.4f ? t.downdraft : 0;
            const float st = float(stage_);
            wind_clock_ = gust_len_ + rng_.range(std::max(3.0f, 8 - 0.4f * st), std::max(5.0f, 12 - 0.6f * st));
            say(gust_down_ > 0 ? "Downdraft" : side > 0 ? "Crosswind from the left" : "Crosswind from the right");
        }
        const float e = gust_len_ > 0 ? sinf(PI * gust_t_ / gust_len_) : 0;
        wind_x_ = gust_wx_ * e, wind_y_ = gust_wy_ * e, down_now_ = gust_down_ * e;
        if (e <= 0) {
            return;
        }
        // the motes go with it, faster than the glider drifts, so it shows
        const float fx = cosf(heading_), fy = sinf(heading_);
        for (Mote &m : motes_) {
            m.x += wind_x_ * 2.5f * dt, m.y += wind_y_ * 2.5f * dt;
            const float dx = m.x - x_, dy = m.y - y_, along = dx * fx + dy * fy, across = -dx * fy + dy * fx;
            if (along < 3 || along > 55 || fabsf(across) > 30 || fabsf(m.z - alt_) > 14) {
                const float a2 = fx_rng_.range(15, 55), c2 = fx_rng_.range(-26, 26);
                m = {x_ + fx * a2 - fy * c2, y_ + fy * a2 + fx * c2, alt_ + fx_rng_.range(-10, 8)};
            }
        }
    }

    // ---- spires ---------------------------------------------------------------------------------
    // Stamped into the land: a round column of sandstone up to the land's
    // highest step, lit on the side the low sun is on.
    void stamp(Spire &s)
    {
        const int x0 = int(floorf(s.x)) - SPAN / 2, y0 = int(floorf(s.y)) - SPAN / 2;
        for (int j = 0; j < SPAN; ++j)
            for (int i = 0; i < SPAN; ++i) {
                uint16_t &c = cell_[((y0 + j) & MASK) * MAP + ((x0 + i) & MASK)];
                s.saved[j * SPAN + i] = c;
                const float cx = float(x0 + i) + 0.5f - s.x, cy = float(y0 + j) + 0.5f - s.y;
                const float d = sqrtf(cx * cx + cy * cy);
                if (d > s.r) {
                    continue;
                }
                const int h = std::max(int(c & 0xFF), SPIRE_TOP - int(d * d / (s.r * s.r) * 24));
                const float l = d < 0.6f ? 0.45f : (cx * -0.75f + cy * -0.35f) / d;
                const float speck = hash2(x0 + i, y0 + j, 99) - 0.5f;
                const int light = int(clampf(0.3f + l * 0.7f + speck * 0.15f, 0, 1) * (LIGHTS - 1));
                c = uint16_t((5 * LIGHTS + light) << 8 | h);
            }
        s.live = true;
    }

    void unstamp(Spire &s)
    {
        const int x0 = int(floorf(s.x)) - SPAN / 2, y0 = int(floorf(s.y)) - SPAN / 2;
        for (int j = 0; j < SPAN; ++j)
            for (int i = 0; i < SPAN; ++i) {
                cell_[((y0 + j) & MASK) * MAP + ((x0 + i) & MASK)] = s.saved[j * SPAN + i];
            }
        s.live = false;
    }

    void clear_spires()
    {
        for (Spire &s : spires_) {
            if (s.live) {
                unstamp(s);
            }
        }
    }

    // A spire where it is fair: not where the glider would see it appear,
    // not on another's cells (each puts back its own), clear of every open
    // hoop's line and of the way straight on past each hoop (the land
    // repeats: so does a spire, far ahead), and not on land nearly as high.
    bool place_spire(float x, float y, float r, uint32_t ring, float margin)
    {
        Spire *slot = nullptr;
        for (Spire &s : spires_) {
            if (!s.live) {
                slot = slot ? slot : &s;
            } else if (std::max(fabsf(wrap(s.x - x)), fabsf(wrap(s.y - y))) < float(SPAN + 1)) {
                return false;
            }
        }
        if (!slot || hypotf(wrap(x - x_), wrap(y - y_)) < 70 || height_at(int(floorf(x)), int(floorf(y))) > float(SPIRE_TOP) * HS - 15) {
            return false;
        }
        float px = x_, py = y_;
        for (const Ring &g : rings_) {
            // the way to the hoop, and straight on past it while the glider turns
            const float ex = g.x + cosf(g.heading) * 35, ey = g.y + sinf(g.heading) * 35;
            if (seg_dist(x, y, px, py, g.x, g.y) < g.r + r + margin || seg_dist(x, y, g.x, g.y, ex, ey) < g.r + r + margin) {
                return false;
            }
            px = g.x, py = g.y;
        }
        slot->x = x, slot->y = y, slot->r = r, slot->ring = ring, slot->shaved = false;
        stamp(*slot);
        return true;
    }

    // Spires beside the way from one hoop to the next, and from stage 4 a
    // gate: one either side of the hoop.
    void add_spires(float ax, float ay, const Ring &to)
    {
        const Tuning &t = tune_;
        const float len = std::max(1.0f, hypotf(to.x - ax, to.y - ay));
        const float ux = (to.x - ax) / len, uy = (to.y - ay) / len;
        for (int i = 0; i < t.spires; ++i) {
            for (int attempt = 0; attempt < 6; ++attempt) {
                const float r = rng_.range(1.8f, 2.8f), along = rng_.range(0.2f, 0.85f) * len;
                const float side = rng_.f() < 0.5f ? -1.0f : 1.0f;
                const float off = side * (to.r + r + t.margin + rng_.range(0, 12));
                if (place_spire(ax + ux * along - uy * off, ay + uy * along + ux * off, r, to.seq, t.margin)) {
                    break;
                }
            }
        }
        if (rng_.f() < t.gate) {
            const float px = -sinf(to.heading), py = cosf(to.heading), r = 1.8f, off = to.r + r + 1.5f;
            place_spire(to.x + px * off, to.y + py * off, r, to.seq, 1);
            place_spire(to.x - px * off, to.y - py * off, r, to.seq, 1);
        }
    }

    // Spires by hoops already behind go once out of sight; the land is as it was.
    void free_spires()
    {
        const uint32_t oldest = rings_.front().seq;
        const float fx = cosf(heading_), fy = sinf(heading_);
        for (Spire &s : spires_) {
            if (!s.live || s.ring >= oldest) {
                continue;
            }
            const float dx = wrap(s.x - x_), dy = wrap(s.y - y_);
            if (dx * fx + dy * fy < -10 || dx * dx + dy * dy > 150 * 150) {
                unstamp(s);
            }
        }
    }

    bool spire_near(float within) const
    {
        for (const Spire &s : spires_) {
            if (s.live && hypotf(wrap(s.x - x_), wrap(s.y - y_)) < s.r + within) {
                return true;
            }
        }
        return false;
    }

    // Passing a spire's side close, below its top: points for the nerve.
    void close_calls()
    {
        if (demo_ || alt_ > float(SPIRE_TOP) * HS) {
            return;
        }
        const float fx = cosf(heading_), fy = sinf(heading_);
        for (Spire &s : spires_) {
            if (!s.live || s.shaved) {
                continue;
            }
            const float dx = wrap(s.x - x_), dy = wrap(s.y - y_);
            if (dx * fx + dy * fy < 0 && hypotf(dx, dy) - s.r < 3.5f) {
                s.shaved = true;
                const int points = 75 * stage_;
                score_ += points;
                say("Close call +%d", points);
            }
        }
    }

    // ---- the world ------------------------------------------------------------------------------
    float height_at(int x, int y) const
    {
        return float(cell_[(y & MASK) * MAP + (x & MASK)] & 0xFF) * HS;
    }
    // Ground (or water) height under a point, bilinear.
    float ground(float x, float y) const
    {
        const float fx = floorf(x), fy = floorf(y);
        const int ix = int(fx), iy = int(fy);
        const float tx = x - fx, ty = y - fy;
        return mixf(mixf(height_at(ix, iy), height_at(ix + 1, iy), tx), mixf(height_at(ix, iy + 1), height_at(ix + 1, iy + 1), tx), ty);
    }
    // What the glider hits: the drawn column under it, but smooth over
    // slopes (a spire's edge is where it is drawn, not a cell further out).
    float collide_ground(float x, float y) const
    {
        return std::min(ground(x, y), height_at(int(floorf(x)), int(floorf(y))));
    }

    void build_world(uint32_t seed)
    {
        std::vector<float> raw(MAP * MAP);  // on the heap (PSRAM on the board)
        float lo = 1e9f, hi = -1e9f;
        for (int y = 0; y < MAP; ++y)
            for (int x = 0; x < MAP; ++x) {
                float n = 0, a = 0.5f, ridge = 0;
                for (int o = 0; o < 5; ++o) {
                    const int period = 4 << o;
                    const float s = float(period) / float(MAP);
                    const float v = value_noise(float(x) * s, float(y) * s, period, seed + uint32_t(o) * 101);
                    n += v * a;
                    ridge += (1 - fabsf(2 * v - 1)) * a;
                    a *= o < 2 ? 0.5f : 0.4f;
                }
                // ranges of peaks where a broad mask says so; rolling land elsewhere
                const float mask = smoothstep(0.45f, 0.75f, value_noise(float(x) * 3 / MAP, float(y) * 3 / MAP, 3, seed + 77));
                const float h = 0.75f * n + 0.55f * ridge * ridge * mask;
                raw[y * MAP + x] = h;
                lo = std::min(lo, h), hi = std::max(hi, h);
            }
        std::vector<float> soft(MAP * MAP);  // one blur: no single-cell spikes
        for (int y = 0; y < MAP; ++y)
            for (int x = 0; x < MAP; ++x) {
                float sum = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        sum += raw[((y + j) & MASK) * MAP + ((x + i) & MASK)];
                    }
                soft[y * MAP + x] = sum / 9;
            }
        for (int i = 0; i < MAP * MAP; ++i) {
            const float t = (soft[i] - lo) / (hi - lo);
            const int h = std::min(255, int(powf(std::max(0.0f, t), 1.7f) * 270));
            cell_[i] = uint16_t(std::max(h, WATER));  // the height; the colour joins it below
            raw[i] = float(h);
        }
        // colour: a band by height (and a speckle), lit by a low western sun
        const float sx = -0.75f, sy = -0.35f, sz = 0.56f;
        for (int y = 0; y < MAP; ++y)
            for (int x = 0; x < MAP; ++x) {
                const float h = raw[y * MAP + x];
                const float dx = raw[y * MAP + ((x + 1) & MASK)] - raw[y * MAP + ((x - 1) & MASK)];
                const float dy = raw[((y + 1) & MASK) * MAP + x] - raw[((y - 1) & MASK) * MAP + x];
                const float nx = -dx * HS, ny = -dy * HS, nz = 2;
                const float l = (nx * sx + ny * sy + nz * sz) / sqrtf(nx * nx + ny * ny + nz * nz);
                const float speck = hash2(x, y, seed) - 0.5f;
                int band, light = int(clampf(0.25f + l * 0.85f + speck * 0.12f, 0, 1) * (LIGHTS - 1));
                if (h <= WATER) {
                    band = h < WATER - 14 || raw[y * MAP + x] < WATER - 14 ? 0 : 1;
                    light = int(hash2(x / 2, y, seed + 9) * (LIGHTS - 1));
                } else if (h < WATER + 5) {
                    band = 2;
                } else if (h < 120) {
                    band = value_noise(float(x) / 9, float(y) / 9, 256 / 9 + 1, seed + 5) + speck * 0.3f > 0.55f ? 4 : 3;
                } else if (h < 165) {
                    band = 5;
                } else if (h < 196 + speck * 20) {
                    band = 6;
                } else {
                    band = 7;
                }
                cell_[y * MAP + x] = uint16_t((band * LIGHTS + light) << 8 | (cell_[y * MAP + x] & 0xFF));
            }
        // where the air rises: over sunny slopes
        Rng r(seed);
        thermals_.clear();
        while (thermals_.size() < 26) {
            const float tx = r.range(0, MAP), ty = r.range(0, MAP);
            if ((cell_[int(ty) * MAP + int(tx)] & 0xFF) > WATER + 10) {
                thermals_.push_back({tx, ty});
            }
        }
    }

    float thermal_lift(float x, float y) const
    {
        float lift = 0;
        for (const Thermal &t : thermals_) {
            float dx = fabsf(x - t.x), dy = fabsf(y - t.y);
            dx = std::min(dx, MAP - dx), dy = std::min(dy, MAP - dy);  // the map wraps
            const float d = sqrtf(dx * dx + dy * dy);
            if (d < 10) {
                lift = std::max(lift, 7.5f * (1 - d / 10));
            }
        }
        return lift;
    }

    // The next hoop: one a glider can reach from the last, the way there
    // never climbing more than about one in three and the hoop no lower than
    // one in two; over a ridge too steep, it tries another way. Spires
    // already standing count as land.
    void add_ring()
    {
        const Tuning &t = tune_;
        constexpr float CLIMB = 0.3f;
        float h = 0, d = 0, x = 0, y = 0, alt = 0;
        for (int attempt = 0; attempt < 10; ++attempt) {
            const float spread = attempt < 5 ? t.spread : t.spread * 2;
            h = last_h_ + rng_.range(-spread, spread);
            d = rng_.range(t.gap_lo, t.gap_hi);
            x = last_x_ + cosf(h) * d, y = last_y_ + sinf(h) * d;
            float top = 0, steep = 0;
            const int samples = int(d / 2) + 1;  // finer than a spire
            for (int i = 1; i <= samples; ++i) {
                const float k = float(i) / float(samples), g = ground(mixf(last_x_, x, k), mixf(last_y_, y, k));
                top = std::max(top, g);
                steep = std::max(steep, (g + 5 - last_alt_) / (k * d));  // the climb to pass over it
            }
            alt = std::max(top + rng_.range(t.above_lo, t.above_hi), float(WATER) * HS + 16);
            alt = std::max(alt, last_alt_ - 0.5f * d);
            if (alt <= last_alt_ + CLIMB * d && steep <= CLIMB) {
                break;
            }
        }
        rings_.push_back({x, y, alt, h, t.ring_r, d, ++ring_seq_});
        if (!demo_) {
            add_spires(last_x_, last_y_, rings_.back());
        }
        last_x_ = x, last_y_ = y, last_h_ = h, last_alt_ = alt;
    }

    // The title screen's pilot (and the tests'): for the next hoop, crabbed
    // into the wind, round a spire in the way, climbing to meet the hoop and
    // over whatever rises before it.
    Input autopilot() const
    {
        Input in;
        if (rings_.empty()) {
            return in;
        }
        const Ring &next = rings_.front();
        const float dx = next.x - x_, dy = next.y - y_;
        float want = atan2f(dy, dx);
        const float across = -sinf(want) * wind_x_ + cosf(want) * wind_y_;
        want -= asinf(clampf(across / speed_, -0.7f, 0.7f));
        const float reach = lift_ > 0 || demo_ ? 6.0f : 0.0f;  // what it can climb over
        const bool blocked = !clear_way(heading_, reach, 30);
        const float way = clear_heading(want, reach);
        in.x = clampf(angle_diff(heading_, way) * (blocked || way != want ? 3.0f : 1.3f), -1, 1);
        // the climb (or dive) that meets the hoop's middle on arrival, and
        // clears what rises ahead on the way
        const float dist = std::max(10.0f, hypotf(dx, dy));
        float rate = (next.alt - alt_) / (dist / speed_), near = 0;
        const float c = cosf(heading_), s = sinf(heading_);
        for (float d = 5; d <= std::max(45.0f, speed_ * 2.2f); d += 4) {
            const float g = height_at(int(floorf(x_ + c * d)), int(floorf(y_ + s * d)));
            rate = std::max(rate, (g + 6 - alt_) / (d / speed_));
            near = d < 22 ? std::max(near, g) : near;
        }
        rate += tune_.sink + down_now_ + (lift_ <= 0 && !demo_ ? 1.5f : 0);
        in.y = clampf(rate / (rate > 0 ? 8.0f : 11.0f), -1, 1);
        const float clearance = alt_ - std::max(near, collide_ground(x_, y_));
        if (clearance < 6) {
            in.y = 1;  // pull up
            in.action = clearance < 4;
        }
        return in;
    }

    // Nothing in the way along a heading, a wingspan wide, that the glider
    // could not climb over by then.
    bool clear_way(float h, float reach, float far) const
    {
        const float c = cosf(h), s = sinf(h);
        for (float d = 4; d <= far; d += 2) {
            const float top = alt_ - 2 + reach * d / speed_;
            for (float side : {
                        -2.5f, 0.0f, 2.5f
                    }) {
                if (height_at(int(floorf(x_ + c * d - s * side)), int(floorf(y_ + s * d + c * side))) >= top) {
                    return false;
                }
            }
        }
        return true;
    }

    // The clear heading nearest the one wanted.
    float clear_heading(float want, float reach) const
    {
        static const float turns[] = {0, 0.2f, -0.2f, 0.4f, -0.4f, 0.65f, -0.65f, 0.9f, -0.9f};
        for (float turn : turns) {
            if (clear_way(want + turn, reach, 48)) {
                return want + turn;
            }
        }
        return want;
    }

    void dot3(uint16_t *fb, float px, float py, float pz, uint16_t color, int size, bool add = false) const;
    bool transposed() const override
    {
        return true;  // drawn column by column
    }
    void profile(char *out, int capacity) override
    {
        prof_.report(out, capacity);
    }

    Rng rng_, fx_rng_;
    int stage_ = 1;
    Tuning tune_ = tuning(1);
    float x_ = 40, y_ = 40, alt_ = 80, heading_ = 0, pitch_ = 0, bank_ = 0, speed_ = 18, climb_ = 0;
    float lift_ = 1, clock_ = 0, warn_ = 0, dist_points_ = 0, lift_cue_ = 0, low_cue_ = 0;
    float last_x_ = 0, last_y_ = 0, last_h_ = 0, last_alt_ = 0;
    float target_clock_ = 0, target_budget_ = 1e9f;
    int rings_done_ = 0, gusts_ = 2, misses_ = 0, streak_ = 0;
    uint32_t ring_seq_ = 0;
    float gust_ = 0;
    // the weather: a gust's shape, and the wind it blows now
    float wind_x_ = 0, wind_y_ = 0, down_now_ = 0, wind_clock_ = 0, gust_t_ = 0, gust_len_ = 0;
    float gust_wx_ = 0, gust_wy_ = 0, gust_down_ = 0;
    char callout_[48] = "", ending_[48] = "";
    float callout_t_ = 0;
    bool land_built_ = false;
    std::vector<Ring> rings_;
    std::vector<Thermal> thermals_;
    Spire spires_[SPIRES] = {};
    Mote motes_[MOTES] = {};
    // height (low byte) and colour (high byte) side by side: one memory read a sample
    uint16_t cell_[MAP * MAP] = {};
    uint16_t fog_[FOGS][BANDS * LIGHTS] = {};
    int ybuf_[W] = {};  // where the sky ends in each column (the sun's mask)
    Profile prof_{{"palette", "land", "sky", "sprites", "glider"}};
    // the view, kept for the sprites
    float fx_ = 1, fy_ = 0, horizon_ = H / 2, shear_ = 0;
};

// band ends (dark, lit) for the palette
const uint32_t BAND_COLORS[BANDS][2] = {
    {0x0A1E3C, 0x3A7AA8}, {0x14445E, 0x6AB0C8}, {0x6A5230, 0xF0D49A}, {0x1C3410, 0x9CC458},
    {0x0E2410, 0x4E8A34}, {0x3A2E2A, 0xC0A688}, {0x4C4446, 0xD8CCC4}, {0x8890A8, 0xFFF8F0},
};

void Skyglide::render(uint16_t *fb)
{
    prof_.start();
    // palette: everything fades to the haze far off; the land's is built once,
    // the water's shimmer every frame (its colours, then integer fog mixes)
    const auto band_colour = [](int b, float t) {
        const uint32_t lo = BAND_COLORS[b][0], hi = BAND_COLORS[b][1];
        const float r = mixf(float((lo >> 16) & 255), float((hi >> 16) & 255), t);
        const float g = mixf(float((lo >> 8) & 255), float((hi >> 8) & 255), t);
        const float bl = mixf(float(lo & 255), float(hi & 255), t);
        return rgb(int(r * 1.06f), int(g), int(bl * 0.9f));
    };
    static int fog_k[FOGS];
    if (!land_built_) {
        for (int f = 0; f < FOGS; ++f) {
            fog_k[f] = int(powf(float(f) / float(FOGS - 1), 1.2f) * 0.92f * 32 + 0.5f);
        }
        for (int b = 2; b < BANDS; ++b)
            for (int l = 0; l < LIGHTS; ++l) {
                const uint16_t c = band_colour(b, float(l) / float(LIGHTS - 1));
                for (int f = 0; f < FOGS; ++f) {
                    fog_[f][b * LIGHTS + l] = mix565(c, hex(HAZE), fog_k[f]);
                }
            }
        land_built_ = true;
    }
    const float wave = clock_ * 2.2f;
    for (int b = 0; b < 2; ++b)
        for (int l = 0; l < LIGHTS; ++l) {
            const float t = float(l) / float(LIGHTS - 1);
            const uint16_t c = band_colour(b, 0.45f + 0.175f * sinf(t * TAU * 2 + wave) + 0.2f * t);
            for (int f = 0; f < FOGS; ++f) {
                fog_[f][b * LIGHTS + l] = mix565(c, hex(HAZE), fog_k[f]);
            }
        }
    prof_.mark(0);
    fx_ = cosf(heading_), fy_ = sinf(heading_);
    const float rx = -fy_, ry = fx_;
    const float half = float(W) / 2 / FOCAL;
    horizon_ = float(H) * 0.42f + tanf(pitch_) * FOCAL;
    shear_ = tanf(bank_ * 0.3f);  // the land tilts a little; the glider itself banks (a full tilt skewed it all)
    float hz[W];
    int *ybuf = ybuf_;
    for (int c = 0; c < W; ++c) {
        hz[c] = horizon_ - (float(c) - float(W) / 2) * shear_;
        ybuf[c] = H;
    }
    // the land, front to back: each column keeps the highest row drawn so far
    float z = 1, dz = 0.35f;
    int open = W;
    while (z < FAR && open > 0) {
        const uint16_t *lut = fog_[std::min(FOGS - 1, int(z / FAR * float(FOGS)))];
        const float inv = FOCAL / z;
        float px = x_ + fx_ * z - rx * z * half, py = y_ + fy_ * z - ry * z * half;
        const float sx = rx * 2 * z * half / float(W), sy = ry * 2 * z * half / float(W);
        open = 0;
        for (int c = 0; c < W; ++c, px += sx, py += sy) {
            const int top = ybuf[c];
            if (top <= 0) {
                continue;
            }
            ++open;
            const int idx = (int(py) & MASK) * MAP + (int(px) & MASK);
            const uint16_t cell = cell_[idx];
            const int ys = int((alt_ - float(cell & 0xFF) * HS) * inv + hz[c]);
            if (ys < top) {
                const int from = std::max(0, ys);
                std::fill_n(fb + c * H + from, top - from, lut[cell >> 8]);  // along the column: contiguous
                ybuf[c] = from;
            }
        }
        z += dz;
        dz += 0.0068f;
    }
    // the sky above: zenith blue down to the warm haze, and the sun low ahead-left
    prof_.mark(1);
    uint16_t sky[SKY_ROWS];  // by rows above the horizon
    for (int i = 0; i < SKY_ROWS; ++i) {
        sky[i] = mix565(hex(HAZE), hex(ZENITH), int(powf(float(i) / float(SKY_ROWS - 1), 0.6f) * 32));
    }
    for (int c = 0; c < W; ++c) {
        const int h = int(hz[c]);
        uint16_t *col = fb + c * H;
        for (int y = 0; y < ybuf[c]; ++y) {
            col[y] = sky[std::clamp(h - y, 0, SKY_ROWS - 1)];
        }
    }
    prof_.mark(2);
    const float sun_az = 3.6f, sun_el = 0.09f;
    const float rel = angle_diff(heading_, sun_az);
    if (fabsf(rel) < 1.2f) {
        const float sxp = float(W) / 2 + tanf(rel) * FOCAL;
        const float syp = horizon_ - (sxp - float(W) / 2) * shear_ - tanf(sun_el) * FOCAL;
        const int r = 30;
        for (int y = std::max(0, int(syp) - r); y < std::min(H, int(syp) + r); ++y)
            for (int x = std::max(0, int(sxp) - r); x < std::min(W, int(sxp) + r); ++x) {
                if (y >= ybuf_[x]) {  // only where the sky shows
                    continue;
                }
                const float d = hypotf(float(x) - sxp, float(y) - syp) / float(r);
                if (d < 0.22f) {
                    fb[x * H + y] = hex(0xFFF6DC);
                } else if (d < 1) {
                    fb[x * H + y] = add565(fb[x * H + y], scale565(hex(SUN), int((1 - d) * (1 - d) * 20)));
                }
            }
    }
    // thermals: motes rising in a slow spiral, near ones only
    for (size_t i = 0; i < thermals_.size(); ++i) {
        const Thermal &t = thermals_[i];
        float dx = t.x - x_, dy = t.y - y_;
        dx -= roundf(dx / MAP) * MAP, dy -= roundf(dy / MAP) * MAP;
        if (dx * dx + dy * dy > 90 * 90) {
            continue;
        }
        const float base = ground(t.x, t.y);
        for (int k = 0; k < 12; ++k) {
            const float a = clock_ * 1.3f + float(k) * 2.4f, rr = 2 + float(k % 6) * 1.3f;
            const float up = fmodf(clock_ * 9 + float(k) * 11.0f, 70);
            dot3(fb, x_ + dx + cosf(a) * rr, y_ + dy + sinf(a) * rr, base + 4 + up, scale565(hex(0xFFF2D0), int(12 - up * 0.15f)), 0, true);
        }
    }
    // the gust, shown by the motes it blows along: short pale streaks
    const float blow = hypotf(wind_x_, wind_y_);
    if (blow > 0.3f) {
        const uint16_t streak = scale565(hex(0xFFF8E8), int(std::min(1.0f, blow / 4) * 14) + 4);
        for (const Mote &m : motes_) {
            for (int k = 0; k < 3; ++k) {
                dot3(fb, m.x - wind_x_ * 0.12f * float(k), m.y - wind_y_ * 0.12f * float(k), m.z, streak, 0, true);
            }
        }
    }
    // hoops: gold, the next one brighter, and red, blinking, as its time runs out
    bool first = true;
    const bool hurry = !demo_ && target_budget_ - target_clock_ < 3;
    for (const Ring &r : rings_) {
        const float nx = -sinf(r.heading), ny = cosf(r.heading);
        uint16_t col = first ? hex(0xFFD24A) : hex(0xC8962A);
        if (first && hurry && fmodf(clock_, 0.4f) < 0.2f) {
            col = hex(0xFF5038);
        }
        const int size = first ? 2 : 1;
        const int dots = first ? 56 : 28;
        for (int k = 0; k < dots; ++k) {
            const float a = float(k) / float(dots) * TAU;
            dot3(fb, r.x + nx * cosf(a) * r.r, r.y + ny * cosf(a) * r.r, r.alt + sinf(a) * r.r, col, size);
        }
        if (first) {  // off screen: a chevron at the edge points the way
            const float rel_h = angle_diff(heading_, atan2f(r.y - y_, r.x - x_));
            if (fabsf(rel_h) > 0.62f) {
                const float s = rel_h > 0 ? 1 : -1, cx = float(W) / 2 + s * 100, cy = 70;
                const P2 tri[3] = {{cx + s * 9, cy}, {cx - s * 5, cy - 9}, {cx - s * 5, cy + 9}};
                fill_poly(fb, tri, 3, col, true);
            } else {  // ahead but above or below the view: up or down
                const float dx = r.x - x_, dy = r.y - y_, z = dx * fx_ + dy * fy_;
                if (z > 1) {
                    const float sy = horizon_ - (r.alt - alt_) / z * FOCAL;
                    const float s = sy < 50 ? -1.0f : sy > H - 20 ? 1.0f : 0.0f;  // the HUD covers the top
                    if (s != 0) {
                        const float cx = float(W) / 2 + 92, cy = s < 0 ? 96.0f : float(H) - 60;
                        const P2 tri[3] = {{cx, cy + s * 9}, {cx - 9, cy - s * 5}, {cx + 9, cy - s * 5}};
                        fill_poly(fb, tri, 3, col, true);
                    }
                }
            }
        }
        first = false;
    }
    prof_.mark(3);
    // the glider, a mesh of the shared engine seen from just behind, banking
    // into its turns (the right wing dips for a right turn)
    static const V3 verts[19] = {
        {0, 0, 1.1f}, {0, 0.13f, 0.55f}, {-0.11f, 0, 0.3f}, {0.11f, 0, 0.3f}, {0, 0.03f, -1.1f},          // nose, canopy, sides, tail
        {-2.3f, 0.12f, 0.2f}, {-2.3f, 0.12f, -0.1f}, {2.3f, 0.12f, 0.2f}, {2.3f, 0.12f, -0.1f},            // wing tips
        {-0.1f, 0.03f, 0.45f}, {-0.1f, 0.03f, -0.16f}, {0.1f, 0.03f, 0.45f}, {0.1f, 0.03f, -0.16f},        // wing roots
        {0, 0.52f, -1.06f}, {-0.62f, 0.05f, -0.98f}, {0.62f, 0.05f, -0.98f}, {0, -0.1f, 0.2f},             // fin top, tailplane, belly
        {0, 0.05f, -0.72f}, {0, 0.05f, -0.82f},                                                           // fin and tailplane roots
    };
    static const uint8_t tris[16][3] = {
        {9, 5, 6}, {9, 6, 10}, {11, 8, 7}, {11, 12, 8},                     // wings
        {0, 2, 1}, {0, 1, 3}, {2, 4, 1}, {1, 4, 3}, {0, 16, 2}, {0, 3, 16}, {2, 16, 4}, {3, 4, 16},  // fuselage
        {17, 13, 4}, {14, 18, 15}, {5, 6, 6}, {7, 8, 8},                    // fin, tailplane (and two unused)
    };
    static const uint32_t colors[16] = {0xF4F4F0, 0xF4F4F0, 0xF4F4F0, 0xF4F4F0, 0xEDEDE8, 0xEDEDE8, 0x30486A, 0x30486A,
                                        0xD8D8D4, 0xD8D8D4, 0xC8C8C4, 0xC8C8C4, 0xE0402A, 0xF4F4F0, 0, 0
                                       };
    static const Mesh glider = {verts, 19, tris, colors, 14};
    Camera view;  // the viewer's own: at the eye, looking ahead
    view.transposed = true, view.focal = FOCAL;
    Pose pose;
    pose.pos = {0, -1.55f + sinf(clock_ * 1.7f) * 0.03f, 3.7f};  // a little below: seen from above
    pose.roll = -bank_ * 0.75f, pose.pitch = pitch_ * 0.25f;
    MeshStyle style;
    style.two_sided = true, style.ambient = 0.55f;
    draw_mesh(fb, view, glider, pose, style);
    // red wing tips
    view.glow_at(fb, pose_point(pose, {-2.26f, 0.12f, 0.05f}), 0.1f, rgb(200, 40, 20), 0.8f, 3);
    view.glow_at(fb, pose_point(pose, {2.26f, 0.12f, 0.05f}), 0.1f, rgb(200, 40, 20), 0.8f, 3);
    prof_.mark(4);
}

// A world point drawn as a small square, unless the land is in the way.
void Skyglide::dot3(uint16_t *fb, float px, float py, float pz, uint16_t color, int size, bool add) const
{
    const float dx = px - x_, dy = py - y_;
    const float z = dx * fx_ + dy * fy_;
    if (z < 1 || z > FAR) {
        return;
    }
    const float xr = -dx * fy_ + dy * fx_;
    const float sx = float(W) / 2 + xr / z * FOCAL;
    const float sy = horizon_ - (sx - float(W) / 2) * shear_ - (pz - alt_) / z * FOCAL;
    if (sx < -4 || sx > W + 4 || sy < -4 || sy > H + 4) {
        return;
    }
    // hidden behind the land? look along the line of sight
    const int steps = z < 15 ? 0 : std::clamp(int(z / 14), 3, 14);
    for (int i = 1; i < steps; ++i) {
        const float t = float(i) / float(steps);
        if (height_at(int(x_ + dx * t), int(y_ + dy * t)) > alt_ + (pz - alt_) * t) {
            return;
        }
    }
    const int s = size ? std::max(1, int(float(size) * 3.5f * std::min(1.0f, 30 / z) + 0.5f)) : 1;
    for (int x = int(sx); x < int(sx) + s; ++x)
        for (int y = int(sy); y < int(sy) + s; ++y) {
            if (unsigned(x) < unsigned(W) && unsigned(y) < unsigned(H)) {
                uint16_t &p = fb[x * H + y];
                p = add ? add565(p, color) : color;
            }
        }
}

}  // namespace

std::unique_ptr<Game> make_skyglide()
{
    return std::make_unique<Skyglide>();
}

}  // namespace games
