// SPDX-License-Identifier: Apache-2.0
// Ember Crypt: a lantern-lit crawl through ray-cast crypts. Gather every
// shard to wake the gate, keep the ember fed with oil, chase the wisps off
// with a flare (tap), and step through the gate to a bigger crypt. Darkness
// past the lantern's reach; shards, runes and the gate glow on their own.
#include <stdio.h>
#include <string.h>

#include <vector>

#include "games_common.h"

namespace games {
namespace {

constexpr int MAXN = 33, TEX = 64, LEVELS = 32;
enum Cell : uint8_t { EMPTY = 0, BRICK = 1, MOSS = 2, RUNE = 3, GATE = 4 };
enum Ramp : int { STONE = 0, EARTH = 64, MOSSY = 128, EMBER = 192 };
constexpr float FOV_PLANE = 0.66f, RADIUS = 0.22f;

struct Item {
    float x, y, phase;
    uint8_t kind;  // 0 shard, 1 oil flask
    bool alive;
};
struct Wisp {
    float x, y, phase, stun;
    int tx, ty;  // the cell it heads for
};

class Crypt final : public Game {
public:
    Crypt()
    {
        build_palette();
        build_textures();
    }

    void reset(uint32_t seed, bool demo) override
    {
        rng_ = Rng(seed);
        demo_ = demo;
        over_ = false;
        score_ = 0;
        events_ = 0;
        level_ = 1;
        oil_ = 1;
        time_ = 0;
        new_crypt();
    }

    void step(float dt, const Input &input) override
    {
        if (over_) {
            return;
        }
        dt = std::min(dt, 0.1f);
        time_ += dt;
        intro_ = std::max(0.0f, intro_ - dt);  // a new crypt: the lantern swells in
        const int cell = int(py_) * n_ + int(px_);
        if (cell != flow_from_) {
            bfs(cell, flow_);
            flow_from_ = cell;
        }
        Input in = demo_ ? autopilot() : input;
        // turning and walking, sliding along walls
        // a tilt turns a little gentler than a drag; turning the board turns the view with it
        pa_ += in.x * (in.absolute ? 1.8f : 2.7f) * dt + in.turn * 1.3f;
        const float speed = in.y * (in.y > 0 ? 2.4f : 1.5f);
        const float dx = cosf(pa_) * speed * dt, dy = sinf(pa_) * speed * dt;
        move(px_, py_, dx, dy, RADIUS);
        walk_ += fabsf(speed) * dt * 6.5f;
        // the ember
        flare_ = std::max(0.0f, flare_ - dt);
        hurt_ = std::max(0.0f, hurt_ - dt);
        if (in.action && flares_ > 0 && flare_ <= 0) {
            --flares_;
            flare_ = 4;
            events_ |= EV_POWER;
            for (Wisp &w : wisps_) {
                if (hypotf(w.x - px_, w.y - py_) < 7) {
                    w.stun = 4.5f;
                }
            }
        }
        oil_ -= dt / (demo_ ? 400.0f : 105.0f);
        if (demo_ && oil_ < 0.3f) {
            oil_ = 1;
        }
        warn_ -= dt;
        if (oil_ < 0.2f && warn_ <= 0) {
            events_ |= EV_WARN;
            warn_ = 3;
        }
        if (oil_ <= 0) {
            oil_ = 0;
            over_ = true;
            events_ |= EV_OVER;
            return;
        }
        // pickups
        for (Item &it : items_) {
            if (!it.alive || hypotf(it.x - px_, it.y - py_) > 0.5f) {
                continue;
            }
            it.alive = false;
            events_ |= EV_PICKUP;
            if (it.kind == 0) {
                score_ += 100;
                if (--shards_left_ == 0) {
                    events_ |= EV_POWER;  // the gate wakes
                }
            } else {
                score_ += 25;
                oil_ = std::min(1.0f, oil_ + 0.35f);
            }
        }
        update_wisps(dt);
        // through the gate
        if (shards_left_ == 0 && hypotf(gate_x_ + 0.5f - px_, gate_y_ + 0.5f - py_) < 1.05f) {
            score_ += 250 * level_ + int(oil_ * 200);
            events_ |= EV_LEVEL;
            ++level_;
            if (demo_ && level_ > 3) {
                level_ = 1;
            }
            oil_ = std::min(1.0f, oil_ + 0.2f);
            new_crypt();
        }
    }

    void render(uint16_t *fb) override;

