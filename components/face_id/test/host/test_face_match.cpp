// SPDX-License-Identifier: Apache-2.0
// Host tests of face ID's decisions (face_match.cpp) on synthetic data only:
// random unit vectors stand in for embeddings (identities and their noisy
// views), and a five-point 3D face model, projected through a pinhole, stands
// in for the detector's landmarks. No face images are involved.
//
//   cmake -S components/face_id/test/host -B build-host/face_id_test && cmake --build build-host/face_id_test
//   ctest --test-dir build-host/face_id_test --output-on-failure
#include "face_match.h"

#include <math.h>
#include <stdio.h>

#include <random>
#include <vector>

namespace {
int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                 \
        }                                                               \
    } while (0)

constexpr int N = 512;
std::mt19937 rng(20261128);

std::vector<float> random_unit()
{
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::vector<float> v(N);
    for (float &x : v) {
        x = g(rng);
    }
    face::normalize(v.data(), N);
    return v;
}

// A view of an identity: its centre plus noise, normalised. noise 0.039
// gives a cosine of ~0.75 with the centre (1 / sqrt(1 + N noise^2)).
std::vector<float> view(const std::vector<float> &centre, float noise)
{
    std::normal_distribution<float> g(0.0f, noise);
    std::vector<float> v(centre);
    for (float &x : v) {
        x += g(rng);
    }
    face::normalize(v.data(), N);
    return v;
}

std::vector<float> enroll(const std::vector<float> &centre, float noise)
{
    static face::Enrollment e;  // 16 KB: not on the stack
    e.reset(N);
    for (int i = 0; i < 5; ++i) {
        e.add(view(centre, noise).data());
    }
    std::vector<float> out(N);
    CHECK(e.finish(out.data(), 5, 0.70f));
    return out;
}

void test_quantize()
{
    static face::Template t;
    const std::vector<float> v = random_unit();
    const size_t bytes = face::quantize(v.data(), N, t);
    CHECK(bytes == face::TEMPLATE_HEADER + N);
    std::vector<float> back(N);
    CHECK(face::dequantize(t, bytes, back.data(), N));
    CHECK(face::dot(v.data(), back.data(), N) > 0.999f);
    // The quantisation moves cosines with other vectors by well under 0.01.
    for (int i = 0; i < 50; ++i) {
        const std::vector<float> other = view(v, 0.039f);
        CHECK(fabsf(face::dot(other.data(), v.data(), N) - face::dot(other.data(), back.data(), N)) < 0.01f);
    }
    CHECK(!face::dequantize(t, bytes - 1, back.data(), N));      // truncated
    CHECK(!face::dequantize(t, bytes, back.data(), N - 1));      // another model's length
    t.version = face::TEMPLATE_VERSION + 1;
    CHECK(!face::dequantize(t, bytes, back.data(), N));          // another format
    CHECK(face::quantize(v.data(), face::MAX_FEAT + 1, t) == 0);
}

void test_matching()
{
    const face::Tuning tuning;
    std::vector<std::vector<float>> centres, templates;
    const float *table[face::PROFILES] = {};
    for (int p = 0; p < 6; ++p) {
        centres.push_back(random_unit());
    }
    for (int p = 0; p < 6; ++p) {
        templates.push_back(enroll(centres[size_t(p)], 0.039f));
    }
    for (int p = 0; p < 6; ++p) {
        table[p * 2] = templates[size_t(p)].data();  // profiles 0, 2, ... 10
    }
    // Genuine views match their own profile; strangers match nobody.
    int genuine_ok = 0, impostor_matched = 0;
    const int trials = 2000;
    for (int i = 0; i < trials; ++i) {
        const int who = i % 6;
        const face::Match m = face::best_match(view(centres[size_t(who)], 0.039f).data(), N, table, tuning);
        CHECK(m.profile < 0 || m.profile == who * 2);  // never the wrong profile
        genuine_ok += m.profile == who * 2;
        impostor_matched += face::best_match(view(random_unit(), 0.039f).data(), N, table, tuning).profile >= 0;
    }
    printf("synthetic views: %d/%d genuine matched, %d/%d strangers matched\n", genuine_ok, trials, impostor_matched,
           trials);
    CHECK(genuine_ok > trials * 98 / 100);
    CHECK(impostor_matched == 0);

    // Below the threshold: no match, but the best is still reported.
    const face::Match weak = face::best_match(view(centres[0], 0.08f).data(), N, table, tuning);
    CHECK(weak.best_profile == 0);
    CHECK(weak.profile == -1 || weak.best >= tuning.threshold);

    // Two profiles nearly alike (one person enrolled twice): the margin refuses both.
    std::vector<float> twin = view(templates[0], 0.006f);
    table[1] = twin.data();
    const face::Match ambiguous = face::best_match(view(centres[0], 0.039f).data(), N, table, tuning);
    CHECK(ambiguous.profile == -1);
    CHECK(ambiguous.best >= tuning.threshold);
    table[1] = nullptr;

    // One profile alone: no runner-up, the threshold decides.
    const float *one[face::PROFILES] = {};
    one[7] = templates[3].data();
    CHECK(face::best_match(view(centres[3], 0.039f).data(), N, one, tuning).profile == 7);
    CHECK(face::best_match(view(centres[4], 0.039f).data(), N, one, tuning).profile == -1);
    const float *none[face::PROFILES] = {};
    CHECK(face::best_match(centres[0].data(), N, none, tuning).best_profile == -1);
}

