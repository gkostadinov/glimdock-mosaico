// SPDX-License-Identifier: Apache-2.0
// Skyglide: a glider over voxel hills at golden hour. Thread the hoops (each
// one buys time), ride the thermals for height, trade height for speed, and
// keep off the ground. The landscape is a height and colour map drawn column
// by column, front to back.
#include <stdio.h>
#include <string.h>

#include <initializer_list>
#include <vector>

#include "games_common.h"

namespace games {
namespace {

constexpr int MAP = 256, MASK = MAP - 1, BANDS = 8, LIGHTS = 32, FOGS = 16;
constexpr float HS = 0.3f;              // world height per height step
constexpr float FAR = 270, FOCAL = 140; // view distance; ~81 degrees across
constexpr int WATER = 52;               // height steps
constexpr float RING_R = 7.5f, G = 9.8f;
constexpr int SKY_ROWS = 151;  // the sky's gradient, horizon to zenith
constexpr uint32_t ZENITH = 0x24306E, HAZE = 0xF2C493, SUN = 0xFFE6B0;

struct Ring {
    float x, y, alt, heading;
    bool done;
};
struct Thermal {
    float x, y;
};

class Skyglide final : public Game {
public:
    Skyglide()
    {
        build_world(0x5EED);
    }

    void reset(uint32_t seed, bool demo) override
    {
        rng_ = Rng(seed);
        demo_ = demo;
        over_ = false;
        score_ = 0;
        events_ = 0;
        x_ = 40, y_ = 40, heading_ = 0.6f, pitch_ = 0, bank_ = 0, speed_ = 18;
        alt_ = ground(x_, y_) + 42;
        time_left_ = 40, clock_ = 0, rings_done_ = 0, warn_ = 0, dist_ = 0, gust_ = 0;
        gusts_ = 2, crashed_ = false;
        rings_.clear();
        last_x_ = x_ + cosf(heading_) * 20, last_y_ = y_ + sinf(heading_) * 20, last_h_ = heading_;
        while (rings_.size() < 5) {
            add_ring();
        }
    }

    void step(float dt, const Input &input) override
    {
        if (over_) {
            return;
        }
        dt = std::min(dt, 0.1f);
        clock_ += dt;
        const Input in = demo_ ? autopilot() : input;
        // bank and pitch follow the stick. Slow, the nose can rise less, smoothly
        // (a stall that dropped the nose and let it rise again bounced)
        const float kb = 1 - expf(-dt * 4), kp = 1 - expf(-dt * 2.5f);
        bank_ += (in.x * 0.85f - bank_) * kb;
        const float climb_limit = clampf((speed_ - 9) / 10, -0.4f, 1) * 0.42f;
        const float want_pitch = std::min(in.y * 0.42f, climb_limit);  // up: the nose rises
        pitch_ += (want_pitch - pitch_) * kp;
        // energy: diving speeds up, climbing slows; drag and a gentle sink
        speed_ += (-G * sinf(pitch_) * 0.8f - 0.0016f * speed_ * speed_ + 0.35f) * dt;
        speed_ = clampf(speed_, 8, 44);
        // the action: a gust lifts for a moment
        if (in.action && gusts_ > 0 && gust_ <= 0) {
            gusts_ -= demo_ ? 0 : 1;  // the title screen's pilot has as many as it needs
            gust_ = 1.6f;
            events_ |= EV_POWER;
        }
        gust_ = std::max(0.0f, gust_ - dt);
        const float lift = thermal_lift(x_, y_) + (gust_ > 0 ? 10 : 0) + (demo_ ? 1.4f : 0);
        alt_ += (speed_ * sinf(pitch_) - 0.9f + lift) * dt;
        alt_ = std::min(alt_, 230.0f);
        heading_ += 3.4f * G * tanf(bank_) / speed_ * dt;  // an arcade turn: tight enough for the hoops
        const float v = speed_ * cosf(pitch_);
        x_ += cosf(heading_) * v * dt;
        y_ += sinf(heading_) * v * dt;
        dist_ += v * dt;
        // hoops: crossing a hoop's plane close to its middle
        for (Ring &r : rings_) {
            if (r.done) {
                continue;
            }
            const float nx = cosf(r.heading), ny = sinf(r.heading);
            const float before = (x_ - cosf(heading_) * v * dt - r.x) * nx + (y_ - sinf(heading_) * v * dt - r.y) * ny;
            const float after = (x_ - r.x) * nx + (y_ - r.y) * ny;
            if (before < 0 && after >= 0) {
                const float side = -(x_ - r.x) * ny + (y_ - r.y) * nx, up = alt_ - r.alt;
                if (side * side + up * up < RING_R * RING_R) {
                    r.done = true;
                    ++rings_done_;
                    score_ += 100 + 10 * rings_done_;
                    time_left_ = std::min(60.0f, time_left_ + std::max(4.0f, 8.0f - float(rings_done_) * 0.1f));
                    if (rings_done_ % 8 == 0 && gusts_ < 3) {
                        ++gusts_;
                    }
                    events_ |= (rings_done_ % 10 == 0) ? EV_LEVEL : EV_PICKUP;
                }
            }
        }
        // missed hoops behind us go; new ones ahead
        while (!rings_.empty()) {
            const Ring &r = rings_.front();
            const float behind = (x_ - r.x) * cosf(r.heading) + (y_ - r.y) * sinf(r.heading);
            if (!r.done && behind < 25) {
                break;
            }
            rings_.erase(rings_.begin());
        }
        while (rings_.size() < 5) {
            add_ring();
        }
        dist_points_ += v * dt * 0.2f;
        while (dist_points_ >= 1) {
            ++score_, dist_points_ -= 1;
        }
        if (lift > 0 && lift_cue_ <= 0) {
            events_ |= EV_POWER;
            lift_cue_ = 4;
        }
        lift_cue_ -= dt;
        time_left_ -= demo_ ? 0 : dt;
        warn_ -= dt;
        const float clearance = alt_ - ground(x_, y_);
        if ((time_left_ < 8 || clearance < 9) && warn_ <= 0) {
            events_ |= EV_WARN;
            warn_ = 2;
        }
        if (clearance < 1.2f || time_left_ <= 0) {
            if (demo_) {
                reset(rng_.next(), true);
                return;
            }
            time_left_ = std::max(0.0f, time_left_);
            over_ = true;
            events_ |= EV_OVER;
            crashed_ = clearance < 1.2f;
        }
    }

