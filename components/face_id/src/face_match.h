// SPDX-License-Identifier: Apache-2.0
// Face ID's decisions, free of ESP-IDF so the host tests run them: cosine
// matching against the enrolled templates, the frames that must agree, the
// head-movement (liveness) check, enrollment averaging and the int8 templates
// kept in NVS. Embeddings are L2-normalised floats (ESP-DL's FeatPostprocessor).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace face {

constexpr int PROFILES = 16;     // profile ids 0-15 (the profiles component's)
constexpr int MAX_FEAT = 512;    // the longest embedding a template holds
constexpr uint8_t TEMPLATE_VERSION = 1;  // bump when the model or the format changes: old templates stop matching

// The five landmarks of ESP-DL's MNP stage, in its order (the order of
// FeatImagePreprocessor's reference points): left eye, left mouth corner,
// nose, right eye, right mouth corner ("left" as seen in the picture).
struct Landmarks {
    float x[5], y[5];
};
enum { LEFT_EYE, LEFT_MOUTH, NOSE, RIGHT_EYE, RIGHT_MOUTH };
Landmarks landmarks_from(const int *keypoints);  // [x0, y0, x1, y1, ...]
float eye_distance(const Landmarks &l);

// Where the nose sits in the affine frame of the eyes and the mouth's middle:
// nose = left_eye + a (right_eye - left_eye) + b (mouth - left_eye). Moving,
// tilting or bending a flat photo is (close to) an affine map of all five
// points, which leaves a and b unchanged; a real head turning or nodding moves
// the nose, which stands out of that plane (a follows the turn, b the nod).
struct NosePose {
    float a = 0, b = 0;
    bool valid = false;
};
NosePose nose_pose(const Landmarks &l);

float dot(const float *a, const float *b, int n);
void normalize(float *v, int n);

// A template as stored (NVS blob): one scale and int8 values; quantising a
// unit vector this way changes its cosine with others by well under 0.01.
struct Template {
    uint8_t version;
    uint8_t reserved;
    uint16_t length;
    float scale;
    int8_t q[MAX_FEAT];
};
constexpr size_t TEMPLATE_HEADER = offsetof(Template, q);
// Bytes to store (header and length values), 0 if n is out of range.
size_t quantize(const float *v, int n, Template &t);
// Fills out (renormalised) from a stored blob of bytes; false if it does not
// fit n values or is from another template version.
bool dequantize(const Template &t, size_t bytes, float *out, int n);

struct Tuning {
    float threshold = 0.55f;  // cosine similarity a match needs (ESP-DL's recogniser uses 0.5)
    float margin = 0.06f;     // ... and its lead over the second-best profile
    int agree = 3;            // consecutive frames on the same profile
    bool liveness = true;     // the nose must move against the face's plane
    float live_spread = 0.12f;  // how far (a or b of NosePose, smoothed) over the agreeing frames
};
bool tuning_valid(const Tuning &t);

struct Match {
    int profile = -1;  // -1: no profile passes threshold and margin
    int best_profile = -1;
    float best = -1.0f, second = -1.0f;
};
// templates[p] points at profile p's unit vector (nullptr: not enrolled).
Match best_match(const float *query, int n, const float *const templates[PROFILES], const Tuning &tuning);

// Consecutive frames on one profile. One frame without a match (a blink, a
// turn) is forgiven; a different profile, a second miss or no face starts over.
class Agreement {
public:
    void reset();
    // A frame with one face: its match (-1 none). True once agree frames on
    // the same profile have been seen.
    bool push(int profile, float similarity, int agree);
    int profile() const
    {
        return profile_;
    }
    int count() const
    {
        return count_;
    }
    float mean_similarity() const
    {
        return count_ ? sum_ / float(count_) : 0.0f;
    }

private:
    int profile_ = -1, count_ = 0, misses_ = 0;
    float sum_ = 0;
};

// The spread of the nose's pose over the frames of one agreement run, on a
// moving average of SMOOTH frames: landmark jitter (about a pixel) alone moves
// a single frame's a and b by up to ~0.1 at a 30-pixel eye distance, a still
// photo's too. Averaged, a head turned about 15 degrees each way spreads
// 0.12-0.2 while flat photos (still or tilted up to 35 degrees) stay under
// ~0.1; see the host tests' simulation.
class Liveness {
public:
    static constexpr int SMOOTH = 3;
    void reset();
    void push(const Landmarks &l);
    float spread() const;  // the larger range of a and b
    bool passed(float threshold) const
    {
        return frames_ > SMOOTH && spread() >= threshold;
    }
    int frames() const
    {
        return frames_;
    }

private:
    float ring_a_[SMOOTH] = {}, ring_b_[SMOOTH] = {};
    float min_a_ = 0, max_a_ = 0, min_b_ = 0, max_b_ = 0;
    int frames_ = 0;
};

// Enrollment: several embeddings of one face, averaged. Samples far from the
// average (another face, a bad crop) are dropped and more are taken.
class Enrollment {
public:
    static constexpr int CAPACITY = 8;
    void reset(int length);
    int count() const
    {
        return count_;
    }
    void add(const float *embedding);
    // With at least need samples within min_cosine of their mean, writes the
    // normalised mean to out and returns true; otherwise drops the outliers.
    bool finish(float *out, int need, float min_cosine);

private:
    int length_ = 0, count_ = 0;
    float samples_[CAPACITY][MAX_FEAT];
};

}  // namespace face
