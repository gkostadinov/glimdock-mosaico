// SPDX-License-Identifier: Apache-2.0
#include "face_match.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace face {

Landmarks landmarks_from(const int *keypoints)
{
    Landmarks l;
    for (int i = 0; i < 5; ++i) {
        l.x[i] = float(keypoints[2 * i]);
        l.y[i] = float(keypoints[2 * i + 1]);
    }
    return l;
}

float eye_distance(const Landmarks &l)
{
    return hypotf(l.x[RIGHT_EYE] - l.x[LEFT_EYE], l.y[RIGHT_EYE] - l.y[LEFT_EYE]);
}

NosePose nose_pose(const Landmarks &l)
{
    // Solve nose - le = a (re - le) + b (mouth - le) for a and b.
    const float ux = l.x[RIGHT_EYE] - l.x[LEFT_EYE], uy = l.y[RIGHT_EYE] - l.y[LEFT_EYE];
    const float mx = (l.x[LEFT_MOUTH] + l.x[RIGHT_MOUTH]) / 2, my = (l.y[LEFT_MOUTH] + l.y[RIGHT_MOUTH]) / 2;
    const float vx = mx - l.x[LEFT_EYE], vy = my - l.y[LEFT_EYE];
    const float px = l.x[NOSE] - l.x[LEFT_EYE], py = l.y[NOSE] - l.y[LEFT_EYE];
    const float det = ux * vy - uy * vx;
    NosePose p;
    // A face collapsed to a line (eyes and mouth in a row) or a point has no frame.
    if (fabsf(det) <= 1e-3f * (ux * ux + uy * uy + vx * vx + vy * vy)) {
        return p;
    }
    p.a = (px * vy - py * vx) / det;
    p.b = (ux * py - uy * px) / det;
    p.valid = true;
    return p;
}

float dot(const float *a, const float *b, int n)
{
    float s = 0;
    for (int i = 0; i < n; ++i) {
        s += a[i] * b[i];
    }
    return s;
}

void normalize(float *v, int n)
{
    const float norm = sqrtf(dot(v, v, n));
    if (norm > 0) {
        for (int i = 0; i < n; ++i) {
            v[i] /= norm;
        }
    }
}

size_t quantize(const float *v, int n, Template &t)
{
    if (n <= 0 || n > MAX_FEAT) {
        return 0;
    }
    float peak = 0;
    for (int i = 0; i < n; ++i) {
        peak = std::max(peak, fabsf(v[i]));
    }
    t.version = TEMPLATE_VERSION;
    t.reserved = 0;
    t.length = uint16_t(n);
    t.scale = peak > 0 ? peak / 127.0f : 1.0f;
    for (int i = 0; i < n; ++i) {
        t.q[i] = int8_t(lroundf(v[i] / t.scale));
    }
    return TEMPLATE_HEADER + size_t(n);
}

bool dequantize(const Template &t, size_t bytes, float *out, int n)
{
    if (bytes < TEMPLATE_HEADER || t.version != TEMPLATE_VERSION || t.length != n || n > MAX_FEAT ||
            bytes != TEMPLATE_HEADER + size_t(n) || !(t.scale > 0)) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        out[i] = float(t.q[i]) * t.scale;
    }
    normalize(out, n);
    return true;
}

bool tuning_valid(const Tuning &t)
{
    return t.threshold >= 0.2f && t.threshold <= 0.95f && t.margin >= 0.0f && t.margin <= 0.5f && t.agree >= 1 &&
           t.agree <= 10 && t.live_spread >= 0.0f && t.live_spread <= 1.0f;
}