    void hud(Hud *h) const override
    {
        h->score = score_;
        h->meter = oil_;
        h->meter_color = oil_ < 0.2f ? 0xFF5A3C : 0xFFB347;
        h->meter_name = "Oil";
        snprintf(h->line, sizeof(h->line), "Crypt %d · %d/%d shards · %d %s", level_, shards_total_ - shards_left_,
                 shards_total_, flares_, flares_ == 1 ? "flare" : "flares");
    }

    const char *ending() const override
    {
        return "The ember went out";
    }
    bool transposed() const override
    {
        return true;  // drawn column by column
    }
    void profile(char *out, int capacity) override
    {
        prof_.report(out, capacity);
    }

private:
    // ---- the crypt ------------------------------------------------------------------------------
    uint8_t at(int x, int y) const
    {
        return (x < 0 || y < 0 || x >= n_ || y >= n_) ? BRICK : map_[y * n_ + x];
    }
    bool solid(float x, float y) const
    {
        return at(int(floorf(x)), int(floorf(y))) != EMPTY;
    }
    void move(float &x, float &y, float dx, float dy, float r) const
    {
        const float sx = dx > 0 ? r : -r, sy = dy > 0 ? r : -r;
        if (dx != 0 && !solid(x + dx + sx, y - r) && !solid(x + dx + sx, y + r)) {
            x += dx;
        }
        if (dy != 0 && !solid(x - r, y + dy + sy) && !solid(x + r, y + dy + sy)) {
            y += dy;
        }
    }

    // Steps from cell `from` to every open cell (0xFFFF: unreachable).
    void bfs(int from, uint16_t *dist) const
    {
        std::fill_n(dist, MAXN * MAXN, uint16_t(0xFFFF));
        int queue[MAXN * MAXN];
        int head = 0, tail = 0;
        dist[from] = 0;
        queue[tail++] = from;
        while (head < tail) {
            const int c = queue[head++], cx = c % n_, cy = c / n_;
            const int nb[4] = {c - 1, c + 1, c - n_, c + n_};
            const bool ok[4] = {cx > 0, cx < n_ - 1, cy > 0, cy < n_ - 1};
            for (int k = 0; k < 4; ++k) {
                if (ok[k] && map_[nb[k]] == EMPTY && dist[nb[k]] == 0xFFFF) {
                    dist[nb[k]] = uint16_t(dist[c] + 1);
                    queue[tail++] = nb[k];
                }
            }
        }
    }

