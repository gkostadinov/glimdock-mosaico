// SPDX-License-Identifier: Apache-2.0
// Shared drawing for the games (games_common.h) and the picker's thumbnails.
#include "games_common.h"

#include <stdio.h>
#include <string.h>

namespace games {

std::unique_ptr<Game> make_crypt();
std::unique_ptr<Game> make_starbore();
std::unique_ptr<Game> make_skyglide();
std::unique_ptr<Game> make_jellystorm();
std::unique_ptr<Game> make_pinball();
std::unique_ptr<Game> make_rally();

int64_t (*clock_us)() = nullptr;

std::unique_ptr<Game> make_game(int index)
{
    switch (index) {
    case 0:
        return make_crypt();
    case 1:
        return make_starbore();
    case 2:
        return make_skyglide();
    case 3:
        return make_jellystorm();
    case 4:
        return make_pinball();
    default:
        return make_rally();
    }
}

void thumbnail(const uint16_t *frame, bool transposed, uint16_t *out, int size)
{
    const int x0 = (W - 2 * size) / 2, y0 = (H - 2 * size) / 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int fx = x0 + 2 * x, fy = y0 + 2 * y;
            const uint32_t s = spread(frame[at(fx, fy, transposed)]) + spread(frame[at(fx + 1, fy, transposed)]) +
                               spread(frame[at(fx, fy + 1, transposed)]) + spread(frame[at(fx + 1, fy + 1, transposed)]);
            out[y * size + x] = unspread(s >> 2);
        }
    }
}

void Profile::report(char *out, int capacity)
{
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < N && names[i] && frames; ++i) {
        n += snprintf(out + n, size_t(std::max(0, capacity - n)), "%s%s %.1f", i ? ", " : "", names[i], double(sum[i]) / 1000.0 / frames);
        sum[i] = 0;
    }
    frames = 0;
}

float value_noise(float x, float y, int period, uint32_t seed)
{
    const float fx0 = floorf(x), fy0 = floorf(y);
    const int x0 = int(fx0), y0 = int(fy0);
    float fx = x - fx0, fy = y - fy0;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const auto wrap = [period](int v) {
        v %= period;
        return v < 0 ? v + period : v;
    };
    const int xa = wrap(x0), xb = wrap(x0 + 1), ya = wrap(y0), yb = wrap(y0 + 1);
    const float a = hash2(xa, ya, seed), b = hash2(xb, ya, seed), c = hash2(xa, yb, seed), d = hash2(xb, yb, seed);
    return mixf(mixf(a, b, fx), mixf(c, d, fx), fy);
}

void fill_poly(uint16_t *fb, const P2 *pts, int n, uint16_t color, bool transposed)
{
    if (n < 3) {
        return;
    }
    // each edge once: its rows and its slope (no division per row)
    struct Edge {
        float y0, y1, x0, slope;
    } edges[16];
    int m = 0;
    float ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 0; i < n && m < 16; ++i) {
        const P2 &a = pts[i], &b = pts[(i + 1) % n];
        ymin = std::min(ymin, a.y);
        ymax = std::max(ymax, a.y);
        if (a.y == b.y) {
            continue;
        }
        const P2 &lo = a.y < b.y ? a : b, &hi = a.y < b.y ? b : a;
        edges[m++] = {lo.y, hi.y, lo.x, (hi.x - lo.x) / (hi.y - lo.y)};
    }
    const int y1 = std::max(0, int(ceilf(ymin - 0.5f))), y2 = std::min(H - 1, int(floorf(ymax - 0.5f)));
    for (int y = y1; y <= y2; ++y) {
        const float yc = float(y) + 0.5f;
        float xl = 1e9f, xr = -1e9f;
        for (int i = 0; i < m; ++i) {
            const Edge &e = edges[i];
            if (yc >= e.y0 && yc < e.y1) {
                const float x = e.x0 + (yc - e.y0) * e.slope;
                xl = std::min(xl, x);
                xr = std::max(xr, x);
            }
        }
        const int x1 = std::max(0, int(ceilf(xl - 0.5f))), x2 = std::min(W - 1, int(floorf(xr - 0.5f)));
        if (x1 > x2) {
            continue;
        }
        if (!transposed) {
            std::fill_n(fb + y * W + x1, x2 - x1 + 1, color);
        } else {
            for (int x = x1; x <= x2; ++x) {
                fb[x * H + y] = color;
            }
        }
    }
}

void line(uint16_t *fb, float x0, float y0, float x1, float y1, uint16_t color, bool transposed)
{
    // Liang-Barsky against the frame
    float t0 = 0, t1 = 1;
    const float dx = x1 - x0, dy = y1 - y0;
    const float p[4] = {-dx, dx, -dy, dy}, q[4] = {x0, float(W - 1) - x0, y0, float(H - 1) - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) {
                return;
            }
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0) {
            t0 = std::max(t0, t);
        } else {
            t1 = std::min(t1, t);
        }
        if (t0 > t1) {
            return;
        }
    }
    const float ax = x0 + dx * t0, ay = y0 + dy * t0, bx = x0 + dx * t1, by = y0 + dy * t1;
    const int steps = int(std::max(fabsf(bx - ax), fabsf(by - ay))) + 1;
    const float sx = (bx - ax) / float(steps), sy = (by - ay) / float(steps);
    float x = ax + 0.5f, y = ay + 0.5f;
    for (int i = 0; i <= steps; ++i, x += sx, y += sy) {
        const int ix = int(x), iy = int(y);
        if (unsigned(ix) < unsigned(W) && unsigned(iy) < unsigned(H)) {
            fb[at(ix, iy, transposed)] = color;
        }
    }
}

void glow(uint16_t *fb, float cx, float cy, float r, uint16_t color, float core, bool transposed)
{
    if (r < 0.5f) {
        if (unsigned(int(cx)) < unsigned(W) && unsigned(int(cy)) < unsigned(H)) {
            uint16_t &p = fb[at(int(cx), int(cy), transposed)];
            p = add565(p, color);
        }
        return;
    }
    const int x1 = std::max(0, int(cx - r)), x2 = std::min(W - 1, int(cx + r));
    const int y1 = std::max(0, int(cy - r)), y2 = std::min(H - 1, int(cy + r));
    const float inv = 1.0f / (r * r), c2 = core * core;
    for (int y = y1; y <= y2; ++y) {
        const float dy = float(y) + 0.5f - cy;
        for (int x = x1; x <= x2; ++x) {
            const float dx = float(x) + 0.5f - cx, d2 = (dx * dx + dy * dy) * inv;
            if (d2 >= 1) {
                continue;
            }
            uint16_t &p = fb[at(x, y, transposed)];
            if (d2 < c2) {
                p = add565(p, color);
                continue;
            }
            float k = 1 - d2;
            k *= k;
            p = add565(p, scale565(color, int(k * 32)));
        }
    }
}

}  // namespace games
