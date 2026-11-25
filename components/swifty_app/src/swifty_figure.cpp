// SPDX-License-Identifier: Apache-2.0
// Swifty's figure: see swifty_figure.h. The geometry, shading and motion
// follow Swifty Home's web/app/swifty/figure.js line for line where they can;
// its WebGL pipeline becomes a depth-buffered triangle rasterizer with
// perspective-correct varyings and the fragment shader as C++. The web
// figure's costumes (hacker, weather) and the lock's house are left out.
#include "swifty_figure.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <vector>

namespace swifty {
namespace {

constexpr float PI = 3.14159265358979f, TAU = PI * 2;

struct V3 {
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 mixv(V3 a, V3 b, float k) { return a + (b - a) * k; }

inline float mixf(float a, float b, float k) { return a + (b - a) * k; }
inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline float sat(float v) { return clampf(v, 0, 1); }
inline float seg(float v, float a, float b) { return sat((v - a) / (b - a)); }
inline float ease(float v) { return v * v * (3 - 2 * v); }
inline float smooth(float e0, float e1, float x)
{
    const float t = sat((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
inline float stepf(float edge, float x) { return x < edge ? 0.f : 1.f; }
inline float fract(float v) { return v - floorf(v); }
inline float len2(float x, float y) { return sqrtf(x * x + y * y); }
// Integer powers by multiplication (powf is slow on the board).
inline float p2(float v) { return v * v; }
inline float p3(float v) { return v * v * v; }
inline float p5(float v) { return p2(p2(v)) * v; }
inline float p8(float v) { return p2(p2(p2(v))); }
inline float p30(float v)
{
    const float v2 = v * v, v4 = v2 * v2, v8 = v4 * v4, v16 = v8 * v8;
    return v16 * v8 * v4 * v2;
}
inline float p40(float v)
{
    const float v8 = p8(v), v32 = p2(p2(v8));
    return v32 * v8;
}
inline V3 hexc(uint32_t h) { return {float(h >> 16) / 255, float((h >> 8) & 255) / 255, float(h & 255) / 255}; }

// ---- matrices: column-major, as WebGL reads them -------------------------------------------
using M4 = std::array<float, 16>;
using M3 = std::array<float, 9>;

M4 mul(const M4 &a, const M4 &b)
{
    M4 o;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
}

// Move, then turn (yaw over pitch over roll), then size.
// (No NaN default for "uniform scale": the rasterizer is built with
// -ffast-math, which assumes NaN never occurs.)
M4 trs(float x, float y, float z, float rx, float ry, float rz, float sx, float sy, float sz)
{
    const float ca = cosf(rx), sa = sinf(rx), cb = cosf(ry), sb = sinf(ry), cc = cosf(rz), sc = sinf(rz);
    return {(cb * cc + sb * sa * sc) * sx, ca * sc * sx, (cb * sa * sc - sb * cc) * sx, 0,
            (sb * sa * cc - cb * sc) * sy, ca * cc * sy, (sb * sc + cb * sa * cc) * sy, 0,
            sb * ca * sz, -sa * sz, cb * ca * sz, 0,
            x, y, z, 1};
}

M4 trs(float x, float y, float z, float rx, float ry, float rz, float s) { return trs(x, y, z, rx, ry, rz, s, s, s); }

// What turns a surface direction the way the matrix turns the surface.
M3 nmat(const M4 &m)
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g, s = a * A + b * B + c * C < 0 ? -1.f : 1.f;
    return {A * s, B * s, C * s, (c * h - b * i) * s, (a * i - c * g) * s, (b * g - a * h) * s,
            (b * f - c * e) * s, (c * d - a * f) * s, (a * e - b * d) * s};
}

const M4 MIRROR = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// ---- shapes: sheets of points wrapped closed; nine numbers a point --------------------------
struct Part {
    int ifrom, icount, vfrom, vcount;
};
std::vector<float> VS;
std::vector<uint16_t> IS;
Part BALL, EYEBALL, LEAF, WING, HOOP, HAZE, QUAD;

using P6 = std::array<float, 6>;

template <class F, class H>
Part surf(int nu, int nv, F f, H hint)
{
    const int base = int(VS.size() / 9), from = int(IS.size());
    const float e = 1e-3f;
    for (int j = 0; j <= nv; j++)
        for (int i = 0; i <= nu; i++) {
            const float u = float(i) / nu, v = float(j) / nv;
            const P6 p = f(u, v), pu = f(u + e, v), mu = f(u - e, v), pv = f(u, v + e), mv = f(u, v - e);
            const V3 a = {pu[0] - mu[0], pu[1] - mu[1], pu[2] - mu[2]}, b = {pv[0] - mv[0], pv[1] - mv[1], pv[2] - mv[2]};
            const V3 h = hint(p, u, v);
            float x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x;
            float l = sqrtf(x * x + y * y + z * z);
            if (l < 1e-7f) {
                x = h.x, y = h.y, z = h.z;
                l = sqrtf(x * x + y * y + z * z);
                if (l == 0) l = 1;
            } else if (x * h.x + y * h.y + z * h.z < 0) {
                l = -l;
            }
            VS.insert(VS.end(), {p[0], p[1], p[2], x / l, y / l, z / l, p[3], p[4], p[5]});
        }
    for (int j = 0; j < nv; j++)
        for (int i = 0; i < nu; i++) {
            const int a = base + j * (nu + 1) + i, b = a + nu + 1;
            IS.insert(IS.end(), {uint16_t(a), uint16_t(b), uint16_t(a + 1), uint16_t(a + 1), uint16_t(b), uint16_t(b + 1)});
        }
    return {from, int(IS.size()) - from, base, int(VS.size() / 9) - base};
}

// The wing's outline, measured off the drawing: s runs from the shoulder to the tip.
float lead(float s) { return .24f * (1 - ease(seg(s, .2f, .8f))) - .05f * sinf(PI * seg(s, .5f, 1.35f)); }
float trail(float s)
{
    float e = -.62f * sqrtf(std::max(0.f, 1 - powf(sat(s / 1.35f), 1.6f))) * (s < .3f ? sat(.85f + .5f * s) : 1);
    if (s < .5425f) e += .09f * (1 - sqrtf(fabsf(sinf(PI * (.41f - s) / .265f))));
    return e;
}

void shapes()
{
    if (!VS.empty()) return;
    auto ball = [](float u, float v) {
        const float a = u * TAU, f = sat(v) * PI, x = sinf(f) * cosf(a), y = cosf(f), z = sinf(f) * sinf(a);
        return P6{x, y, z, x, y, z};
    };
    auto ball_hint = [](const P6 &p, float, float) { return V3{p[0], p[1], p[2]}; };
    BALL = surf(30, 18, ball, ball_hint);
    EYEBALL = surf(16, 10, ball, ball_hint);  // the eyes are small: fewer facets
    LEAF = surf(10, 12,
                [](float u, float v) {
                    v = sat(v);
                    const float r = powf(sinf(PI * powf(v, .75f)), .9f), a = u * TAU;
                    return P6{r * cosf(a), v, r * sinf(a), r * cosf(a), v * 2 - 1, r * sinf(a)};
                },
                [](const P6 &p, float, float v) { return V3{p[0], v - .4f, p[2]}; });
    WING = surf(12, 40,
                [](float u, float v) {
                    const float s = mixf(-.3f, 1.35f, sat(v)), a = u * TAU, x = mixf(trail(s), lead(s), cosf(a) * .5f + .5f);
                    return P6{x, s, sinf(a) * .075f * (1 - .5f * sat(s / 1.35f)) * sat((1.35f - s) * 5), x, s, sinf(a)};
                },
                [](const P6 &p, float u, float) { return V3{cosf(u * TAU) * .4f, 0, p[5]}; });
    auto hoop = [](float r) {
        return surf(56, 4,
                    [r](float u, float v) {
                        const float a = u * TAU, q = v * TAU, k = 1 + r * cosf(q);
                        return P6{k * cosf(a), r * sinf(q), k * sinf(a), u, 0, 0};
                    },
                    [](const P6 &p, float u, float) { return V3{p[0] - cosf(u * TAU), p[1], p[2] - sinf(u * TAU)}; });
    };
    HOOP = hoop(.014f);
    HAZE = hoop(.05f);
    const int q = int(VS.size() / 9);
    const float corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (auto &c : corners) VS.insert(VS.end(), {c[0], c[1], 0, 0, 0, 1, c[0], c[1], 0});
    QUAD = {int(IS.size()), 6, q, 4};
    IS.insert(IS.end(), {uint16_t(q), uint16_t(q + 1), uint16_t(q + 2), uint16_t(q + 2), uint16_t(q + 1), uint16_t(q + 3)});
}

// ---- colours ----------------------------------------------------------------------------------
const V3 ONE{1, 1, 1}, BLUE{.45f, .5f, .84f}, WARM{.9f, .6f, .5f};
const V3 TAIL[2] = {hexc(0x1A2F9E), hexc(0x2C6BF5)}, TUFT[2] = {hexc(0x45BDFF), hexc(0x2E9BF5)},
         CREST[2] = {hexc(0x9AF8FF), hexc(0x4CC8FF)}, BEAK[2] = {hexc(0xFF9338), hexc(0xFFD76E)},
         CHIN[2] = {hexc(0xD9661F), hexc(0xF0862E)}, MOUTH[2] = {hexc(0x8A2A3A), hexc(0x5A1524)},
         GOLD[2] = {hexc(0xFFFBE0), hexc(0xFFCF4D)};
// The dark sheet's ink: orbit cream and cyan, the glow, and the sparks.
const V3 INK[5] = {hexc(0xFFF4C2), hexc(0x8BE9FF), {.3f, .55f, 1}, {1, 1, 1}, hexc(0xBFE9FF)};

// ---- springs: each joint's pull and how soon it settles ---------------------------------------
enum J { X, Y, YAW, PIT, ROL, SQ, WL, WR, WS, WC, WT, TF, TW, TS, EO, EH, ES, EQ, EW, BK, LX, LY, BL, RO, GL, OP, FA, FF, NJ };
const float KS[NJ][2] = {{70, .62f},  {70, .55f},  {90, .7f},   {90, .7f},   {90, .5f},   {260, .42f}, {130, .55f},
                         {130, .55f}, {60, .8f},   {110, .4f},  {90, .6f},   {150, .35f}, {120, .2f},  {90, .28f},
                         {420, 1},    {260, .8f},  {160, 1},    {200, .7f},  {200, .5f},  {900, .8f},  {240, .8f},
                         {240, .8f},  {30, 1},     {30, 1},     {40, 1},     {26, 1},     {40, 1},     {30, 1}};

struct Uniforms {
    int k = 0;
    float x = 1;
    V3 a = ONE, z = ONE, s = BLUE;
    float f[4] = {0, 0, 0, 1};
    float b[2] = {0, 0};
};

struct Vert {
    float sx, sy, sz, iw;
    float n[3], o[3], w[3];  // already divided by w
};

struct Bit {
    float x, y, vx, vy, t, d, s, r;
};

}  // namespace

struct Figure::Impl {
    int size, ss, rw;  // output size, supersampling, render width
    bool glow = true;
    std::vector<uint8_t> color;  // premultiplied RGBA at render width
    std::vector<uint8_t> halo;   // the glow, drawn once: each frame starts from it
    std::vector<float> depth;
    std::vector<Vert> verts;
    M4 view;
    // pose
    float P[NJ] = {}, Vv[NJ] = {}, T[NJ] = {};
    float clock = 0, since = 0, talk = 0, spin = 0, ph = 0, ring = 1, blink = 0, next_blink = 2, next_gaze = 1,
          next_fid = 6, lvl = 0, level_in = 0;
    float gaze[2] = {0, 0}, sp[3] = {1, 1, 0};
    Mood st = Mood::Idle;
    struct {
        bool on = false;
        Shot n = Shot::Hello;
        float t = 0, d = 0;
        int f = 0;
    } shot;
    struct {
        bool on = false;
        float k = 0, s = 1, t = 0;
        int f = 0;
    } fid;
    std::vector<Bit> bits;
    uint32_t seed = 12345;
    // uniforms that span a frame
    float uC[3] = {0, 0, 0}, uG[3] = {0, 0, 0}, uE[4] = {1, 0, 0, 1};

    float dice(float a, float b)
    {
        seed = uint32_t(uint64_t(seed) * 16807 % 2147483647);
        return a + float(seed) / 2147483647.f * (b - a);
    }

    // ---- shading -------------------------------------------------------------------------
    V3 lit(V3 b, V3 sh, V3 n, float g) const
    {
        const float d = dot(n, {-.42f, .72f, .55f}) * .5f + .5f, f = 1 - std::max(n.z, 0.f);
        V3 c = mixv(b * sh, b, smooth(.18f, .82f, d)) + b * (.1f * std::max(n.y, 0.f));
        c = c + V3{.45f, .82f, 1} * (f * f * f * .85f * smooth(-.3f, .5f, dot(n, {.55f, .45f, -.7f})));
        return c + ONE * (g * p40(std::max(dot(n, {-.24f, .41f, .88f}), 0.f)));
    }
    float meet(float k, const float *w) const
    {
        const float dist = sqrtf((w[0] - uC[0]) * (w[0] - uC[0]) + (w[1] - uC[1]) * (w[1] - uC[1]) + (w[2] - uC[2]) * (w[2] - uC[2]));
        return mixf(1, smooth(.8f, 1.5f, dist) * .5f + .5f, k);
    }

    // Returns premultiplied colour and alpha.
    void shade(const Uniforms &u, V3 n, const float *o, const float *w, float out[4]) const
    {
        const float x = u.x;
        V3 c{0, 0, 0};
        float alpha = 1;
        switch (u.k) {
        case 0:
            c = lit(mixv(u.z, u.a, smooth(-1, 1, o[1])), u.s, n, u.f[0]) * meet(u.f[1], w) + u.a * u.f[2];
            break;
        case 1: {
            const float qx = o[0], qy = o[1], t = o[1] * .5f + .5f;
            V3 b = mixv({.169f, .361f, .949f}, {.208f, .698f, 1}, smooth(0, .55f, t));
            b = mixv(b, {.49f, .953f, 1}, smooth(.5f, 1, t));
            // the heart-shaped face: two lobes and a wide chin, run together
            const float l = len2(fabsf(qx) - .36f, qy - .15f) - .47f;
            const float wv = (len2(qx / .77f, (qy + .06f) / .64f) - 1) * .64f;
            const float k = clampf(.5f + 5 * (wv - l), 0, 1), d = mixf(wv, l, k) - .1f * k * (1 - k);
            const float m = (1 - smooth(-x, x, d)) * stepf(0, o[2]);
            V3 fc = mixv({1, .906f, .769f}, {1, .996f, .976f}, smooth(-.7f, .6f, qy));
            fc = mixv(fc, {1, .5f, .66f}, uG[2] * .8f * (1 - smooth(.3f, 1.25f, len2((fabsf(qx) - .52f) / .17f, (qy + .3f) / .115f))));
            fc = fc * (.84f + .16f * smooth(.27f, .43f, len2(fabsf(qx) - .36f, qy - .07f)));
            fc = fc * (.87f + .13f * smooth(.04f, .2f, len2(qx, (qy + .27f) * 1.3f)));
            const float f = 1 - std::max(n.z, 0.f);
            b = mixv(b, {.082f, .196f, .66f}, .4f * f * f);
            const V3 body = lit(b, {.4f, .47f, .8f}, n, .3f) + ONE * (.2f * p5(std::max(dot(n, {-.5f, .7f, .51f}), 0.f)));
            c = mixv(body, lit(fc, {.9f, .82f, .86f}, n, .1f), m);
            break;
        }
        case 2: {
            const float ex = o[0], ey = o[1];
            const float g = len2(ex - uG[0] * .3f, ey - (uG[1] * .3f - .76f)) / 1.64f, f = 1 - std::max(n.z, 0.f);
            c = mixv({.36f, .91f, 1}, {.165f, .28f, .77f}, smooth(0, .34f, g));
            c = mixv(c, {.07f, .082f, .263f}, smooth(.34f, .72f, g));
            c = mixv(c, {.04f, .047f, .165f}, smooth(.72f, 1, g));
            c = c + V3{.2f, .4f, .75f} * (f * f * .5f);
            const float hx = ex - uG[0] * .1f, hy = ey - uG[1] * .1f;
            c = mixv(c, ONE, (1 - uE[1]) * (1 - smooth(.36f - x, .36f + x, len2(hx + .33f, hy - .39f))));
            c = mixv(c, ONE, .92f * (1 - uE[1]) * (1 - smooth(.15f - x, .15f + x, len2(hx - .36f, hy + .33f))));
            // lids: the upper one comes down to blink and slopes when sorry; a smile closes both to one upturned arc
            const float e2 = ex * ex, arc = .1f - .55f * e2;
            const float up = mixf(mixf(-.5f, 1.3f, uE[0]) + .25f * e2 - uE[2] * (.45f + .5f * uE[3] * ex), arc + .28f - .25f * e2, uE[1]);
            const float lo = mixf(mixf(-.55f + .2f * e2, -1.5f, uE[0]), arc, uE[1]);
            const float dl = std::min(up - ey, ey - lo);
            V3 sk = lit({1, .95f, .87f}, {.9f, .82f, .86f}, n, .1f);
            sk = mixv(sk, {.2f, .16f, .36f}, .75f * (1 - smooth(.03f, .1f + x, fabsf(ey - up))) * std::max(1 - uE[0] * uE[0], uE[2]));
            c = c * mixf(.55f, 1, smooth(0, .3f, dl));
            c = mixv(sk, c, smooth(-x, x, dl));
            break;
        }
        case 3: {
            const float s = o[1], h = o[0];
            const float le = .24f * (1 - smooth(.2f, .8f, s)) - .05f * sinf(3.1416f * clampf((s - .5f) / .85f, 0, 1));
            const float lw = mixf(-.29f, -.12f, smooth(.5f, 1.03f, s));
            c = mixv({.173f, .42f, .96f}, {.106f, .22f, .72f}, clampf(s / 1.35f, 0, 1));
            c = mixv(c, mixv({.373f, .84f, 1}, {.204f, .47f, .965f}, clampf(s, 0, 1)),
                     smooth(-x, x, h - lw) * smooth(-x, x, le - .07f - h) * (1 - smooth(1, 1.03f, s)));
            c = mixv(c, {.435f, .706f, 1},
                     .5f * (1 - smooth(.01f, .016f + x, fabsf(fract((.41f - s) / .265f + .5f) - .5f) * .265f)) * stepf(h, lw) *
                         stepf(s, .5f) * stepf(-.2f, s));
            c = lit(c, {.45f, .5f, .84f}, n, .2f) * meet(1, w);
            break;
        }
        case 4: {
            const float d = fract(u.f[0] - o[0]);
            const float a = mixf(p3(1 - d), 1, u.f[2]) * u.f[1];
            c = mixv(u.z, u.a, p8(1 - d)) * a;
            alpha = a * u.f[3];
            break;
        }
        case 5: {
            const float px = fabsf(o[0]) * 1.7f, py = fabsf(o[1]) * 1.7f;
            const float k = 1 - smooth(1 - x * 3, 1, sqrtf(px) + sqrtf(py)), g = expf(-(px * px + py * py) * 1.5f) * .5f;
            c = (mixv(u.z, u.a, o[1] * .85f + .5f) * k + u.z * g) * u.f[1];
            alpha = (k + g * u.f[3]) * u.f[1];
            break;
        }
        default: {  // 6: a soft glow
            const float a = expf(-(o[0] * o[0] + o[1] * o[1]) * 4.5f) * u.f[1];
            c = u.a * a;
            alpha = a * u.f[3];
            break;
        }
        }
        out[0] = c.x, out[1] = c.y, out[2] = c.z, out[3] = alpha;
    }

    // ---- rasterizer ----------------------------------------------------------------------
    void put(const Part &mesh, const M4 &m, const Uniforms &u, bool blend, bool depth_test)
    {
        const M3 nm = nmat(m);
        const M4 mvp = mul(view, m);
        if (int(verts.size()) < mesh.vcount) verts.resize(mesh.vcount);
        const float half = rw * .5f;
        for (int i = 0; i < mesh.vcount; ++i) {
            const float *s = &VS[size_t(mesh.vfrom + i) * 9];
            float px = s[0], py = s[1], pz = s[2], nx = s[3], ny = s[4], nz = s[5];
            if (u.b[0] != 0 || u.b[1] != 0) {  // bend along the length
                const float yy = py * py;
                float a = u.b[0] * yy, c = cosf(a), sn = sinf(a);
                float t1 = py * c - pz * sn, t2 = py * sn + pz * c;
                py = t1, pz = t2;
                t1 = ny * c - nz * sn, t2 = ny * sn + nz * c;
                ny = t1, nz = t2;
                a = u.b[1] * yy, c = cosf(a), sn = sinf(a);
                t1 = px * c - py * sn, t2 = px * sn + py * c;
                px = t1, py = t2;
                t1 = nx * c - ny * sn, t2 = nx * sn + ny * c;
                nx = t1, ny = t2;
            }
            Vert &v = verts[i];
            const float wx = m[0] * px + m[4] * py + m[8] * pz + m[12], wy = m[1] * px + m[5] * py + m[9] * pz + m[13],
                        wz = m[2] * px + m[6] * py + m[10] * pz + m[14];
            const float cx = mvp[0] * px + mvp[4] * py + mvp[8] * pz + mvp[12], cy = mvp[1] * px + mvp[5] * py + mvp[9] * pz + mvp[13],
                        cz = mvp[2] * px + mvp[6] * py + mvp[10] * pz + mvp[14], cw = mvp[3] * px + mvp[7] * py + mvp[11] * pz + mvp[15];
            const float iw = 1 / cw;
            v.sx = (cx * iw + 1) * half;
            v.sy = (1 - cy * iw) * half;
            v.sz = cz * iw;
            v.iw = iw;
            v.n[0] = (nm[0] * nx + nm[3] * ny + nm[6] * nz) * iw;
            v.n[1] = (nm[1] * nx + nm[4] * ny + nm[7] * nz) * iw;
            v.n[2] = (nm[2] * nx + nm[5] * ny + nm[8] * nz) * iw;
            v.o[0] = s[6] * iw, v.o[1] = s[7] * iw, v.o[2] = s[8] * iw;
            v.w[0] = wx * iw, v.w[1] = wy * iw, v.w[2] = wz * iw;
        }
        const uint16_t *idx = &IS[size_t(mesh.ifrom)];
        for (int t = 0; t < mesh.icount; t += 3)
            tri(verts[idx[t] - mesh.vfrom], verts[idx[t + 1] - mesh.vfrom], verts[idx[t + 2] - mesh.vfrom], u, blend, depth_test);
    }

    void tri(const Vert &a, const Vert &b, const Vert &c, const Uniforms &u, bool blend, bool depth_test)
    {
        // Opaque surfaces facing away are hidden behind their own front:
        // skip them before any pixel work (normals are divided by w > 0).
        if (!blend && a.n[2] < -.25f * a.iw && b.n[2] < -.25f * b.iw && c.n[2] < -.25f * c.iw) return;
        const float area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
        if (!(fabsf(area) >= 1e-6f && fabsf(area) < 1e7f)) return;  // degenerate, or behind the eye
        const int x0 = std::max(0, int(floorf(std::min({a.sx, b.sx, c.sx})))), x1 = std::min(rw - 1, int(ceilf(std::max({a.sx, b.sx, c.sx}))));
        const int y0 = std::max(0, int(floorf(std::min({a.sy, b.sy, c.sy})))), y1 = std::min(rw - 1, int(ceilf(std::max({a.sy, b.sy, c.sy}))));
        if (x0 > x1 || y0 > y1) return;
        const float inv = 1 / area;
        for (int y = y0; y <= y1; ++y) {
            const float py = y + .5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = x + .5f;
                const float w0 = ((b.sx - px) * (c.sy - py) - (b.sy - py) * (c.sx - px)) * inv;
                const float w1 = ((c.sx - px) * (a.sy - py) - (c.sy - py) * (a.sx - px)) * inv;
                const float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = w0 * a.sz + w1 * b.sz + w2 * c.sz;
                float &zb = depth[size_t(y) * rw + x];
                if (depth_test && z >= zb) continue;
                const float iw = 1 / (w0 * a.iw + w1 * b.iw + w2 * c.iw);
                auto lerp = [&](const float *pa, const float *pb, const float *pc, float *out) {
                    for (int i = 0; i < 3; ++i) out[i] = (w0 * pa[i] + w1 * pb[i] + w2 * pc[i]) * iw;
                };
                float n[3], o[3], w[3];
                lerp(a.n, b.n, c.n, n);
                lerp(a.o, b.o, c.o, o);
                lerp(a.w, b.w, c.w, w);
                const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                const V3 nn = nl > 0 ? V3{n[0] / nl, n[1] / nl, n[2] / nl} : V3{0, 0, 1};
                float col[4];
                shade(u, nn, o, w, col);
                uint8_t *dst = &color[(size_t(y) * rw + x) * 4];
                auto to8 = [](float v) { return uint8_t(v <= 0 ? 0 : v >= 1 ? 255 : int(v * 255 + .5f)); };
                if (blend) {
                    const float keep = 1 - clampf(col[3], 0, 1);
                    for (int i = 0; i < 3; ++i) dst[i] = to8(col[i] + dst[i] / 255.f * keep);
                    dst[3] = to8(col[3] + dst[3] / 255.f * keep);
                } else {
                    dst[0] = to8(col[0]), dst[1] = to8(col[1]), dst[2] = to8(col[2]), dst[3] = 255;
                    zb = z;
                }
            }
        }
    }

    // ---- motion ----------------------------------------------------------------------------
    void aim()
    {
        const float o = P[OP], u = shot.on ? clock - shot.t : 0, s = clock - since;
        const Mood now = st == Mood::Idle && clock < talk ? Mood::Talk : st;
        // at rest the wings are spread like the drawing
        float x = 0, y = 0, yaw = gaze[0] * .25f, pit = -gaze[1] * .12f, rol = 0, sq = 0, wl = mixf(-.2f, .02f, o), wr = wl,
              ws = mixf(.85f, .1f, o), wc = 0, wt = 0, tf = 0, eo = 1, eh = 0, es = 0, eq = 0, ew = 0, bk = 0, lx = gaze[0],
              ly = gaze[1], bl = .7f, ro = o * .55f, gl = .4f, fa = mixf(.06f, .09f, o), ff = 8;
        spin = 0;
        if (now == Mood::Listen) {
            lvl = mixf(lvl, level_in, .3f);
            const float lv = lvl;
            yaw = .4f, rol = -.13f, x = .08f, pit = .05f, ew = 1, tf = 1, wl = wr = mixf(-.05f, .12f, o), ws = mixf(.45f, .1f, o);
            fa = .025f, ff = 24, ro = .45f + lv * .55f, sq = lv * .07f, gl = .5f + lv * .5f, lx = .55f, ly = -.15f, bl = .8f;
        } else if (now == Mood::Think) {
            yaw = -.28f + .1f * sinf(s * .8f), pit = -.2f + .04f * sinf(s * 1.7f), rol = .16f * sinf(s * .65f + .5f), y = .03f;
            ro = 1, fa = .045f, ff = 6, tf = .35f, gl = .55f + .15f * sinf(s * 3);
            lx = clampf(sp[0] * .5f, -.75f, .75f), ly = clampf(.25f + sp[1] * .5f, -.3f, .8f);
        } else if (now == Mood::Talk) {
            const float e = sat(sinf(s * 13 + 1.7f * sinf(s * 4.1f)) * .9f + .2f) * (.55f + .45f * sinf(s * 2.7f + 1));
            bk = e, pit = .04f + e * .06f, yaw = .22f + .05f * sinf(s * 2.1f), rol = .05f * sinf(s * 3.3f), y = e * .02f, tf = .25f * e;
            lx = .4f, ly = 0, eh = .15f, wl += .14f * std::max(0.f, sinf(s * 2.3f)), wr += .14f * std::max(0.f, sinf(s * 2.3f + 2.4f));
            ws = mixf(.6f, .1f, o), gl = .5f;
        } else if (fid.on) {
            const float f = clock - fid.t;
            if (f > 1.2f) fid.on = false;
            else if (fid.k < 1) fa = .36f, ff = 26, y = .06f;
            else if (fid.k < 2) rol = .22f * fid.s;
            else if (fid.k < 3) lx = .6f, ly = -.5f, yaw = .2f, pit = .08f;
            else if (!fid.f) fid.f = 1, Vv[TW] += 8 * fid.s, blink = .001f;
        }
        if (shot.on) {
            auto once = [&](int b, auto fn) {
                if (!(shot.f & b)) {
                    shot.f |= b;
                    fn();
                }
            };
            switch (shot.n) {
            case Shot::Hello:
                eh = seg(u, .1f, .25f) * (1 - seg(u, .95f, 1.2f));
                if (u > .15f && u < .95f) wr = .3f + .38f * sinf(u * 24), wt = .2f;
                ws = .05f, rol = .1f, yaw = .12f, bl = 1, bk = u > .12f && u < .4f ? .6f : 0, gl = .7f, tf = .6f;
                break;
            case Shot::Send:
                once(1, [&] { Vv[Y] += 1.1f; });
                lx = .8f, ly = .3f, yaw = .34f, pit = -.08f, tf = .8f, ew = .6f;
                break;
            case Shot::Pleased: {
                const float k = seg(u, .18f, .66f);
                if (u < .18f) {
                    sq = -.14f, y = -.1f, wl = wr = -.5f, eo = .8f;
                } else {
                    once(1, [&] { Vv[SQ] += 2.4f; });
                    y = .7f * sinf(PI * k), spin = TAU * ease(k), wl = wr = k < 1 ? .42f : .15f, ws = .02f, fa = k < 1 ? .2f : .1f;
                    ff = 32, eh = u < 1.6f ? 1 : 0, bl = 1, gl = 1, tf = .8f;
                    if (k > .45f) once(2, [&] { burst(9); });
                    if (k >= 1) {
                        once(4, [&] { Vv[SQ] -= 2.6f, Vv[Y] -= .8f; });
                        rol = .1f * sinf((u - .66f) * 15) * std::max(0.f, 1 - (u - .66f) / .8f), spin = 0;
                    }
                }
                break;
            }
            case Shot::Sorry: {
                const float w = seg(u, 0, .35f) * (1 - seg(u, shot.d - .6f, shot.d));
                y = -.13f * w, pit = .33f * w, tf = -w, wl = wr = mixf(wl, -1.15f, w), wc = .4f * w, ws = mixf(ws, .3f, w), es = w;
                eo = 1 - .22f * w, ly = -.6f * w, lx = 0, bl = .3f, gl = .15f, fa = .02f;
                yaw = .2f * sinf((u - .6f) * 6.5f) * seg(u, .6f, .8f) * (1 - seg(u, 1.9f, 2.2f)), bk = u > .3f && u < .75f ? .3f : 0;
                break;
            }
            case Shot::Confused: {
                const float w = 1 - seg(u, shot.d - .5f, shot.d), side = u < 1.25f ? 1.f : -1.f;
                rol = (u < 1.25f ? .42f : -.3f) * w, yaw = -.12f * side * w, eq = .5f * side * w, ew = .5f * w;
                once(1, [&] { Vv[TF] += 9, Vv[TW] += 5; });
                wl += .12f * w, wr -= .18f * w, lx = .45f * sinf(u * 5.5f) * w, ly = .15f * w, tf = .4f * w;
                if (fabsf(u - .5f) < .07f || fabsf(u - .78f) < .07f) eo = 0;
                break;
            }
            case Shot::Poke:
                if (u < .13f) {
                    sq = -.3f, eo = 0;
                } else {
                    once(1, [&] { Vv[Y] += 1.7f, Vv[SQ] += 2.5f, burst(4); });
                    eo = u < .3f ? 0 : 1, eh = u > .3f ? 1 - seg(u, .7f, .9f) : 0, tf = 1, rol = .14f * sinf(u * 20) * std::max(0.f, 1 - u), bl = 1;
                }
                break;
            }
            if (u > shot.d) shot.on = false;
        }
        const float t[NJ] = {x, y, yaw, pit, rol, sq, wl, wr, ws, wc, wt, tf, 0, 0, eo, eh, es, eq, ew, bk, lx, ly, bl, ro, gl, 1, fa, ff};
        memcpy(T, t, sizeof(T));
        // what trails behind: the tuft and the tail lag the body
        T[TW] = -Vv[X] * .5f - Vv[YAW] * .25f - Vv[ROL] * .4f;
        T[TS] = Vv[X] * .4f + Vv[YAW] * .2f + .07f * sinf(clock * 1.3f);
    }

    void burst(int n)
    {
        for (int i = 0; i < n; i++)
            bits.push_back({P[X] + dice(-.5f, .5f), P[Y] + dice(0, .7f), dice(-2.2f, 2.2f), dice(.8f, 2.8f), 0, dice(.55f, 1.1f),
                            dice(.1f, .24f), dice(0, 3)});
    }

    void step(float dt)
    {
        clock += dt;
        ph += P[FF] * dt;
        ring += dt * (st == Mood::Think ? 3.6f : st == Mood::Listen ? 2 : .7f);
        if (blink > 0) {
            blink += dt / .15f;
            if (blink >= 1) blink = 0;
        } else if (clock > next_blink) {
            blink = .001f;
            next_blink = clock + (dice(0, 1) < .2f ? .28f : dice(2.2f, 5.5f));
        }
        if (clock > next_gaze) {
            if (dice(0, 1) < .45f) gaze[0] = gaze[1] = 0;
            else gaze[0] = dice(-.5f, .5f), gaze[1] = dice(-.2f, .3f);
            next_gaze = clock + dice(.8f, 2.8f);
        }
        if (st == Mood::Idle && clock > next_fid) {
            fid.on = true, fid.k = dice(0, 4), fid.s = dice(0, 1) < .5f ? -1.f : 1.f, fid.t = clock, fid.f = 0;
            next_fid = clock + dice(5, 10);
        }
        aim();
        for (int i = 0; i < 2; i++)
            for (int k = 0; k < NJ; k++) {
                Vv[k] += ((T[k] - P[k]) * KS[k][0] - 2 * KS[k][1] * sqrtf(KS[k][0]) * Vv[k]) * dt / 2;
                P[k] += Vv[k] * dt / 2;
            }
        for (auto &b : bits) b.t += dt, b.x += b.vx * dt, b.y += b.vy * dt, b.vy -= 3.5f * dt;
        bits.erase(std::remove_if(bits.begin(), bits.end(), [](const Bit &b) { return b.t >= b.d; }), bits.end());
    }

    void bake_halo()
    {
        // The glow it floats in, at rest (the web figure moves it a little
        // with the bird and brightens it while listening; here it stays put).
        std::fill(color.begin(), color.end(), 0);
        std::fill(depth.begin(), depth.end(), 1e30f);
        Uniforms u;
        u.k = 6;
        u.a = INK[2];
        u.f[1] = .4f * .5f, u.f[3] = .15f;
        put(QUAD, trs(0, .05f, -2, 0, 0, 0, 2.6f), u, true, false);
        halo = color;
    }

    void draw()
    {
        if (glow && halo.size() == color.size()) memcpy(color.data(), halo.data(), color.size());
        else std::fill(color.begin(), color.end(), 0);
        std::fill(depth.begin(), depth.end(), 1e30f);
        const float flap = P[FA] * sinf(ph), by = P[Y] + .045f * sinf(clock * 1.9f) - P[FA] * .1f * cosf(ph),
                    bx = P[X] + .02f * sinf(clock * 1.1f), q = 1 + P[SQ] + .012f * sinf(clock * 2.6f);
        const M4 R = trs(bx, by, 0, P[PIT], P[YAW] + spin, P[ROL], 1 / sqrtf(q), q, 1 / sqrtf(q));
        auto B = [&](const M4 &m) { return mul(R, m); };
        const float bl = sinf(PI * blink);
        uC[0] = bx, uC[1] = by, uC[2] = 0;
        uG[0] = P[LX], uG[1] = P[LY], uG[2] = P[BL];
        const float aa = 4.8f / rw;
        auto U = [&](int k, float x = 1) {
            Uniforms u;
            u.k = k;
            u.x = x * aa;
            return u;
        };
        // the glow is baked into the background (bake_halo)
        if (false) {
            Uniforms u = U(6);
            u.a = INK[2];
            u.f[1] = P[GL] * .5f, u.f[3] = .15f;
            put(QUAD, trs(bx * .7f, by * .7f + .05f, -2, 0, 0, 0, 2.6f), u, true, false);
        }
        put(BALL, B(trs(0, 0, 0, 0, 0, 0, 1, .98f, .95f)), U(1), false, true);
        const float tf = P[TF], low = std::max(0.f, -tf);
        for (int si = 0; si < 2; ++si) {
            const float s = si ? 1.f : -1.f;
            // an eye, set into the face and turned to look out along it
            const float open = sat(P[EO] - bl - std::max(0.f, s * P[EQ])), wide = .27f * (1 + .1f * P[EW]);
            uE[0] = open, uE[1] = P[EH], uE[2] = P[ES], uE[3] = s;
            put(EYEBALL, B(trs(s * .324f, .063f, .795f, -.07f, s * .369f, 0, wide, wide * (1 + .06f * P[EW]), .15f)), U(2, 1 / wide), false, true);
            // a wing: lifted at the shoulder, swept back, twisted, bent by its own beat
            const M4 W = trs(-.7f, .05f, -.2f, P[WT], -P[WS], .89f - (s < 0 ? P[WL] : P[WR]) - flap, 1);
            Uniforms w = U(3);
            w.b[0] = -P[FA] * 1.3f * sinf(ph - 1.1f), w.b[1] = P[WC];
            put(WING, B(s < 0 ? W : mul(MIRROR, W)), w, false, true);
            Uniforms tail = U(0);
            tail.a = TAIL[0], tail.z = TAIL[1], tail.f[0] = .15f, tail.f[1] = 1, tail.b[1] = P[TS] * .8f;
            put(LEAF, B(trs(s * .1f, -.8f, -.22f, .25f, 0, PI + s * .52f + P[TS] * .5f, .16f, .7f, .05f)), tail, false, true);
            Uniforms tuft = U(0);
            tuft.a = TUFT[0], tuft.z = TUFT[1], tuft.f[0] = .2f, tuft.f[1] = .6f, tuft.b[1] = P[TW] * 1.1f;
            put(LEAF, B(trs(s * .05f, .88f, -.06f, -.1f + low * .5f - tf * .1f, 0, -s * (.5f + tf * .18f) + P[TW] * .4f, .085f,
                            .44f * (1 - low * .2f), .045f)),
                tuft, false, true);
        }
        Uniforms crest = U(0);
        crest.a = CREST[0], crest.z = CREST[1], crest.f[0] = .25f, crest.f[1] = .6f, crest.b[1] = P[TW] * 1.2f;
        put(LEAF, B(trs(0, .9f, -.03f, -.12f + low * .5f - tf * .12f, 0, P[TW] * .5f, .1f, .56f * (1 + tf * .1f), .05f)), crest, false, true);
        // the beak opens on a hinge, with the dark of the mouth behind it
        Uniforms beak = U(0);
        beak.a = BEAK[0], beak.z = BEAK[1], beak.s = WARM, beak.f[0] = .55f;
        put(LEAF, B(trs(0, -.15f, .84f, PI / 2 + .42f - P[BK] * .2f, 0, 0, .165f, .36f, .095f)), beak, false, true);
        Uniforms chin = U(0);
        chin.a = CHIN[0], chin.z = CHIN[1], chin.s = WARM, chin.f[0] = .2f;
        put(LEAF, B(trs(0, -.21f, .82f, PI / 2 + .85f + P[BK] * .8f, 0, 0, .12f, .27f, .065f)), chin, false, true);
        if (P[BK] > .05f) {
            Uniforms mouth = U(0);
            mouth.a = MOUTH[0], mouth.z = MOUTH[1], mouth.s = WARM;
            put(EYEBALL, B(trs(0, -.21f, .9f, 0, 0, 0, .085f, .02f + .06f * P[BK], .05f)), mouth, false, true);
        }
        // the orbit and its spark, behind the body where they pass behind it
        const M4 O = mul(trs(bx, by, 0, 0, 0, 0, 1), trs(0, 0, 0, .5f, 0, -.3f, mixf(1.34f, 1.75f, P[OP])));
        const float a = fmodf(ring, TAU), cx = cosf(a), sz = sinf(a);
        sp[0] = O[0] * cx + O[8] * sz + O[12] - bx, sp[1] = O[1] * cx + O[9] * sz + O[13] - by, sp[2] = O[2] * cx + O[10] * sz;
        if (P[RO] > .01f) {
            const float t = a / TAU, ad = .3f;
            Uniforms haze = U(4);
            haze.a = INK[0], haze.z = INK[1], haze.f[0] = t, haze.f[1] = P[RO] * .3f, haze.f[3] = ad;
            put(HAZE, O, haze, true, true);
            Uniforms hoop = haze;
            hoop.f[1] = P[RO];
            put(HOOP, O, hoop, true, true);
            const float z = .3f * (1 + .15f * sinf(clock * 9));
            Uniforms spark = U(5, 1 / z);
            spark.a = GOLD[0], spark.z = GOLD[1], spark.f[1] = std::min(1.f, P[RO] * 1.6f), spark.f[3] = .4f;
            put(QUAD, trs(sp[0] + bx, sp[1] + by, sp[2], 0, 0, clock * 1.5f, z), spark, true, true);
        }
        for (const Bit &b : bits) {
            const float z = b.s * sinf(PI * b.t / b.d);
            Uniforms bit = U(5, 1 / std::max(z, .01f));
            bit.a = INK[3], bit.z = INK[4], bit.f[1] = 1, bit.f[3] = .4f;
            put(QUAD, trs(b.x, b.y, 1.3f, 0, 0, b.r + b.t * 3, z), bit, true, true);
        }
    }
};

Figure::Figure(int size, int ss) : d(new Impl)
{
    shapes();
    d->size = size;
    d->ss = ss < 1 ? 1 : ss;
    d->rw = size * d->ss;
    d->color.resize(size_t(d->rw) * d->rw * 4);
    d->depth.resize(size_t(d->rw) * d->rw);
    // a long lens: the figure fills 4.8 body sizes across, barely foreshortened
    const float t = 14 / 2.4f;
    const M4 proj = {t, 0, 0, 0, 0, t, 0, 0, 0, 0, -28.f / 16, -1, 0, 0, -2 * 22 * 6.f / 16, 0};
    d->view = mul(proj, trs(0, 0, -14, 0, 0, 0, 1));
    d->P[EO] = 1;
    d->P[OP] = 1;
    d->bake_halo();
}

Figure::~Figure() = default;

void Figure::set_mood(Mood mood)
{
    if (mood != d->st) {
        d->st = mood;
        d->since = d->clock;
    }
}

void Figure::shoot(Shot shot)
{
    static const float LEN[] = {1.3f, .55f, 2, 3, 2.4f, .95f};
    d->shot.on = true;
    d->shot.n = shot;
    d->shot.t = d->clock;
    d->shot.d = LEN[int(shot)];
    d->shot.f = 0;
}

void Figure::react(Shot mood, bool failed, int text_length)
{
    shoot(failed ? Shot::Sorry : mood);
    d->talk = d->clock + (failed ? .9f : clampf(text_length / 26.f, .9f, 3.4f)) + (d->shot.on ? d->shot.d * .6f : 0);
}

void Figure::set_glow(bool glow) { d->glow = glow; }
void Figure::set_level(float level) { d->level_in = sat(level); }
void Figure::step(float dt) { d->step(std::min(dt, .034f)); }
int Figure::size() const { return d->size; }

void Figure::render(uint8_t *bgra)
{
    d->draw();
    const int ss = d->ss, rw = d->rw, n = ss * ss;
    for (int y = 0; y < d->size; ++y)
        for (int x = 0; x < d->size; ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i) {
                    const uint8_t *s = &d->color[((size_t(y) * ss + j) * rw + size_t(x) * ss + i) * 4];
                    r += s[0], g += s[1], b += s[2], a += s[3];
                }
            uint8_t *o = &bgra[(size_t(y) * d->size + x) * 4];
            a /= n;
            if (a == 0) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            // straight alpha for LVGL
            o[0] = uint8_t(std::min(255, b / n * 255 / a));
            o[1] = uint8_t(std::min(255, g / n * 255 / a));
            o[2] = uint8_t(std::min(255, r / n * 255 / a));
            o[3] = uint8_t(a);
        }
}

}  // namespace swifty