    void new_crypt()
    {
        n_ = std::min(MAXN, 11 + 4 * level_);
        std::fill_n(map_, MAXN * MAXN, uint8_t(BRICK));
        // a maze over the odd cells (depth first)
        std::vector<int> stack = {1 * n_ + 1};
        map_[n_ + 1] = EMPTY;
        while (!stack.empty()) {
            const int c = stack.back(), cx = c % n_, cy = c / n_;
            int options[4], count = 0;
            const int d[4][2] = {{2, 0}, {-2, 0}, {0, 2}, {0, -2}};
            for (int k = 0; k < 4; ++k) {
                const int nx = cx + d[k][0], ny = cy + d[k][1];
                if (nx > 0 && ny > 0 && nx < n_ - 1 && ny < n_ - 1 && map_[ny * n_ + nx] != EMPTY) {
                    options[count++] = k;
                }
            }
            if (!count) {
                stack.pop_back();
                continue;
            }
            const int k = options[rng_.below(count)];
            map_[(cy + d[k][1] / 2) * n_ + cx + d[k][0] / 2] = EMPTY;
            map_[(cy + d[k][1]) * n_ + cx + d[k][0]] = EMPTY;
            stack.push_back((cy + d[k][1]) * n_ + cx + d[k][0]);
        }
        // loops, so a wisp can be walked around
        for (int i = n_ * n_ / 14; i > 0; --i) {
            const int x = 1 + rng_.below(n_ - 2), y = 1 + rng_.below(n_ - 2);
            if (map_[y * n_ + x] == EMPTY) {
                continue;
            }
            const bool horiz = at(x - 1, y) == EMPTY && at(x + 1, y) == EMPTY && at(x, y - 1) != EMPTY && at(x, y + 1) != EMPTY;
            const bool vert = at(x, y - 1) == EMPTY && at(x, y + 1) == EMPTY && at(x - 1, y) != EMPTY && at(x + 1, y) != EMPTY;
            if (horiz || vert) {
                map_[y * n_ + x] = EMPTY;
            }
        }
        // a few halls
        for (int i = 1 + level_ / 2; i > 0; --i) {
            const int cx = 3 + 2 * rng_.below((n_ - 6) / 2), cy = 3 + 2 * rng_.below((n_ - 6) / 2);
            for (int y = cy - 1; y <= cy + 1; ++y)
                for (int x = cx - 1; x <= cx + 1; ++x) {
                    map_[y * n_ + x] = EMPTY;
                }
        }
        // wall faces: moss in patches deeper in, runes here and there
        const uint32_t patch = rng_.next();
        for (int y = 0; y < n_; ++y)
            for (int x = 0; x < n_; ++x) {
                uint8_t &c = map_[y * n_ + x];
                if (c == EMPTY) {
                    continue;
                }
                const float m = value_noise(float(x) / 4, float(y) / 4, 64, patch);
                c = m > 0.68f - 0.03f * float(level_) ? MOSS : BRICK;
                if (rng_.f() < 0.045f) {
                    c = RUNE;
                }
            }
        bfs(n_ + 1, flow_);
        // the gate: in a wall beside the farthest open cell
        int far = n_ + 1;
        for (int i = 0; i < n_ * n_; ++i) {
            if (flow_[i] != 0xFFFF && flow_[i] > flow_[far]) {
                far = i;
            }
        }
        const int fx = far % n_, fy = far / n_;
        const int around[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        gate_x_ = fx + 1, gate_y_ = fy;
        for (const auto &a : around) {
            const int gx = fx + a[0], gy = fy + a[1];
            if (gx >= 0 && gy >= 0 && gx < n_ && gy < n_ && map_[gy * n_ + gx] != EMPTY) {
                gate_x_ = gx, gate_y_ = gy;
                break;
            }
        }
        map_[gate_y_ * n_ + gate_x_] = GATE;
        // shards, oil and wisps on open cells away from the start
        std::vector<int> open;
        for (int i = 0; i < n_ * n_; ++i) {
            if (map_[i] == EMPTY && flow_[i] != 0xFFFF && flow_[i] >= 4) {
                open.push_back(i);
            }
        }
        for (size_t i = open.size(); i > 1; --i) {
            std::swap(open[i - 1], open[size_t(rng_.below(int(i)))]);
        }
        items_.clear();
        wisps_.clear();
        shards_total_ = shards_left_ = std::min(12, 4 + level_);
        const int flasks = 2 + level_ / 2;
        size_t next = 0;
        for (int i = 0; i < shards_total_ + flasks && next < open.size(); ++i, ++next) {
            const int c = open[next];
            items_.push_back({float(c % n_) + 0.5f, float(c / n_) + 0.5f, rng_.range(0, TAU), uint8_t(i < shards_total_ ? 0 : 1), true});
        }
        shards_total_ = shards_left_ = std::min(shards_total_, int(items_.size()));
        for (int i = 0; i < std::min(7, level_) && next < open.size(); ++next) {
            const int c = open[next];
            if (flow_[c] < 8) {
                continue;
            }
            wisps_.push_back({float(c % n_) + 0.5f, float(c / n_) + 0.5f, rng_.range(0, TAU), 0, c % n_, c / n_});
            ++i;
        }
        px_ = 1.5f, py_ = 1.5f;
        pa_ = at(2, 1) == EMPTY ? 0 : PI / 2;
        flares_ = 2;
        flare_ = 0;
        flow_from_ = -1;
        intro_ = 1.2f;
    }

    void update_wisps(float dt)
    {
        const float speed = std::min(1.8f, 0.85f + 0.12f * float(level_));
        for (Wisp &w : wisps_) {
            w.phase += dt;
            w.stun = std::max(0.0f, w.stun - dt);
            const int c = int(w.y) * n_ + int(w.x);
            const float tx = float(w.tx) + 0.5f, ty = float(w.ty) + 0.5f;
            const float dist = hypotf(tx - w.x, ty - w.y);
            if (dist < 0.08f || map_[w.ty * n_ + w.tx] != EMPTY) {
                // the next cell: towards the player when near, away when stunned, else wander
                const int cx = c % n_, cy = c / n_;
                const int nb[4][2] = {{cx + 1, cy}, {cx - 1, cy}, {cx, cy + 1}, {cx, cy - 1}};
                int best = -1;
                float best_score = 1e9f;
                for (int k = 0; k < 4; ++k) {
                    const int x = nb[k][0], y = nb[k][1];
                    if (at(x, y) != EMPTY) {
                        continue;
                    }
                    const float f = float(flow_[y * n_ + x]);
                    float s = rng_.f() * 3;
                    if (flow_[c] < 12) {
                        s += w.stun > 0 ? -f * 4 : f * 4;
                    }
                    if (s < best_score) {
                        best_score = s, best = k;
                    }
                }
                if (best >= 0) {
                    w.tx = nb[best][0], w.ty = nb[best][1];
                }
            } else {
                const float k = std::min(1.0f, speed * (w.stun > 0 ? 1.3f : 1.0f) * dt / dist);
                w.x += (tx - w.x) * k;
                w.y += (ty - w.y) * k;
            }
            if (w.stun <= 0 && hurt_ <= 0 && hypotf(w.x - px_, w.y - py_) < 0.45f) {
                oil_ -= demo_ ? 0 : 0.2f;
                hurt_ = 0.6f;
                w.stun = 3;
                events_ |= EV_HIT;
            }
        }
    }

    // For the title screen: off to the nearest shard (then the gate), with a
    // flare when a wisp comes close.
    Input autopilot()
    {
        Input in;
        int target = -1, best = 0xFFFF;
        for (const Item &it : items_) {
            const int c = int(it.y) * n_ + int(it.x);
            if (it.alive && it.kind == 0 && flow_[c] < best) {
                best = flow_[c], target = c;
            }
        }
        if (target < 0) {  // the open cell in front of the gate
            const int around[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &a : around) {
                const int x = gate_x_ + a[0], y = gate_y_ + a[1];
                if (at(x, y) == EMPTY && flow_[y * n_ + x] < best) {
                    best = flow_[y * n_ + x], target = y * n_ + x;
                }
            }
        }
        if (target < 0) {
            return in;
        }
        // walk back from the target to the cell next to us
        int c = target;
        while (flow_[c] > 1) {
            const int cx = c % n_, cy = c / n_;
            const int nb[4] = {c - 1, c + 1, c - n_, c + n_};
            const bool ok[4] = {cx > 0, cx < n_ - 1, cy > 0, cy < n_ - 1};
            int step = c;
            for (int k = 0; k < 4; ++k) {
                if (ok[k] && flow_[nb[k]] == flow_[c] - 1) {
                    step = nb[k];
                    break;
                }
            }
            if (step == c) {
                break;
            }
            c = step;
        }
        const float goal = atan2f(float(c / n_) + 0.5f - py_, float(c % n_) + 0.5f - px_);
        const float diff = angle_diff(pa_, goal);
        in.x = clampf(diff * 2.2f, -1, 1);
        in.y = fabsf(diff) < 0.5f ? 0.75f : 0.1f;
        for (const Wisp &w : wisps_) {
            if (w.stun <= 0 && hypotf(w.x - px_, w.y - py_) < 1.8f) {
                in.action = true;
            }
        }
        return in;
    }

    // ---- looks ---------------------------------------------------------------------------------
    void build_palette()
    {
        // four ramps of 64: cold stone, earth, moss, ember (which glows)
        const float ramps[4][2][3] = {
            {{10, 10, 16}, {196, 186, 176}}, {{12, 7, 4}, {206, 150, 98}}, {{4, 12, 6}, {126, 176, 84}}, {{50, 10, 0}, {255, 226, 150}},
        };
        for (int l = 0; l < LEVELS; ++l) {
            const float k = powf(float(l) / float(LEVELS - 1), 1.35f);
            for (int i = 0; i < 256; ++i) {
                const int r = i / 64;
                const float t = float(i % 64) / 63.0f;
                float c[3];
                for (int j = 0; j < 3; ++j) {
                    c[j] = mixf(ramps[r][0][j], ramps[r][1][j], t);
                }
                // the lantern's warm light; embers glow by themselves
                const float lit = r == 3 ? 0.5f + 0.5f * k : k;
                lut_[l][i] = rgb(int(c[0] * lit), int(c[1] * lit * 0.86f), int(c[2] * lit * 0.66f));
            }
        }
    }

    void build_textures()
    {
        Rng r(0xC0FFEE);
        const auto bricks = [&](uint8_t *t, int ramp, bool moss, bool rune) {
            for (int x = 0; x < TEX; ++x)
                for (int y = 0; y < TEX; ++y) {
                    const int row = y / 16, off = (row & 1) * 16;
                    const int bx = (x + off) % 32, by = y % 16;
                    const int brick = ((x + off) / 32) * 7 + row * 13;
                    const bool mortar = bx < 1 || by < 1;
                    const float n = value_noise(float(x) / 3, float(y) / 3, 22, 7) * 0.6f + r.f() * 0.4f;
                    int v = mortar ? 8 + int(n * 6) : 20 + int(hash2(brick, 3, 9) * 14) + int(n * 16);
                    if (!mortar && (bx == 1 || by == 1)) {
                        v += 8;  // a lit top-left edge
                    }
                    int base = ramp;
                    if (moss) {
                        const float m = value_noise(float(x) / 6, float(y) / 5, 64 / 6 + 1, 31) + (y > 40 ? 0.15f : 0);
                        if (m > 0.62f) {
                            base = MOSSY;
                            v = 14 + int(m * 30) + int(r.f() * 8);
                        }
                    }
                    t[x * TEX + y] = uint8_t(base + std::clamp(v, 0, 63));
                }
            if (rune) {  // a carved glowing sigil: a ring with three strokes
                for (int x = 0; x < TEX; ++x)
                    for (int y = 0; y < TEX; ++y) {
                        const float dx = float(x) - 31.5f, dy = float(y) - 31.5f, d = hypotf(dx, dy);
                        const bool ring = fabsf(d - 15) < 1.6f;
                        const bool strokes = (fabsf(dx) < 1.5f && fabsf(dy) < 13) || (fabsf(dy + dx * 0.55f) < 1.4f && d < 14 && dy < 0) ||
                                             (fabsf(dy - dx * 0.55f) < 1.4f && d < 14 && dy < 0);
                        if (ring || strokes) {
                            t[x * TEX + y] = uint8_t(EMBER + 40 + int(r.f() * 20));
                        }
                    }
            }
        };
        bricks(tex_[BRICK], STONE, false, false);
        bricks(tex_[MOSS], STONE, true, false);
        bricks(tex_[RUNE], STONE, false, true);
        // the gate: a spiral of values that, cycled, flows inwards
        for (int x = 0; x < TEX; ++x)
            for (int y = 0; y < TEX; ++y) {
                const float dx = float(x) - 31.5f, dy = float(y) - 31.5f;
                const float d = hypotf(dx, dy), a = atan2f(dy, dx);
                int v = int(d * 1.6f + a / TAU * 64) & 63;
                if (x < 3 || y < 3 || x > 60 || y > 60) {
                    v = 255;  // the frame
                }
                tex_[GATE][x * TEX + y] = uint8_t(v);
            }
        // floor: big worn flagstones; ceiling: rough dark stone with beams
        for (int y = 0; y < TEX; ++y)
            for (int x = 0; x < TEX; ++x) {
                const bool joint = x % 32 < 1 || y % 32 < 1;
                const float n = value_noise(float(x) / 4, float(y) / 4, 16, 3) * 0.7f + r.f() * 0.3f;
                const int stone = ((x / 32) + (y / 32) * 2) * 5;
                floor_[y * TEX + x] = uint8_t(EARTH + (joint ? 4 : std::clamp(18 + stone + int(n * 22), 0, 63)));
                const bool beam = x % 32 < 6;
                const float c = value_noise(float(x) / 5, float(y) / 5, 13, 11);
                ceil_[y * TEX + x] = uint8_t(beam ? EARTH + 10 + int(c * 10) + (x % 32 == 0 ? -6 : 0) : STONE + 8 + int(c * 14));
            }
    }

    int light_level(float d, float reach) const
    {
        const float k = clampf(1 - d / reach, 0, 1);
        return int(powf(k, 1.7f) * float(LEVELS - 1) + 0.5f);
    }

    void draw_sprite(uint16_t *fb, float sx, float sy, int kind, float size, float lift, float phase, int horizon, float reach);
    void draw_lantern(uint16_t *fb, int horizon);

    Rng rng_;
    int n_ = 15, level_ = 1, shards_total_ = 0, shards_left_ = 0, flares_ = 2, gate_x_ = 0, gate_y_ = 0, flow_from_ = -1;
    float px_ = 1.5f, py_ = 1.5f, pa_ = 0, oil_ = 1, time_ = 0, walk_ = 0, flare_ = 0, hurt_ = 0, warn_ = 0, intro_ = 0;
    uint8_t map_[MAXN * MAXN] = {};
    uint16_t flow_[MAXN * MAXN] = {};
    std::vector<Item> items_;
    std::vector<Wisp> wisps_;
    uint16_t lut_[LEVELS][256] = {};
    uint8_t tex_[5][TEX * TEX] = {}, floor_[TEX * TEX] = {}, ceil_[TEX * TEX] = {};
    float zbuf_[W] = {};
    Profile prof_{{"walls", "sprites", "lantern"}};
};

void Crypt::render(uint16_t *fb)
{
    prof_.start();
    const float dx = cosf(pa_), dy = sinf(pa_);
    const float plx = -dy * FOV_PLANE, ply = dx * FOV_PLANE;
    const int horizon = H / 2 + int(sinf(walk_) * 1.6f);
    // how far the lantern reaches: with the oil, flickering; a flare lights far
    float reach = 1.4f + 5.8f * sqrtf(oil_);
    reach *= 1 + 0.035f * sinf(time_ * 13) + 0.025f * sinf(time_ * 31 + 1);
    if (flare_ > 0) {
        reach = mixf(reach, 13.0f, std::min(1.0f, flare_));
    }
    if (intro_ > 0) {
        reach *= 1 - std::min(1.0f, intro_) * 0.7f;
    }
    // floor and ceiling rows: distance and light are the same along a row
    float row_dist[H];
    const uint16_t *row_lut[H];
    for (int y = 0; y < H; ++y) {
        const int p = y > horizon ? y - horizon : horizon - y;
        row_dist[y] = p ? float(H) * 0.5f / float(p) : 1e6f;
        const int l = light_level(row_dist[y], reach);
        row_lut[y] = (l <= (y > horizon ? 0 : 9)) ? nullptr : lut_[y > horizon ? l : l - 9];
    }
    // the gate's colours this frame: asleep it smoulders red, awake it swirls
    uint16_t gate_lut[64];
    const bool open = shards_left_ == 0;
    for (int i = 0; i < 64; ++i) {
        const float v = 0.5f + 0.5f * sinf(float(i) / 64 * TAU * 2 - time_ * (open ? 6 : 1.5f));
        gate_lut[i] = open ? shade(0x6FFFE0, 0.25f + 0.75f * v, 0xFFFFFF, v * v * 0.5f) : shade(0xC2381E, 0.25f + 0.35f * v);
    }
    const uint16_t gate_frame = rgb(60, 40, 30);

    for (int x = 0; x < W; ++x) {
        const float cam = 2 * float(x) / float(W) - 1;
        const float rx = dx + plx * cam, ry = dy + ply * cam;
        int mx = int(px_), my = int(py_);
        const float ddx = rx == 0 ? 1e30f : fabsf(1 / rx), ddy = ry == 0 ? 1e30f : fabsf(1 / ry);
        int stx, sty;
        float sdx, sdy;
        if (rx < 0) {
            stx = -1, sdx = (px_ - float(mx)) * ddx;
        } else {
            stx = 1, sdx = (float(mx) + 1 - px_) * ddx;
        }
        if (ry < 0) {
            sty = -1, sdy = (py_ - float(my)) * ddy;
        } else {
            sty = 1, sdy = (float(my) + 1 - py_) * ddy;
        }
        int side = 0;
        uint8_t cell = EMPTY;
        for (int i = 0; i < 96; ++i) {
            if (sdx < sdy) {
                sdx += ddx, mx += stx, side = 0;
            } else {
                sdy += ddy, my += sty, side = 1;
            }
            cell = at(mx, my);
            if (cell != EMPTY) {
                break;
            }
        }
        const float perp = std::max(0.05f, side == 0 ? sdx - ddx : sdy - ddy);
        zbuf_[x] = perp;
        const int line_h = int(float(H) / perp);
        const int top = horizon - line_h / 2, bottom = top + line_h;  // bottom exclusive
        float wall_x = side == 0 ? py_ + perp * ry : px_ + perp * rx;
        wall_x -= floorf(wall_x);
        int tx = int(wall_x * TEX);
        if ((side == 0 && rx > 0) || (side == 1 && ry < 0)) {
            tx = TEX - 1 - tx;
        }
        const int y1 = std::max(0, top), y2 = std::min(H, bottom);
        const float tstep = float(TEX) / float(line_h);
        float tpos = float(y1 - top) * tstep;
        if (cell == GATE) {
            const uint8_t *col = tex_[GATE] + tx * TEX;
            const int fade = int(clampf(1.3f - perp / 18, 0.35f, 1) * 32);
            for (int y = y1; y < y2; ++y, tpos += tstep) {
                const uint8_t v = col[int(tpos) & 63];
                fb[x * H + y] = scale565(v == 255 ? gate_frame : gate_lut[v], fade);
            }
        } else {
            const int l = light_level(perp, reach) - (side ? 4 : 0);
            const uint8_t *col = tex_[cell] + tx * TEX;
            if (l <= 0 && cell != RUNE) {
                for (int y = y1; y < y2; ++y) {
                    fb[x * H + y] = 0;
                }
            } else {
                const uint16_t *lut = lut_[std::max(0, l)];
                for (int y = y1; y < y2; ++y, tpos += tstep) {
                    fb[x * H + y] = lut[col[int(tpos) & 63]];
                }
            }
        }
        // floor below the wall, ceiling above it
        for (int y = std::max(y2, horizon + 1); y < H; ++y) {
            const uint16_t *lut = row_lut[y];
            if (!lut) {
                fb[x * H + y] = 0;
                continue;
            }
            const float d = row_dist[y];
            const int fx = int((px_ + rx * d) * TEX) & 63, fy = int((py_ + ry * d) * TEX) & 63;
            fb[x * H + y] = lut[floor_[fy * TEX + fx]];
        }
        for (int y = 0; y < std::min(y1, horizon + 1); ++y) {
            const uint16_t *lut = row_lut[y];
            if (!lut) {
                fb[x * H + y] = 0;
                continue;
            }
            const float d = row_dist[y];
            const int fx = int((px_ + rx * d) * TEX) & 63, fy = int((py_ + ry * d) * TEX) & 63;
            fb[x * H + y] = lut[ceil_[fy * TEX + fx]];
        }
    }

    prof_.mark(0);
    // sprites, far to near
    struct S {
        float x, y, d, size, lift, phase;
        int kind;
    };
    S list[64];
    int count = 0;
    for (const Item &it : items_) {
        if (it.alive && count < 64) {
            const float bob = it.kind == 0 ? 0.08f * sinf(time_ * 2.2f + it.phase) : 0;
            list[count++] = {it.x, it.y, 0, it.kind == 0 ? 0.36f : 0.28f, it.kind == 0 ? 0.04f + bob : 0.36f, it.phase, it.kind};
        }
    }
    for (const Wisp &w : wisps_) {
        if (count < 64) {
            list[count++] = {w.x, w.y, 0, 0.55f, 0.02f * sinf(w.phase * 3), w.phase, w.stun > 0 ? 3 : 2};
        }
    }
    for (int i = 0; i < count; ++i) {
        list[i].d = (list[i].x - px_) * (list[i].x - px_) + (list[i].y - py_) * (list[i].y - py_);
    }
    std::sort(list, list + count, [](const S & a, const S & b) {
        return a.d > b.d;
    });
    for (int i = 0; i < count; ++i) {
        draw_sprite(fb, list[i].x, list[i].y, list[i].kind, list[i].size, list[i].lift, list[i].phase, horizon, reach);
    }
    prof_.mark(1);
    draw_lantern(fb, horizon);
    prof_.mark(2);
    if (hurt_ > 0) {  // a wisp's touch: the view flushes red
        const int k = int(hurt_ / 0.6f * 14);
        const uint16_t red = rgb(200, 20, 30);
        for (int i = 0; i < W * H; ++i) {
            fb[i] = mix565(fb[i], red, k);
        }
    }
}

void Crypt::draw_sprite(uint16_t *fb, float sx, float sy, int kind, float size, float lift, float phase, int horizon,
                        float reach)
{
    const float dx = cosf(pa_), dy = sinf(pa_);
    const float plx = -dy * FOV_PLANE, ply = dx * FOV_PLANE;
    const float rx = sx - px_, ry = sy - py_;
    const float inv = 1.0f / (plx * dy - dx * ply);
    const float tx = inv * (dy * rx - dx * ry), tz = inv * (-ply * rx + plx * ry);
    if (tz < 0.2f) {
        return;
    }
    const float cx = float(W) / 2 * (1 + tx / tz), scale = float(H) / tz;
    const float s = scale * size, cy = float(horizon) + scale * lift;
    const int x1 = std::max(0, int(cx - s)), x2 = std::min(W - 1, int(cx + s));
    const int y1 = std::max(0, int(cy - s)), y2 = std::min(H - 1, int(cy + s));
    const float d = hypotf(rx, ry);
    const int lit = light_level(d, reach);
    const float fade = clampf(1.25f - d / 16, 0.3f, 1), near = clampf((tz - 0.25f) * 2, 0, 1);
    const float inv_s = 1.0f / s;
    for (int x = x1; x <= x2; ++x) {
        if (tz >= zbuf_[x]) {
            continue;
        }
        const float u = (float(x) + 0.5f - cx) * inv_s;
        for (int y = y1; y <= y2; ++y) {
            const float v = (float(y) + 0.5f - cy) * inv_s;
            uint16_t &p = fb[x * H + y];
            switch (kind) {
            case 0: {  // a shard: a glowing crystal, two-faced, with a halo
                const float f = fabsf(u) * 2.1f + fabsf(v) * 1.15f;
                if (f < 1) {
                    const float t = 1 - f;
                    const uint32_t face = u < 0 ? 0x9FF8FF : 0x3AB4DC;
                    p = shade(face, (0.65f + 0.5f * t) * fade, 0xFFFFFF, v < -0.35f ? 0.35f : t * t * 0.4f);
                } else {
                    const float g = 1 - (u * u + v * v);
                    if (g > 0) {
                        p = add565(p, scale565(rgb(40, 150, 190), int(g * g * 20 * fade)));
                    }
                }
                break;
            }
            case 1: {  // oil: an amber flask on the floor, lit by the lantern
                const float body = u * u + (v - 0.25f) * (v - 0.25f) * 1.2f;
                const bool neck = fabsf(u) < 0.22f && v > -0.85f && v < -0.2f;
                if (body < 0.42f || neck) {
                    const float k = (0.3f + 0.7f * float(lit) / float(LEVELS - 1));
                    const float hi = (u < -0.15f && v > 0 && v < 0.4f) ? 0.45f : 0;
                    p = shade(neck && body >= 0.42f ? 0x6A4A30 : 0xF0A030, k + hi, 0xFFFFE0, hi);
                }
                break;
            }
            default: {  // a wisp: pale light that drifts; blue and faint when chased off
                const float flick = (0.8f + 0.2f * sinf(time_ * 17 + phase * 5)) * near;
                const float r2 = u * u + (v * 1.15f) * (v * 1.15f);
                const float g = 1 - r2;
                if (g > 0) {
                    const uint16_t c = kind == 3 ? rgb(40, 80, 160) : rgb(150, 90, 230);
                    p = add565(p, scale565(c, int(g * g * g * 26 * flick * fade)));
                    if (r2 < 0.025f) {
                        p = add565(p, scale565(rgb(255, 235, 255), int(26 * flick)));
                    }
                }
                break;
            }
            }
        }
    }
}

// The lantern held low in front, swaying as you walk.
void Crypt::draw_lantern(uint16_t *fb, int horizon)
{
    const float sway = sinf(walk_ * 0.5f) * 5;
    const int cx = W / 2 + 54 + int(sway), cy = H - 26 + int(fabsf(sinf(walk_ * 0.5f)) * 3) + (horizon - H / 2);
    const float flame = 0.75f + 0.25f * sinf(time_ * 19) * sinf(time_ * 7);
    const float k = 0.35f + 0.65f * sqrtf(oil_);
    glow(fb, float(cx), float(cy), 38 * k, scale565(rgb(110, 60, 10), int(32 * flame * k)), 0, true);
    const uint16_t metal = rgb(58, 44, 34), metal_hi = rgb(120, 92, 60);
    // handle and cap
    for (int a = 0; a < 16; ++a) {
        const float t = float(a) / 15 * PI;
        const int x = cx + int(cosf(t) * 9), y = cy - 22 - int(sinf(t) * 7);
        if (unsigned(x) < unsigned(W) && unsigned(y) < unsigned(H)) {
            fb[x * H + y] = metal_hi;
        }
    }
    const P2 cap[4] = {{float(cx - 10), float(cy - 22)}, {float(cx + 10), float(cy - 22)}, {float(cx + 12), float(cy - 16)}, {float(cx - 12), float(cy - 16)}};
    fill_poly(fb, cap, 4, metal, true);
    // the glass: warm light, brightest round the flame
    for (int y = cy - 16; y < cy + 12; ++y) {
        for (int x = cx - 10; x < cx + 10; ++x) {
            if (unsigned(x) >= unsigned(W) || unsigned(y) >= unsigned(H)) {
                continue;
            }
            const float u = float(x - cx) / 10, v = float(y - cy) / 14;
            const float f = 1 - (u * u * 2.5f + (v - 0.15f) * (v - 0.15f) * 1.6f);
            const bool bar = (x - cx + 10) % 7 == 0;
            fb[x * H + y] = bar ? metal : shade(0xFFB040, (0.35f + 0.9f * std::max(0.0f, f) * flame) * k, 0xFFF4C0, std::max(0.0f, f * f) * k);
        }
    }
    const P2 base[4] = {{float(cx - 12), float(cy + 12)}, {float(cx + 12), float(cy + 12)}, {float(cx + 9), float(cy + 18)}, {float(cx - 9), float(cy + 18)}};
    fill_poly(fb, base, 4, metal, true);
}

}  // namespace

std::unique_ptr<Game> make_crypt()
{
    return std::make_unique<Crypt>();
}

}  // namespace games
