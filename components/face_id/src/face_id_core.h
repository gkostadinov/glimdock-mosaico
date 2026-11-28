// SPDX-License-Identifier: Apache-2.0
// Between face ID's core (face_id.cpp on the board, face_id_host.cpp in the
// desktop preview) and its preview widget (face_preview.cpp).
#pragma once

#include <stdint.h>

#include <atomic>

namespace face_core {

// What the preview's status line says.
enum class Status : uint8_t {
    Idle,           // the preview alone, nothing asked yet
    Starting,       // the camera is opening
    Look,           // no face in view
    Closer,         // a face, too small (or cut off by the edge)
    OneFace,        // more than one face
    Hold,           // a face being measured
    Turn,           // matched, waiting for some head movement
    Rear,           // no face, and the camera faces away from the person
    Recognised,
    NotRecognised,
    Saved,          // enrolled
    Busy,           // another mode has the camera
    NoCamera,
    Error,
};
const char *status_text(Status s);
uint32_t status_color(Status s);  // 0xRRGGBB for the ring and the text

// Three frames for one preview, RGB565A8 at size x size (LVGL's layout: the
// colour plane, then an alpha plane holding the round mask, the same in all
// three). The camera's task writes one while the LVGL task shows another; the
// third is the newest finished one.
class PreviewFrames {
public:
    explicit PreviewFrames(int size);
    ~PreviewFrames();
    bool ok() const
    {
        return buf_[0] && buf_[1] && buf_[2];
    }
    int size() const
    {
        return size_;
    }
    uint8_t *buffer(int index) const
    {
        return buf_[index];
    }
    // Writer: the buffer to fill, then publish() it.
    uint16_t *write_target()
    {
        return reinterpret_cast<uint16_t *>(buf_[write_]);
    }
    void publish();
    // Reader: the index of a newer finished frame, or -1 if there is none;
    // the previously shown buffer goes back to the writer.
    int take();
    // One 240 x 240 BGR888 camera frame, scaled to the preview (nearest).
    void fill_from_bgr(const uint8_t *bgr, int source_size);

private:
    static constexpr uint8_t FRESH = 0x80;
    int size_;
    uint8_t *buf_[3] = {};
    int write_ = 0, read_ = 2;
    std::atomic<uint8_t> latest_{1};
    int map_source_ = 0;      // the source size the map was made for
    uint16_t *map_ = nullptr;  // preview x (and y) -> source index
};

// LVGL task. The core writes the camera's frames into the newest preview's
// frames; detaching returns once nothing writes into them any more (and
// leaves a newer preview attached).
void attach_preview(PreviewFrames *frames);
void detach_preview(PreviewFrames *frames);
// Whether the preview's screen is the one shown: hidden, the camera is let go.
void preview_visible(bool visible);
Status status();

}  // namespace face_core
