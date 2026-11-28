// SPDX-License-Identifier: Apache-2.0
// Face ID in the desktop preview (host/CMakeLists.txt): no camera and no
// models, so face_id_available() is false and nothing starts; the preview
// widget is the board's own (face_preview.cpp), shown with a synthetic frame
// (a plain grey head-and-shoulders shape on a gradient, never a real face) and
// a status chosen with face_id_host_status().
#include "face_id.h"
#include "face_id_core.h"

#include <math.h>
#include <stdlib.h>

#include <vector>

namespace {
face_core::Status shown = face_core::Status::Look;

// A 240 x 240 BGR888 frame: a dark blue-grey gradient with an ellipse for a
// head and a wider one for the shoulders.
const uint8_t *synthetic_frame()
{
    static std::vector<uint8_t> frame;
    if (!frame.empty()) {
        return frame.data();
    }
    constexpr int N = 240;
    frame.resize(size_t(N) * N * 3);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float t = float(y) / N;
            float r = 40 + 30 * t, g = 46 + 30 * t, b = 58 + 40 * t;
            const float hx = (x - 120) / 46.0f, hy = (y - 100) / 58.0f;   // head
            const float sx = (x - 120) / 100.0f, sy = (y - 250) / 80.0f;  // shoulders
            if (hx * hx + hy * hy < 1.0f || sx * sx + sy * sy < 1.0f) {
                const float shade = 1.0f - 0.25f * fminf(1.0f, hx * hx + hy * hy);
                r = g = b = 150 * shade;
            }
            uint8_t *p = &frame[(size_t(y) * N + x) * 3];
            p[0] = uint8_t(b), p[1] = uint8_t(g), p[2] = uint8_t(r);
        }
    }
    return frame.data();
}
}  // namespace

namespace face_core {
void attach_preview(PreviewFrames *frames)
{
    frames->fill_from_bgr(synthetic_frame(), 240);
}

void detach_preview(PreviewFrames *) {}

void preview_visible(bool) {}

Status status()
{
    return shown;
}
}  // namespace face_core

// For the preview's captures: which status line to show (face_core::Status).
extern "C" void face_id_host_status(int status)
{
    shown = face_core::Status(status);
}

extern "C" void face_id_init(void (*)(void)) {}

extern "C" bool face_id_available(void)
{
    return false;
}

extern "C" bool face_id_enroll_start(uint8_t, void (*)(face_id_result_t, void *), void *)
{
    return false;
}

extern "C" bool face_id_identify_start(uint32_t, void (*)(face_id_match_t, void *), void *)
{
    return false;
}

extern "C" void face_id_cancel(void) {}

extern "C" bool face_id_enrolled(uint8_t)
{
    return false;
}

extern "C" void face_id_forget(uint8_t) {}

extern "C" void face_id_set_liveness(bool) {}