    void render(uint16_t *fb) override;

    void hud(Hud *h) const override
    {
        h->score = score_;
        h->meter = time_left_ / 60;
        h->meter_color = time_left_ < 8 ? 0xFF6A4A : 0x8FD8FF;
        h->meter_name = "Time";
        const int clearance = std::max(0, int(alt_ - ground(x_, y_)));
        snprintf(h->line, sizeof(h->line), "%d hoops · %d m up · %d %s", rings_done_, clearance * 2, gusts_, gusts_ == 1 ? "gust" : "gusts");
    }

    const char *ending() const override
    {
        if (!crashed_) {
            return "Out of time";
        }
        return ground(x_, y_) <= float(WATER) * HS + 0.1f ? "Into the lake" : "Down in the hills";
    }

private:
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

    void add_ring()
    {
        const float h = last_h_ + rng_.range(-0.55f, 0.55f);
        const float d = rng_.range(55, 78);
        const float x = last_x_ + cosf(h) * d, y = last_y_ + sinf(h) * d;
        float top = 0;
        for (int i = 0; i <= 10; ++i) {
            const float t = float(i) / 10;
            top = std::max(top, ground(mixf(last_x_, x, t), mixf(last_y_, y, t)));
        }
        const float alt = std::max(top + rng_.range(16, 28), float(WATER) * HS + 16);
        rings_.push_back({x, y, alt, h, false});
        last_x_ = x, last_y_ = y, last_h_ = h;
    }

