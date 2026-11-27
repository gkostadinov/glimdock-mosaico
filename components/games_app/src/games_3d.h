// SPDX-License-Identifier: Apache-2.0
// The games' shared 3D engine: vectors, a camera (perspective projection and
// near-plane clipping), flat-shaded meshes drawn far faces first, and
// particles. Every game draws through it. Where a world is drawn faster
// another way (Ember Crypt's ray caster, Skyglide's voxel land, the rally
// road), the game's own renderer draws that world, and the engine's camera,
// meshes and particles go on top.
#pragma once

#include "games_common.h"

namespace games {

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
inline float length(V3 a)
{
    return sqrtf(dot(a, a));
}
inline V3 norm(V3 a)
{
    const float l = length(a);
    return l > 0 ? a * (1 / l) : a;
}

// World: x right, y up, z ahead. Camera space: the same seen from the camera.
struct Camera {
    V3 pos{0, 0, 0}, right{1, 0, 0}, up{0, 1, 0}, fwd{0, 0, 1};
    float focal = 128, near = 0.12f, cx = float(W) / 2, cy = float(H) / 2;
    bool transposed = false;  // the frame's layout (games_common.h)

    // Looking from eye at target, turned by roll (radians, + anticlockwise).
    void look_at(V3 eye, V3 target, float roll = 0);
    V3 to_cam(V3 world) const
    {
        const V3 d = world - pos;
        return {dot(d, right), dot(d, up), dot(d, fwd)};
    }
    P2 project(V3 c) const  // camera space, in front of near
    {
        return {cx + focal * c.x / c.z, cy - focal * c.y / c.z};
    }
    // A world point on the screen; false when behind the camera.
    bool screen(V3 world, P2 *out, float *depth = nullptr) const;
    // Camera-space shapes, clipped at the near plane.
    void poly(uint16_t *fb, const V3 *cam, int n, uint16_t color) const;
    void edge(uint16_t *fb, V3 a, V3 b, uint16_t color) const;
    // A world-space shape.
    void poly_world(uint16_t *fb, const V3 *world, int n, uint16_t color) const;
    void edge_world(uint16_t *fb, V3 a, V3 b, uint16_t color) const
    {
        edge(fb, to_cam(a), to_cam(b), color);
    }
    // A glowing world point, radius in world units (at most max_px on screen).
    void glow_at(uint16_t *fb, V3 world, float radius, uint16_t color, float core = 0.35f, float max_px = 40) const;
};

// A small flat-shaded mesh (ships, cars, gliders, creatures).
struct Mesh {
    const V3 *verts;
    int nverts;
    const uint8_t (*tris)[3];
    const uint32_t *colors;  // one per triangle
    int ntris;
};
// Where a mesh stands: turned by roll (+ lifts the right side), pitch (+ the
// nose up), then yaw (+ the nose right), scaled, moved to pos.
struct Pose {
    V3 pos{0, 0, 0};
    float yaw = 0, pitch = 0, roll = 0, scale = 1;
};
struct MeshStyle {
    V3 light{0.4f, 0.9f, -0.3f};  // in camera space
    float ambient = 0.35f;
    bool two_sided = false;        // thin surfaces (wings): no faces culled
    bool outline = false;          // edges in trim (faces before split) or trim2
    uint16_t trim = 0, trim2 = 0;
    int split = 0;
};
// Back faces culled (outward: away from the mesh's middle), far faces first.
void draw_mesh(uint16_t *fb, const Camera &cam, const Mesh &mesh, const Pose &pose, const MeshStyle &style = {});
// A model-space point of a posed mesh, in the world.
V3 pose_point(const Pose &pose, V3 model);

// Sparks, dust and debris: points that fly, fall and fade, drawn as glows.
class Particles {
public:
    void burst(V3 at, int count, float speed, uint16_t color, float life, float size, Rng &rng, V3 drift = {0, 0, 0});
    void step(float dt, V3 gravity = {0, 0, 0});
    void draw(uint16_t *fb, const Camera &cam) const;
    void clear()
    {
        n_ = 0;
    }

private:
    struct P {
        V3 p, v;
        float life, total, size;
        uint16_t color;
    };
    static constexpr int MAX = 160;
    P p_[MAX];
    int n_ = 0;
};

}  // namespace games
