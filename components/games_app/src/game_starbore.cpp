// SPDX-License-Identifier: Apache-2.0
// Starbore: a dart of a ship down a twisting neon bore, flat-shaded. Slip
// through the gates (spinning bars, plates with a hole, half walls), sweep up
// energy orbs for the shield, and go faster every sector.
#include <stdio.h>
#include <string.h>

#include <vector>

#include "games_common.h"

namespace games {
namespace {

constexpr int N = 12;                       // sides of the bore
constexpr float R = 2.6f, SEG = 1.6f, FAR = 66, NEAR = 0.12f, F = 128, SHIP_R = 0.32f;
constexpr int RINGS = int(FAR / SEG) + 4;
constexpr uint32_t FOG = 0x07021A;

struct V3 {
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 operator-(V3 a, V3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline V3 operator*(V3 a, float k)
{
    return {a.x * k, a.y * k, a.z * k};
}
inline float dot(V3 a, V3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 cross(V3 a, V3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline V3 norm(V3 a)
{
    const float l = sqrtf(dot(a, a));
    return l > 0 ? a * (1 / l) : a;
}

enum GateType : uint8_t { BAR, PLATE, HALF, CROSS, KINDS };
struct Gate {
    float z, angle, spin, hx, hy, hr;
    GateType type;
    bool done, broken;
};
struct Orb {
    float z, x, y;
    bool alive;
};

class Starbore final : public Game {
public:
    void reset(uint32_t seed, bool demo) override
    {
        rng_ = Rng(seed);
        demo_ = demo;
        over_ = false;
        score_ = 0;
        events_ = 0;
        z_ = 0, ox_ = 0, oy_ = -0.6f, vx_ = 0, vy_ = 0, bank_ = 0;
        speed_ = 10, shield_ = 1, hurt_ = 0, time_ = 0, travelled_ = 0;
        sector_ = 1, gates_passed_ = 0, pulses_ = 1, orbs_got_ = 0, burst_ = 0;
        next_z_ = 30;
        gates_.clear();
        orbs_.clear();
        for (auto &d : dust_) {
            d = {rng_.range(-2, 2), rng_.range(-2, 2), rng_.range(0, FAR)};
        }
        spawn();
    }

    void step(float dt, const Input &input) override
    {
        if (over_) {
            return;
        }
        dt = std::min(dt, 0.1f);
        time_ += dt;
        const Input in = demo_ ? autopilot() : input;
        // steering: a springy slide across the bore
        const float k = 1 - expf(-dt * 7);
        vx_ += (in.x * 6.2f - vx_) * k;
        vy_ += (in.y * 6.2f - vy_) * k;
        ox_ += vx_ * dt;
        oy_ += vy_ * dt;
        const float r = hypotf(ox_, oy_), limit = R - 0.55f;
        if (r > limit) {
            ox_ *= limit / r, oy_ *= limit / r;
        }
        bank_ += (-vx_ * 0.11f - bank_) * k;
        // forward, faster each sector
        speed_ = std::min(30.0f, 10 + 1.7f * float(sector_ - 1) + 0.03f * travelled_ / 10);
        const float z0 = z_;
        z_ += speed_ * dt;
        travelled_ += speed_ * dt;
        hurt_ = std::max(0.0f, hurt_ - dt);
        burst_ = std::max(0.0f, burst_ - dt);
        // the action: a pulse shatters the next gate ahead
        if (in.action && pulses_ > 0) {
            for (Gate &g : gates_) {
                if (!g.done && g.z > z_ && g.z < z_ + 20) {
                    g.done = g.broken = true;
                    --pulses_;
                    score_ += 15;
                    burst_ = 0.5f, burst_z_ = g.z;
                    events_ |= EV_POWER;
                    break;
                }
            }
        }
        // gates crossed this step
        for (Gate &g : gates_) {
            if (g.done || g.z > z_ || g.z <= z0) {
                continue;
            }
            g.done = true;
            const float a = g.angle + g.spin * time_;
            if (hits(g, a, ox_, oy_)) {
                if (hurt_ <= 0) {
                    shield_ -= demo_ ? 0 : 0.34f;
                    hurt_ = 1.2f;
                    events_ |= EV_HIT;
                }
            } else {
                ++gates_passed_;
                score_ += 10 * sector_;
                if (gates_passed_ % 10 == 0) {
                    ++sector_;
                    events_ |= EV_LEVEL;
                    score_ += 200;
                }
            }
        }
        for (Orb &o : orbs_) {
            if (o.alive && o.z <= z_ + 0.4f && o.z > z0 - 0.4f && hypotf(o.x - ox_, o.y - oy_) < 0.7f) {
                o.alive = false;
                score_ += 25;
                if (++orbs_got_ % 12 == 0 && pulses_ < 3) {
                    ++pulses_;
                }
                shield_ = std::min(1.0f, shield_ + 0.06f);
                events_ |= EV_PICKUP;
            }
        }
        score_ = std::max(score_, 0);
        dist_points_ += speed_ * dt * 0.5f;
        while (dist_points_ >= 1) {
            ++score_;
            dist_points_ -= 1;
        }
        if (shield_ < 0.35f && shield_ > 0 && warn_ <= 0) {
            events_ |= EV_WARN;
            warn_ = 4;
        }
        warn_ -= dt;
        if (shield_ <= 0.001f) {
            shield_ = 0;
            over_ = true;
            events_ |= EV_OVER;
        }
        // drop what is behind, add what is ahead
        while (!gates_.empty() && gates_.front().z < z_ - 6) {
            gates_.erase(gates_.begin());
        }
        orbs_.erase(std::remove_if(orbs_.begin(), orbs_.end(), [this](const Orb & o) {
            return o.z < z_ - 6;
        }), orbs_.end());
        spawn();
        for (auto &d : dust_) {
            if (d.z < z_ - 3) {
                d = {rng_.range(-2.2f, 2.2f), rng_.range(-2.2f, 2.2f), z_ + FAR - rng_.range(0, 8)};
            }
        }
    }

    void render(uint16_t *fb) override;

    void hud(Hud *h) const override
    {
        h->score = score_;
        h->meter = shield_;
        h->meter_color = shield_ < 0.35f ? 0xFF4A6A : 0x46E6FF;
        h->meter_name = "Shield";
        snprintf(h->line, sizeof(h->line), "Sector %d · %d %s", sector_, pulses_, pulses_ == 1 ? "pulse" : "pulses");
    }

    const char *ending() const override
    {
        return "Shield down";
    }
    void profile(char *out, int capacity) override
    {
        prof_.report(out, capacity);
    }

private:
    static V3 center(float z)
    {
        return {2.4f * sinf(z * 0.031f) + 1.2f * sinf(z * 0.083f + 1.3f), 1.6f * sinf(z * 0.043f + 0.7f) + 0.8f * sinf(z * 0.101f + 2.1f), z};
    }
    static float twist(float z)
    {
        return z * 0.035f;
    }

    static bool hits(const Gate &g, float a, float x, float y)
    {
        const float c = cosf(a), s = sinf(a);
        const float across = fabsf(-s * x + c * y);  // distance from the bar's line
        switch (g.type) {
        case BAR:
            return across < 0.42f + SHIP_R;
        case CROSS:
            return across < 0.42f + SHIP_R || fabsf(c * x + s * y) < 0.42f + SHIP_R;
        case HALF:
            return -s * x + c * y > -SHIP_R;  // the blocked half is on the bar's left
        case PLATE:
        default:
            return hypotf(x - g.hx, y - g.hy) > g.hr - SHIP_R;
        }
    }

    void spawn()
    {
        while (next_z_ < z_ + FAR + 10) {
            Gate g{};
            g.z = next_z_;
            const int kinds = std::min(int(KINDS), 2 + sector_ / 2);
            g.type = GateType(rng_.below(kinds));
            g.angle = rng_.range(0, TAU);
            g.spin = (rng_.f() < 0.5f ? -1 : 1) * rng_.range(0.25f, 0.45f + 0.15f * float(std::min(sector_, 6)));
            if (g.type == PLATE) {
                g.spin = 0;
                const float a = rng_.range(0, TAU), d = rng_.range(0, R - 1.35f);
                g.hx = cosf(a) * d, g.hy = sinf(a) * d;
                g.hr = std::max(0.85f, 1.15f - 0.05f * float(sector_));
            }
            gates_.push_back(g);
            // orbs on a line between this gate and the next
            const float gap = std::max(9.0f, 17.0f - float(sector_));
            if (rng_.f() < 0.8f) {
                const float a0 = rng_.range(0, TAU), a1 = a0 + rng_.range(-1.5f, 1.5f), r0 = rng_.range(0, 1.6f), r1 = rng_.range(0, 1.6f);
                const int count = 4 + rng_.below(3);
                for (int i = 0; i < count; ++i) {
                    const float t = float(i) / float(count - 1);
                    const float a = mixf(a0, a1, t), rr = mixf(r0, r1, t);
                    orbs_.push_back({g.z + gap * 0.3f + t * gap * 0.45f, cosf(a) * rr, sinf(a) * rr, true});
                }
            }
            next_z_ += gap;
        }
    }

    // For the title screen: aim for the gap of the next gate, else the orbs.
    Input autopilot() const
    {
        float tx = 0, ty = 0;
        bool aim = false;
        for (const Gate &g : gates_) {
            if (g.done || g.z < z_) {
                continue;
            }
            if (g.z - z_ > 16) {
                break;
            }
            const float a = g.angle + g.spin * (time_ + (g.z - z_) / speed_);
            const float c = cosf(a), s = sinf(a);
            switch (g.type) {
            case PLATE:
                tx = g.hx, ty = g.hy;
                break;
            case HALF:
                tx = s * 1.3f, ty = -c * 1.3f;
                break;
            case BAR: {
                const float side = (-s * ox_ + c * oy_) >= 0 ? 1 : -1;
                tx = -s * 1.35f * side, ty = c * 1.35f * side;
                break;
            }
            default: {
                const float q = atan2f(oy_, ox_) - a;
                const float qa = a + (floorf(q / (PI / 2)) + 0.5f) * (PI / 2);
                tx = cosf(qa) * 1.45f, ty = sinf(qa) * 1.45f;
                break;
            }
            }
            aim = true;
            break;
        }
        if (!aim) {
            for (const Orb &o : orbs_) {
                if (o.alive && o.z > z_ && o.z - z_ < 12) {
                    tx = o.x, ty = o.y;
                    break;
                }
            }
        }
        Input in;
        in.x = clampf((tx - ox_) * 1.6f, -1, 1);
        in.y = clampf((ty - oy_) * 1.6f, -1, 1);
        for (const Gate &g : gates_) {  // about to hit: pulse it
            if (!g.done && g.z > z_ && g.z - z_ < speed_ * 0.25f && hypotf(tx - ox_, ty - oy_) > 1.0f) {
                in.action = true;
            }
        }
        return in;
    }

    // camera space
    V3 to_cam(V3 p) const
    {
        const V3 d = p - cam_;
        return {dot(d, right_), dot(d, up_), dot(d, fwd_)};
    }
    static P2 project(V3 c)
    {
        return {float(W) / 2 + F * c.x / c.z, float(H) / 2 - F * c.y / c.z};
    }
    // A camera-space polygon clipped at the near plane, projected and filled.
    static void poly(uint16_t *fb, const V3 *v, int n, uint16_t color)
    {
        P2 out[12];
        int m = 0;
        for (int i = 0; i < n && m < 11; ++i) {
            const V3 &a = v[i], &b = v[(i + 1) % n];
            const bool ia = a.z >= NEAR, ib = b.z >= NEAR;
            if (ia) {
                out[m++] = project(a);
            }
            if (ia != ib) {
                const float t = (NEAR - a.z) / (b.z - a.z);
                out[m++] = project({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, NEAR});
            }
        }
        fill_poly(fb, out, m, color);
    }
    static void edge(uint16_t *fb, V3 a, V3 b, uint16_t color)
    {
        if (a.z < NEAR && b.z < NEAR) {
            return;
        }
        if (a.z < NEAR) {
            const float t = (NEAR - a.z) / (b.z - a.z);
            a = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, NEAR};
        } else if (b.z < NEAR) {
            const float t = (NEAR - b.z) / (a.z - b.z);
            b = {b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t, NEAR};
        }
        const P2 p = project(a), q = project(b);
        line(fb, p.x, p.y, q.x, q.y, color);
    }
    static int fog_of(float z)
    {
        return int(clampf(z / FAR, 0, 1) * 32);
    }

    void draw_gate(uint16_t *fb, const Gate &g);
    void draw_ship(uint16_t *fb);

    Rng rng_;
    float z_ = 0, ox_ = 0, oy_ = 0, vx_ = 0, vy_ = 0, bank_ = 0, speed_ = 10, shield_ = 1, hurt_ = 0, warn_ = 0, time_ = 0;
    float travelled_ = 0, next_z_ = 30, dist_points_ = 0;
    int sector_ = 1, gates_passed_ = 0, pulses_ = 1, orbs_got_ = 0;
    float burst_ = 0, burst_z_ = 0;
    std::vector<Gate> gates_;
    std::vector<Orb> orbs_;
    V3 dust_[36] = {};
    V3 cam_{}, right_{1, 0, 0}, up_{0, 1, 0}, fwd_{0, 0, 1};
    V3 ring_[RINGS][N] = {};
    Profile prof_{{"setup", "panels", "lines", "objects", "ship"}};
};

void Starbore::render(uint16_t *fb)
{
    prof_.start();
    // the camera rides behind and above the ship, looking down the bore
    const float shake = hurt_ > 0.9f ? (hurt_ - 0.9f) * 0.5f : 0;
    const float zc = z_ - 2.8f;
    cam_ = center(zc) + V3{ox_ * 0.75f + sinf(time_ * 71) * shake, oy_ * 0.75f + 0.82f + cosf(time_ * 53) * shake, 0};
    const V3 target = center(z_ + 11) + V3{ox_ * 0.35f, oy_ * 0.35f, 0};
    fwd_ = norm(target - cam_);
    V3 r = norm(cross({0, 1, 0}, fwd_)), u = cross(fwd_, r);
    const float cr = cosf(bank_), sr = sinf(bank_);
    right_ = r * cr + u * sr;
    up_ = u * cr - r * sr;

    std::fill_n(fb, W * H, hex(FOG));
    // the light at the end of the bore
    const V3 end = to_cam(center(z_ + FAR));
    if (end.z > 1) {
        const P2 p = project(end);
        glow(fb, p.x, p.y, 34, rgb(150, 40, 120), 0);
        glow(fb, p.x, p.y, 12, rgb(255, 180, 220), 0.3f);
    }
    // rings of the bore, in camera space
    const int k0 = int(floorf((zc - 1) / SEG));
    static float base_c[N], base_s[N];  // the sides' angles, turned by each ring's twist
    if (base_c[0] == 0) {
        for (int j = 0; j < N; ++j) {
            base_c[j] = cosf(float(j) * TAU / float(N)), base_s[j] = sinf(float(j) * TAU / float(N));
        }
    }
    for (int i = 0; i < RINGS; ++i) {
        const float z = float(k0 + i) * SEG;
        const V3 c = center(z);
        const float tc = cosf(twist(z)) * R, ts = sinf(twist(z)) * R;
        for (int j = 0; j < N; ++j) {
            ring_[i][j] = to_cam({c.x + tc * base_c[j] - ts * base_s[j], c.y + ts * base_c[j] + tc * base_s[j], z});
        }
    }
    prof_.mark(0);
    // objects by depth, drawn inside their segment
    size_t gi = gates_.size(), oi = orbs_.size();
    const uint16_t neon_edge = hex(0x2EE8FF), neon_ring = hex(0xFF3FD0);
    for (int i = RINGS - 2; i >= 0; --i) {
        const float z = float(k0 + i) * SEG;
        const int kidx = k0 + i;
        const float depth = std::max(0.0f, z - zc);
        const int fog = fog_of(depth);
        for (int j = 0; j < N; ++j) {
            const int j1 = (j + 1) % N;
            const V3 q[4] = {ring_[i][j], ring_[i][j1], ring_[i + 1][j1], ring_[i + 1][j]};
            if (q[0].z < NEAR && q[1].z < NEAR && q[2].z < NEAR && q[3].z < NEAR) {
                continue;
            }
            // two tones in a slow checker, brighter where a panel faces the camera
            const bool alt = ((j + (kidx >> 2)) & 1) != 0;
            const V3 mid = (q[0] + q[2]) * 0.5f;
            const V3 nrm = norm(cross(q[1] - q[0], q[3] - q[0]));
            const float facing = fabsf(dot(nrm, norm(mid)));
            const float near = clampf(1.6f - depth / 14, 0.6f, 1.6f);
            const uint16_t base = shade(alt ? 0x2A1060 : 0x1A0A44, (0.55f + 0.45f * facing) * near);
            poly(fb, q, 4, mix565(base, hex(FOG), fog));
        }
        prof_.mark(1);
        // neon edges: along the bore, and a ring every third
        for (int j = 0; j < N; ++j) {
            edge(fb, ring_[i][j], ring_[i + 1][j], mix565((j & 1) ? hex(0x1A6FA0) : neon_edge, hex(FOG), fog));
        }
        if (kidx % 3 == 0) {
            const uint16_t c = mix565(neon_ring, hex(FOG), fog);
            for (int j = 0; j < N; ++j) {
                edge(fb, ring_[i][j], ring_[i][(j + 1) % N], c);
            }
        }
        prof_.mark(2);
        // what lies in this segment
        while (gi > 0 && gates_[gi - 1].z >= z) {
            --gi;
            if (!gates_[gi].broken && gates_[gi].z < z + SEG * 2 && gates_[gi].z > zc + NEAR) {
                draw_gate(fb, gates_[gi]);
            }
        }
        while (oi > 0 && orbs_[oi - 1].z >= z) {
            --oi;
            const Orb &o = orbs_[oi];
            if (!o.alive) {
                continue;
            }
            const V3 c = to_cam(center(o.z) + V3{o.x, o.y, 0});
            if (c.z > NEAR + 0.3f) {
                const P2 p = project(c);
                const float rr = std::min(9.0f, F * 0.3f / c.z);
                const int f = 32 - fog_of(c.z) * 3 / 4;
                const float pulse = 0.85f + 0.15f * sinf(time_ * 9 + o.z);
                glow(fb, p.x, p.y, rr * 2.4f * pulse, scale565(rgb(60, 200, 120), f), 0);
                glow(fb, p.x, p.y, rr, scale565(rgb(200, 255, 210), f), 0.5f);
            }
        }
    }
    prof_.mark(3);
    // dust streaks for speed
    for (const V3 &d : dust_) {
        const V3 base = center(d.z) + V3{d.x, d.y, 0};
        const V3 a = to_cam(base), b = to_cam(base + V3{0, 0, 0.25f + speed_ * 0.03f});
        if (a.z > NEAR && b.z > NEAR) {
            edge(fb, a, b, mix565(hex(0xB8C8FF), hex(FOG), fog_of(a.z)));
        }
    }
    if (burst_ > 0) {  // the shattered gate's flash
        const V3 c = to_cam(center(burst_z_));
        if (c.z > NEAR) {
            const P2 p = project(c);
            const float k = burst_ / 0.5f;
            glow(fb, p.x, p.y, F * R * (1.6f - k) / c.z, scale565(hex(0xFFB060), int(k * 30)), 0);
        }
    }
    draw_ship(fb);
    prof_.mark(4);
    if (hurt_ > 0.8f) {
        const int k = int((hurt_ - 0.8f) / 0.4f * 16);
        for (int i = 0; i < W * H; ++i) {
            fb[i] = mix565(fb[i], hex(0xFF2050), k);
        }
    }
}

void Starbore::draw_gate(uint16_t *fb, const Gate &g)
{
    const V3 c = center(g.z);
    const float a = g.angle + g.spin * time_;
    const float ca = cosf(a), sa = sinf(a);
    const int fog = fog_of(to_cam(c).z);
    const auto at = [&](float x, float y) {
        return to_cam({c.x + x, c.y + y, g.z});
    };
    const uint16_t rim = mix565(hex(0xFFE070), hex(FOG), fog);
    if (g.type == PLATE) {
        const float tw = twist(g.z);
        V3 outer[N], inner[N];
        for (int j = 0; j < N; ++j) {
            const float t = tw + float(j) * TAU / float(N);
            outer[j] = at(R * cosf(t), R * sinf(t));
            inner[j] = at(g.hx + g.hr * cosf(t), g.hy + g.hr * sinf(t));
        }
        for (int j = 0; j < N; ++j) {
            const int j1 = (j + 1) % N;
            const uint16_t col = mix565(hex((j & 1) ? 0x8A1030 : 0x6A0A26), hex(FOG), fog);
            const V3 t1[3] = {outer[j], outer[j1], inner[j1]}, t2[3] = {outer[j], inner[j1], inner[j]};
            poly(fb, t1, 3, col);
            poly(fb, t2, 3, col);
        }
        for (int j = 0; j < N; ++j) {
            edge(fb, inner[j], inner[(j + 1) % N], rim);
        }
        return;
    }
    const auto bar = [&](float c0, float s0) {
        const float L = R * 0.96f, w = 0.42f;
        const V3 q[4] = {at(c0 * L - s0 * w, s0 * L + c0 * w), at(-c0 * L - s0 * w, -s0 * L + c0 * w), at(-c0 * L + s0 * w, -s0 * L - c0 * w),
                         at(c0 * L + s0 * w, s0 * L - c0 * w)
                        };
        poly(fb, q, 4, mix565(hex(0xFF6418), hex(FOG), fog));
        for (int j = 0; j < 4; ++j) {
            edge(fb, q[j], q[(j + 1) % 4], rim);
        }
    };
    if (g.type == BAR || g.type == CROSS) {
        bar(ca, sa);
        if (g.type == CROSS) {
            bar(-sa, ca);
        }
        return;
    }
    // HALF: the half on the bar's left (where -s x + c y > 0)
    V3 q[N / 2 + 1];
    for (int j = 0; j <= N / 2; ++j) {
        const float t = a + float(j) * PI / float(N / 2);
        q[j] = at(R * 0.96f * cosf(t), R * 0.96f * sinf(t));
    }
    poly(fb, q, N / 2 + 1, mix565(hex(0xC81E5A), hex(FOG), fog));
    edge(fb, q[0], q[N / 2], rim);
}

void Starbore::draw_ship(uint16_t *fb)
{
    if (hurt_ > 0 && fmodf(hurt_, 0.16f) < 0.06f) {
        return;  // blinking while the shield recovers
    }
    // nose, wings, the spine's top, belly and tail; the hull slopes down to the tail
    static const V3 model[6] = {{0, 0.02f, 1.0f}, {-0.85f, -0.05f, -0.45f}, {0.85f, -0.05f, -0.45f}, {0, 0.34f, -0.12f}, {0, -0.18f, -0.3f}, {0, 0.08f, -0.56f}};
    static const uint8_t faces[8][3] = {{0, 1, 3}, {0, 3, 2}, {1, 5, 3}, {3, 5, 2}, {0, 4, 1}, {0, 2, 4}, {1, 4, 5}, {2, 5, 4}};
    static const uint32_t colors[8] = {0xD8E4FA, 0xB8C8E8, 0x8090B8, 0x7080A8, 0x3A4260, 0x3A4260, 0x262C44, 0x262C44};
    const float roll = -vx_ * 0.16f, pitch = vy_ * 0.05f;
    const float cr = cosf(roll), sr = sinf(roll), cp = cosf(pitch), sp = sinf(pitch);
    const V3 pos = center(z_) + V3{ox_, oy_, 0};
    V3 cam[6];
    for (int i = 0; i < 6; ++i) {
        V3 m = model[i];
        m = {m.x * cr - m.y * sr, m.x * sr + m.y * cr, m.z};
        m = {m.x, m.y * cp + m.z * sp, -m.y * sp + m.z * cp};
        cam[i] = to_cam(pos + m * 0.8f);
    }
    struct F {
        float z;
        int i;
    } order[8];
    for (int i = 0; i < 8; ++i) {
        order[i] = {cam[faces[i][0]].z + cam[faces[i][1]].z + cam[faces[i][2]].z, i};
    }
    std::sort(order, order + 8, [](const F & a, const F & b) {
        return a.z > b.z;
    });
    const V3 light = norm({0.4f, 0.9f, -0.3f});
    V3 middle = {0, 0, 0};
    for (const V3 &v : cam) {
        middle = middle + v * (1.0f / 6);
    }
    for (const F &f : order) {
        const V3 &a = cam[faces[f.i][0]], &b = cam[faces[f.i][1]], &c = cam[faces[f.i][2]];
        // the outward normal (away from the hull's middle); skip faces turned away
        V3 n = norm(cross(b - a, c - a));
        const V3 g = (a + b + c) * (1.0f / 3);
        if (dot(n, g - middle) < 0) {
            n = n * -1;
        }
        if (dot(n, g) >= 0 || a.z < NEAR || b.z < NEAR || c.z < NEAR) {
            continue;
        }
        const float lit = 0.35f + 0.65f * fabsf(dot(n, light));
        const V3 t[3] = {a, b, c};
        poly(fb, t, 3, shade(colors[f.i], lit));
        const uint16_t trim = f.i < 4 ? hex(0xF0F8FF) : hex(0x2EE8FF);
        edge(fb, a, b, trim);
        edge(fb, b, c, trim);
        edge(fb, c, a, trim);
    }
    const V3 tail = to_cam(pos + V3{0, 0.06f, -0.46f});
    if (tail.z > NEAR) {
        const P2 p = project(tail);
        const float flick = 0.8f + 0.2f * sinf(time_ * 60);
        glow(fb, p.x, p.y, 13 * flick, rgb(40, 150, 255), 0);
        glow(fb, p.x, p.y, 4.5f, rgb(220, 240, 255), 0.6f);
    }
}

}  // namespace

std::unique_ptr<Game> make_starbore()
{
    return std::make_unique<Starbore>();
}

}  // namespace games