Match best_match(const float *query, int n, const float *const templates[PROFILES], const Tuning &tuning)
{
    Match m;
    for (int p = 0; p < PROFILES; ++p) {
        if (!templates[p]) {
            continue;
        }
        const float s = dot(query, templates[p], n);
        if (s > m.best) {
            m.second = m.best;
            m.best = s;
            m.best_profile = p;
        } else if (s > m.second) {
            m.second = s;
        }
    }
    // With one profile enrolled there is no runner-up: the margin is over
    // nothing (second stays -1), so the threshold alone decides.
    if (m.best_profile >= 0 && m.best >= tuning.threshold && m.best - m.second >= tuning.margin) {
        m.profile = m.best_profile;
    }
    return m;
}

void Agreement::reset()
{
    profile_ = -1;
    count_ = misses_ = 0;
    sum_ = 0;
}

bool Agreement::push(int profile, float similarity, int agree)
{
    if (profile < 0) {
        if (profile_ >= 0 && ++misses_ <= 1) {
            return false;    // forgiven once: the run goes on
        }
        reset();
        return false;
    }
    if (profile != profile_) {
        reset();
        profile_ = profile;
    }
    misses_ = 0;
    ++count_;
    sum_ += similarity;
    return count_ >= agree;
}

void Liveness::reset()
{
    frames_ = 0;
}

void Liveness::push(const Landmarks &l)
{
    const NosePose p = nose_pose(l);
    if (!p.valid) {
        return;
    }
    ring_a_[frames_ % SMOOTH] = p.a;
    ring_b_[frames_ % SMOOTH] = p.b;
    if (++frames_ < SMOOTH) {
        return;
    }
    float a = 0, b = 0;
    for (int i = 0; i < SMOOTH; ++i) {
        a += ring_a_[i];
        b += ring_b_[i];
    }
    a /= SMOOTH;
    b /= SMOOTH;
    if (frames_ == SMOOTH) {
        min_a_ = max_a_ = a;
        min_b_ = max_b_ = b;
    } else {
        min_a_ = std::min(min_a_, a), max_a_ = std::max(max_a_, a);
        min_b_ = std::min(min_b_, b), max_b_ = std::max(max_b_, b);
    }
}

float Liveness::spread() const
{
    return frames_ >= SMOOTH ? std::max(max_a_ - min_a_, max_b_ - min_b_) : 0.0f;
}

void Enrollment::reset(int length)
{
    length_ = std::min(std::max(length, 0), MAX_FEAT);
    count_ = 0;
}

void Enrollment::add(const float *embedding)
{
    if (count_ == CAPACITY) {  // full of samples that never agreed: the oldest goes
        memmove(samples_[0], samples_[1], sizeof(samples_[0]) * (CAPACITY - 1));
        --count_;
    }
    memcpy(samples_[count_++], embedding, sizeof(float) * size_t(length_));
}

bool Enrollment::finish(float *out, int need, float min_cosine)
{
    if (count_ < need || !length_) {
        return false;
    }
    // The mean, the samples close to it, then the mean of those.
    for (int pass = 0; pass < 2; ++pass) {
        memset(out, 0, sizeof(float) * size_t(length_));
        for (int s = 0; s < count_; ++s)
            for (int i = 0; i < length_; ++i) {
                out[i] += samples_[s][i];
            }
        normalize(out, length_);
        int kept = 0;
        for (int s = 0; s < count_; ++s) {
            if (dot(samples_[s], out, length_) >= min_cosine) {
                if (kept != s) {
                    memcpy(samples_[kept], samples_[s], sizeof(float) * size_t(length_));
                }
                ++kept;
            }
        }
        const bool all_kept = kept == count_;
        count_ = kept;
        if (count_ < need) {
            return false;
        }
        if (all_kept) {
            return true;    // out is the mean of exactly the samples kept
        }
    }
    // The second pass dropped more: average what is left once more.
    memset(out, 0, sizeof(float) * size_t(length_));
    for (int s = 0; s < count_; ++s)
        for (int i = 0; i < length_; ++i) {
            out[i] += samples_[s][i];
        }
    normalize(out, length_);
    return true;
}

}  // namespace face
