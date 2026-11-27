// SPDX-License-Identifier: Apache-2.0
// Helpers the three games share: RGB565 colour maths, a small random number
// generator, value noise and a flat polygon and line rasterizer.
#pragma once

#include <math.h>
#include <stdint.h>

#include <algorithm>

#include "games_engine.h"

namespace games {

constexpr float PI = 3.14159265358979f, TAU = 2 * PI;

inline float clampf(float v, float a, float b)
{
    return v < a ? a : v > b ? b : v;
}
inline float mixf(float a, float b, float k)
{
    return a + (b - a) * k;
}
inline float smoothstep(float e0, float e1, float x)
{
    const float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}
// The shortest signed turn from angle a to angle b.
inline float angle_diff(float a, float b)
{
    float d = fmodf(b - a + PI, TAU);
    if (d < 0) {
        d += TAU;
    }
    return d - PI;
}

// ---- colour -----------------------------------------------------------------------------------
inline uint16_t rgb(int r, int g, int b)
{
    r = std::clamp(r, 0, 255), g = std::clamp(g, 0, 255), b = std::clamp(b, 0, 255);
    return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
inline uint16_t hex(uint32_t c)
{
    return rgb(int(c >> 16) & 255, int(c >> 8) & 255, int(c) & 255);
}
// 0xRRGGBB scaled by k (0..1+) and mixed towards fog by f (0..1).
inline uint16_t shade(uint32_t c, float k, uint32_t fog = 0, float f = 0)
{
    const float r = float((c >> 16) & 255) * k, g = float((c >> 8) & 255) * k, b = float(c & 255) * k;
    const float fr = float((fog >> 16) & 255), fg = float((fog >> 8) & 255), fb = float(fog & 255);
    return rgb(int(mixf(r, fr, f)), int(mixf(g, fg, f)), int(mixf(b, fb, f)));
}
// RGB565 spread over 32 bits (G in the top half) so the three channels can be
// scaled and summed in one integer.
inline uint32_t spread(uint16_t c)
{
    return (c | (uint32_t(c) << 16)) & 0x07E0F81Fu;
}
inline uint16_t unspread(uint32_t x)
{
    x &= 0x07E0F81Fu;
    return uint16_t(x | (x >> 16));
}
// a towards b by k (0..32).
inline uint16_t mix565(uint16_t a, uint16_t b, int k)
{
    return unspread((spread(a) * uint32_t(32 - k) + spread(b) * uint32_t(k)) >> 5);
}
// c scaled by k (0..32).
inline uint16_t scale565(uint16_t c, int k)
{
    return unspread((spread(c) * uint32_t(k)) >> 5);
}
// a + b, each channel saturating (light added: glows).
inline uint16_t add565(uint16_t a, uint16_t b)
{
    int r = (a >> 11) + (b >> 11), g = ((a >> 5) & 63) + ((b >> 5) & 63), bl = (a & 31) + (b & 31);
    r = r > 31 ? 31 : r, g = g > 63 ? 63 : g, bl = bl > 31 ? 31 : bl;
    return uint16_t((r << 11) | (g << 5) | bl);
}

// ---- randomness -------------------------------------------------------------------------------
struct Rng {
    uint32_t s = 1;
    explicit Rng(uint32_t seed = 1) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float f()  // 0..1
    {
        return float(next() >> 8) / 16777216.0f;
    }
    float range(float a, float b)
    {
        return a + (b - a) * f();
    }
    int below(int n)
    {
        return n > 0 ? int(next() % uint32_t(n)) : 0;
    }
};

// A hash of a lattice point, 0..1.
inline float hash2(int x, int y, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
}
// Smooth value noise on a grid of cell pixels, wrapping every period cells.
float value_noise(float x, float y, int period, uint32_t seed);

// ---- profiling (on the board: where drawing time goes) ---------------------------------------
// Sections are timed from one mark to the next; report() gives milliseconds
// per frame and starts over. Nothing happens without games::clock_us.
struct Profile {
    static constexpr int N = 6;
    const char *names[N] = {};
    int64_t sum[N] = {}, last = 0;
    int frames = 0;
    void start()
    {
        if (clock_us) {
            last = clock_us();
            ++frames;
        }
    }
    void mark(int i)
    {
        if (clock_us) {
            const int64_t t = clock_us();
            sum[i] += t - last;
            last = t;
        }
    }
    void report(char *out, int capacity);
};

// ---- raster ---------------------------------------------------------------------------------
// A frame is row-major (pixel x, y at y * W + x), or transposed (x * H + y)
// for the games that draw column by column: their writes then run along
// memory, which matters with the frame in PSRAM.
inline int at(int x, int y, bool transposed)
{
    return transposed ? x * H + y : y * W + x;
}
struct P2 {
    float x, y;
};
// A convex polygon (screen pixels), filled flat; clipped to the frame.
void fill_poly(uint16_t *fb, const P2 *pts, int n, uint16_t color, bool transposed = false);
// A line, clipped to the frame.
void line(uint16_t *fb, float x0, float y0, float x1, float y1, uint16_t color, bool transposed = false);
// A glowing dot: colour added with a soft falloff over radius r, solid core.
void glow(uint16_t *fb, float cx, float cy, float r, uint16_t color, float core = 0.35f, bool transposed = false);

}  // namespace games