    Input autopilot() const
    {
        Input in;
        const Ring *next = nullptr;
        for (const Ring &r : rings_) {
            if (!r.done) {
                next = &r;
                break;
            }
        }
        if (!next) {
            return in;
        }
        const float want = atan2f(next->y - y_, next->x - x_);
        in.x = clampf(angle_diff(heading_, want) * 1.1f, -0.7f, 0.7f);
        // the highest ground ahead decides; keep flying speed
        float ahead = 0;
        for (float d : {
                    6.0f, 16.0f, 28.0f, 42.0f
                }) {
            ahead = std::max(ahead, ground(x_ + cosf(heading_) * d, y_ + sinf(heading_) * d));
        }
        const float clearance = alt_ - ahead;
        // the glide slope down (or up) to the hoop's middle
        const float dist = std::max(10.0f, hypotf(next->x - x_, next->y - y_));
        in.y = -clampf(asinf(clampf((alt_ - next->alt + 1) / dist, -0.6f, 0.6f)) / 0.42f, -1, 1);
        if (speed_ < 13 && clearance > 14) {
            in.y = std::min(in.y, -0.3f);  // nose down for speed
        }
        if (clearance < 9) {
            in.y = 0.8f;  // pull up
            in.action = clearance < 6;
        }
        return in;
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

    Rng rng_;
    float x_ = 40, y_ = 40, alt_ = 80, heading_ = 0, pitch_ = 0, bank_ = 0, speed_ = 18;
    float time_left_ = 40, clock_ = 0, warn_ = 0, dist_ = 0, dist_points_ = 0, lift_cue_ = 0;
    float last_x_ = 0, last_y_ = 0, last_h_ = 0;
    int rings_done_ = 0, gusts_ = 2;
    float gust_ = 0;
    bool crashed_ = false, land_built_ = false;
    std::vector<Ring> rings_;
    std::vector<Thermal> thermals_;
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
    // hoops: gold, the next one brighter
    bool first = true;
    for (const Ring &r : rings_) {
        if (r.done) {
            continue;
        }
        const float nx = -sinf(r.heading), ny = cosf(r.heading);
        const uint16_t col = first ? hex(0xFFD24A) : hex(0xC8962A);
        const int size = first ? 2 : 1;
        const int dots = first ? 56 : 28;
        for (int k = 0; k < dots; ++k) {
            const float a = float(k) / float(dots) * TAU;
            dot3(fb, r.x + nx * cosf(a) * RING_R, r.y + ny * cosf(a) * RING_R, r.alt + sinf(a) * RING_R, col, size);
        }
        if (first) {  // off screen: a chevron at the edge points the way
            const float rel_h = angle_diff(heading_, atan2f(r.y - y_, r.x - x_));
            if (fabsf(rel_h) > 0.62f) {
                const float s = rel_h > 0 ? 1 : -1, cx = float(W) / 2 + s * 100, cy = 70;
                const P2 tri[3] = {{cx + s * 9, cy}, {cx - s * 5, cy - 9}, {cx - s * 5, cy + 9}};
                fill_poly(fb, tri, 3, hex(0xFFD24A), true);
            }
        }
        first = false;
    }
    prof_.mark(3);
    // the glider, from just behind: long white wings, red tips, banking into
    // its turns (the right wing dips for a right turn)
    const float bob = sinf(clock_ * 1.7f) * 1.2f;
    const float cx = float(W) / 2, cy = float(H) * 0.8f + bob - pitch_ * 12;
    const float roll = bank_ * 0.7f, rc = cosf(roll), rs = sinf(roll);
    const auto shape = [&](std::initializer_list<P2> local, uint16_t colour) {
        P2 pts[8];
        int n = 0;
        for (const P2 &q : local) {
            pts[n++] = {cx + q.x * rc - q.y * rs, cy + q.x * rs + q.y * rc};
        }
        fill_poly(fb, pts, n, colour, true);
    };
    const uint16_t wing = hex(0xF4F4F0), under = hex(0xB8BCC4), tip = hex(0xE0402A);
    shape({{-62, -1}, {-4, -4}, {-4, 2}, {-61, 2}}, wing);
    shape({{4, -4}, {62, -1}, {61, 2}, {4, 2}}, wing);
    shape({{-61, 2}, {-4, 2}, {-4, 3}, {-61, 3}}, under);
    shape({{4, 2}, {61, 2}, {61, 3}, {4, 3}}, under);
    shape({{-66, -1}, {-58, -1.5f}, {-58, 2}, {-65, 2}}, tip);
    shape({{58, -1.5f}, {66, -1}, {65, 2}, {58, 2}}, tip);
    shape({{-4, 6}, {-3, -13}, {0, -17}, {3, -13}, {4, 6}, {0, 8}}, hex(0xE8E8E4));
    shape({{-2.5f, -6}, {0, -13}, {2.5f, -6}, {0, -3}}, hex(0x30486A));
    shape({{-17, 9}, {17, 9}, {16, 11}, {-16, 11}}, wing);
    shape({{-1.2f, 10}, {-0.8f, -2}, {0.8f, -2}, {1.2f, 10}}, tip);
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
