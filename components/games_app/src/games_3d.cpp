// SPDX-License-Identifier: Apache-2.0
// The games' shared 3D engine (games_3d.h).
#include "games_3d.h"

namespace games {

void Camera::look_at(V3 eye, V3 target, float roll)
{
    pos = eye;
    fwd = norm(target - eye);
    V3 r = cross({0, 1, 0}, fwd);
    if (length(r) < 1e-4f) {
        r = {1, 0, 0};  // looking straight up or down
    }
    r = norm(r);
    const V3 u = cross(fwd, r);
    const float c = cosf(roll), s = sinf(roll);
    right = r * c + u * s;
    up = u * c - r * s;
}

bool Camera::screen(V3 world, P2 *out, float *depth) const
{
    const V3 c = to_cam(world);
    if (c.z < near) {
        return false;
    }
    *out = project(c);
    if (depth) {
        *depth = c.z;
    }
    return true;
}

void Camera::poly(uint16_t *fb, const V3 *v, int n, uint16_t color) const
{
    P2 out[16];
    int m = 0;
    for (int i = 0; i < n && m < 15; ++i) {
        const V3 &a = v[i], &b = v[(i + 1) % n];
        const bool ia = a.z >= near, ib = b.z >= near;
        if (ia) {
            out[m++] = project(a);
        }
        if (ia != ib) {
            const float t = (near - a.z) / (b.z - a.z);
            out[m++] = project({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, near});
        }
    }
    fill_poly(fb, out, m, color, transposed);
}

void Camera::poly_world(uint16_t *fb, const V3 *world, int n, uint16_t color) const
{
    V3 c[16];
    n = std::min(n, 16);
    for (int i = 0; i < n; ++i) {
        c[i] = to_cam(world[i]);
    }
    poly(fb, c, n, color);
}

void Camera::edge(uint16_t *fb, V3 a, V3 b, uint16_t color) const
{
    if (a.z < near && b.z < near) {
        return;
    }
    if (a.z < near) {
        const float t = (near - a.z) / (b.z - a.z);
        a = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, near};
    } else if (b.z < near) {
        const float t = (near - b.z) / (a.z - b.z);
        b = {b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t, near};
    }
    const P2 p = project(a), q = project(b);
    line(fb, p.x, p.y, q.x, q.y, color, transposed);
}

void Camera::glow_at(uint16_t *fb, V3 world, float radius, uint16_t color, float core, float max_px) const
{
    P2 p;
    float z;
    if (!screen(world, &p, &z)) {
        return;
    }
    glow(fb, p.x, p.y, std::min(max_px, focal * radius / z), color, core, transposed);
}

V3 pose_point(const Pose &pose, V3 m)
{
    const float cr = cosf(pose.roll), sr = sinf(pose.roll);
    m = {m.x * cr - m.y * sr, m.x * sr + m.y * cr, m.z};
    const float cp = cosf(pose.pitch), sp = sinf(pose.pitch);
    m = {m.x, m.y * cp + m.z * sp, -m.y * sp + m.z * cp};
    const float cy = cosf(pose.yaw), sy = sinf(pose.yaw);
    m = {m.x * cy + m.z * sy, m.y, -m.x * sy + m.z * cy};
    return pose.pos + m * pose.scale;
}

void draw_mesh(uint16_t *fb, const Camera &cam, const Mesh &mesh, const Pose &pose, const MeshStyle &style)
{
    V3 c[32];
    const int nv = std::min(mesh.nverts, 32);
    V3 middle{0, 0, 0};
    for (int i = 0; i < nv; ++i) {
        c[i] = cam.to_cam(pose_point(pose, mesh.verts[i]));
        middle = middle + c[i] * (1.0f / float(nv));
    }
    struct Face {
        float z;
        int i;
    } order[48];
    const int nt = std::min(mesh.ntris, 48);
    for (int i = 0; i < nt; ++i) {
        order[i] = {c[mesh.tris[i][0]].z + c[mesh.tris[i][1]].z + c[mesh.tris[i][2]].z, i};
    }
    std::sort(order, order + nt, [](const Face & a, const Face & b) {
        return a.z > b.z;
    });
    const V3 light = norm(style.light);
    for (int k = 0; k < nt; ++k) {
        const int f = order[k].i;
        const V3 &a = c[mesh.tris[f][0]], &b = c[mesh.tris[f][1]], &d = c[mesh.tris[f][2]];
        if (a.z < cam.near || b.z < cam.near || d.z < cam.near) {
            continue;
        }
        // the outward normal (away from the mesh's middle); faces turned away are skipped
        V3 n = norm(cross(b - a, d - a));
        const V3 g = (a + b + d) * (1.0f / 3);
        if (dot(n, g - middle) < 0) {
            n = n * -1;
        }
        if (dot(n, g) >= 0 && !style.two_sided) {
            continue;
        }
        const float lit = style.ambient + (1 - style.ambient) * fabsf(dot(n, light));
        const V3 t[3] = {a, b, d};
        cam.poly(fb, t, 3, shade(mesh.colors[f], lit));
        if (style.outline) {
            const uint16_t trim = f < style.split ? style.trim : style.trim2;
            cam.edge(fb, a, b, trim);
            cam.edge(fb, b, d, trim);
            cam.edge(fb, d, a, trim);
        }
    }
}

void Particles::burst(V3 at, int count, float speed, uint16_t color, float life, float size, Rng &rng, V3 drift)
{
    for (int i = 0; i < count; ++i) {
        // a random direction, roughly even over the sphere
        const float z = rng.range(-1, 1), a = rng.range(0, TAU), r = sqrtf(std::max(0.0f, 1 - z * z));
        const V3 dir{r * cosf(a), z, r * sinf(a)};
        P part{at, dir * (speed * rng.range(0.4f, 1.0f)) + drift, 0, life * rng.range(0.6f, 1.0f), size, color};
        if (n_ < MAX) {
            p_[n_++] = part;
        } else {
            p_[rng.below(MAX)] = part;  // full: an old one makes way
        }
    }
}

void Particles::step(float dt, V3 gravity)
{
    int k = 0;
    for (int i = 0; i < n_; ++i) {
        P &q = p_[i];
        q.life += dt;
        if (q.life >= q.total) {
            continue;
        }
        q.v = q.v + gravity * dt;
        q.p = q.p + q.v * dt;
        p_[k++] = q;
    }
    n_ = k;
}

void Particles::draw(uint16_t *fb, const Camera &cam) const
{
    for (int i = 0; i < n_; ++i) {
        const P &q = p_[i];
        const float left = 1 - q.life / q.total;
        cam.glow_at(fb, q.p, q.size, scale565(q.color, int(left * 32)), 0.4f, 6);
    }
}

}  // namespace games