void test_enrollment()
{
    static face::Enrollment e;
    const std::vector<float> me = random_unit(), stranger = random_unit();
    std::vector<float> out(N);
    e.reset(N);
    for (int i = 0; i < 4; ++i) {
        e.add(view(me, 0.039f).data());
    }
    CHECK(!e.finish(out.data(), 5, 0.70f));  // too few
    e.add(view(stranger, 0.039f).data());     // someone walks through
    CHECK(!e.finish(out.data(), 5, 0.70f));  // the stranger is dropped: four left
    CHECK(e.count() == 4);
    e.add(view(me, 0.039f).data());
    CHECK(e.finish(out.data(), 5, 0.70f));
    CHECK(face::dot(out.data(), me.data(), N) > 0.9f);
    CHECK(face::dot(out.data(), stranger.data(), N) < 0.2f);
}

void test_agreement()
{
    face::Agreement a;
    CHECK(!a.push(3, 0.7f, 3));
    CHECK(!a.push(3, 0.8f, 3));
    CHECK(a.push(3, 0.9f, 3));
    CHECK(fabsf(a.mean_similarity() - 0.8f) < 1e-5f);
    a.reset();
    CHECK(!a.push(3, 0.7f, 3));
    CHECK(!a.push(-1, 0.4f, 3));  // one miss: forgiven
    CHECK(!a.push(3, 0.7f, 3));
    CHECK(a.push(3, 0.7f, 3));
    a.reset();
    a.push(3, 0.7f, 3);
    a.push(-1, 0.4f, 3);
    a.push(-1, 0.4f, 3);          // two in a row: over
    CHECK(a.count() == 0 && a.profile() == -1);
    a.push(3, 0.7f, 3);
    a.push(3, 0.7f, 3);
    CHECK(!a.push(5, 0.7f, 3));   // another profile starts over
    CHECK(a.profile() == 5 && a.count() == 1);
    CHECK(a.push(1, 0.9f, 1));    // agree = 1: the first frame decides
}

// ---- liveness on simulated landmarks -------------------------------------------------------
// A face in eye-distance units (x right, y down, z towards the camera): the
// nose stands ~0.45 out of the eye-mouth plane (about 28 mm for 63 mm between
// the eyes), in face_match.h's order.
struct P3 {
    float x, y, z;
};
const P3 FACE[5] = {{-0.5f, 0.0f, 0.0f}, {-0.4f, 1.1f, 0.1f}, {0.0f, 0.55f, 0.45f}, {0.5f, 0.0f, 0.0f}, {0.4f, 1.1f, 0.1f}};
constexpr float CAMERA = 4.8f;  // the camera's distance (~30 cm) in eye distances

P3 rotate(P3 p, float yaw, float pitch, float roll)
{
    float c = cosf(yaw), s = sinf(yaw);
    p = {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
    c = cosf(pitch), s = sinf(pitch);
    p = {p.x, p.y * c - p.z * s, p.y * s + p.z * c};
    c = cosf(roll), s = sinf(roll);
    return {p.x * c - p.y * s, p.x * s + p.y * c, p.z};
}

face::Landmarks shoot(const P3 *pts, float yaw, float pitch, float roll, float eyes_px, float jitter_px)
{
    std::normal_distribution<float> g(0.0f, jitter_px > 0 ? jitter_px : 1e-6f);
    face::Landmarks l;
    for (int i = 0; i < 5; ++i) {
        const P3 q = rotate(pts[i], yaw, pitch, roll);
        const float k = eyes_px * CAMERA / (CAMERA - q.z);  // pinhole
        l.x[i] = 120.0f + q.x * k + (jitter_px > 0 ? g(rng) : 0.0f);
        l.y[i] = 100.0f + q.y * k + (jitter_px > 0 ? g(rng) : 0.0f);
    }
    return l;
}

float rad(float deg)
{
    return deg * 3.14159265f / 180.0f;
}

void test_nose_pose()
{
    // Frontal: about (0.25, 0.5), the reference landmarks' own; the camera's
    // perspective enlarges the nearer nose a little (here ~0.23, ~0.54).
    const face::NosePose frontal = face::nose_pose(shoot(FACE, 0, 0, 0, 40, 0));
    CHECK(frontal.valid);
    CHECK(fabsf(frontal.a - 0.25f) < 0.05f && fabsf(frontal.b - 0.5f) < 0.06f);
    // A flat photo under affine maps (turned, scaled, sheared, squashed by a
    // tilt) keeps a and b exactly.
    face::Landmarks l = shoot(FACE, 0, 0, 0, 40, 0);
    for (int i = 0; i < 5; ++i) {
        const float x = l.x[i], y = l.y[i];
        l.x[i] = 0.7f * x + 0.3f * y + 11;
        l.y[i] = -0.2f * x + 0.9f * y - 5;
    }
    const face::NosePose moved = face::nose_pose(l);
    CHECK(fabsf(moved.a - frontal.a) < 1e-4f && fabsf(moved.b - frontal.b) < 1e-4f);
    // A real head turned 15 degrees moves a clearly.
    CHECK(fabsf(face::nose_pose(shoot(FACE, rad(15), 0, 0, 40, 0)).a - frontal.a) > 0.08f);
    face::Landmarks line = {};
    CHECK(!face::nose_pose(line).valid);
}

float run_spread(bool live, float degrees, float eyes, float jitter, int frames)
{
    P3 flat[5];
    for (int i = 0; i < 5; ++i) {
        flat[i] = {FACE[i].x, FACE[i].y, 0.0f};
    }
    std::uniform_real_distribution<float> tilt(-degrees, degrees), roll(-30, 30);
    face::Liveness lv;
    for (int t = 0; t < frames; ++t) {
        if (live) {  // turning the head side to side
            lv.push(shoot(FACE, rad(degrees * sinf(t * 0.8f)), 0, 0, eyes, jitter));
        } else {     // a printed face waved and tilted in front of the camera
            lv.push(shoot(flat, rad(tilt(rng)), rad(tilt(rng)), rad(roll(rng)), eyes, jitter));
        }
    }
    return lv.spread();
}

void test_liveness()
{
    const float threshold = face::Tuning().live_spread;
    face::Liveness lv;
    for (int t = 0; t < 3; ++t) {
        lv.push(shoot(FACE, rad(20.0f * (t % 2 ? 1 : -1)), 0, 0, 40, 0));
    }
    CHECK(!lv.passed(threshold));  // not enough frames for a moving average to move
    // The simulation's landmark jitter (1 px) and a modest eye distance (32 px).
    int live_pass = 0, still_pass = 0, waved_pass = 0;
    const int runs = 400;
    for (int i = 0; i < runs; ++i) {
        live_pass += run_spread(true, 15, 32, 1.0f, 8) >= threshold;
        still_pass += run_spread(false, 0, 32, 1.0f, 8) >= threshold;
        waved_pass += run_spread(false, 35, 32, 1.0f, 8) >= threshold;
    }
    printf("liveness at %.2f: head turned +-15 deg passes %d/%d; a photo held still %d/%d, waved and tilted to 35 deg %d/%d\n",
           double(threshold), live_pass, runs, still_pass, runs, waved_pass, runs);
    CHECK(live_pass >= runs * 90 / 100);
    CHECK(still_pass <= runs / 50);
    CHECK(waved_pass <= runs / 50);
    // Noise-free: a still face never passes, however many frames.
    CHECK(run_spread(true, 0, 40, 0, 20) < 1e-4f);
}

void test_tuning()
{
    face::Tuning t;
    CHECK(face::tuning_valid(t));
    t.threshold = 0.1f;
    CHECK(!face::tuning_valid(t));
    t = face::Tuning();
    t.agree = 0;
    CHECK(!face::tuning_valid(t));
}
}  // namespace

int main()
{
    test_quantize();
    test_matching();
    test_enrollment();
    test_agreement();
    test_nose_pose();
    test_liveness();
    test_tuning();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("face_match: all checks passed\n");
    return 0;
}
