// SPDX-License-Identifier: Apache-2.0
// Face ID's live preview: a round picture of what the camera sees and a status
// line, shared by the board and the desktop preview. The picture is RGB565A8
// with the circle in its alpha plane, so LVGL draws it round without a mask
// layer; frames are scaled to the widget's size while they are converted (on
// the camera's task), never by LVGL, and swapped in at most 15 times a second.
#include "face_id.h"
#include "face_id_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mui.h"
#include "src/misc/cache/instance/lv_image_cache.h"  // the three frames change in place

namespace face_core {

const char *status_text(Status s)
{
    switch (s) {
    case Status::Idle:
    case Status::Look: return "Look at the camera";
    case Status::Starting: return "Starting the camera\xE2\x80\xA6";
    case Status::Closer: return "Come closer";
    case Status::OneFace: return "One face at a time";
    case Status::Hold: return "Hold still";
    case Status::Turn: return "Turn your head slightly";
    case Status::Rear: return "Turn the camera towards you";
    case Status::Recognised: return "Recognised";
    case Status::NotRecognised: return "Not recognised";
    case Status::Saved: return "Face saved";
    case Status::Busy: return "The camera is in use";
    case Status::NoCamera: return "No camera module";
    case Status::Error: return "Face ID is unavailable";
    }
    return "";
}

uint32_t status_color(Status s)
{
    switch (s) {
    case Status::Recognised:
    case Status::Saved: return mui::OK;
    case Status::Closer:
    case Status::OneFace:
    case Status::Turn:
    case Status::Rear: return mui::WARN;
    case Status::NotRecognised:
    case Status::Busy:
    case Status::NoCamera:
    case Status::Error: return mui::RED;
    case Status::Hold: return mui::CYAN.hi;
    default: return mui::TEXT;
    }
}

PreviewFrames::PreviewFrames(int size) : size_(size)
{
    const size_t pixels = size_t(size) * size;
    for (auto &b : buf_) {
        b = static_cast<uint8_t *>(malloc(pixels * 3));  // PSRAM first on the board
    }
    map_ = static_cast<uint16_t *>(malloc(sizeof(uint16_t) * size_t(size)));
    if (!ok() || !map_) {
        return;
    }
    // The colour planes start a dark grey (the camera is opening); the alpha
    // plane is the circle, with a one-pixel soft edge.
    uint8_t *alpha = buf_[0] + pixels * 2;
    const float r = size / 2.0f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float d = hypotf(x + 0.5f - r, y + 0.5f - r);
            const float a = fminf(fmaxf(r - d, 0.0f), 1.0f);
            alpha[y * size + x] = uint8_t(lroundf(a * 255.0f));
        }
    }
    uint16_t *grey = reinterpret_cast<uint16_t *>(buf_[0]);
    for (size_t i = 0; i < pixels; ++i) {
        grey[i] = 0x18E3;  // #1C1C1C
    }
    memcpy(buf_[1], buf_[0], pixels * 3);
    memcpy(buf_[2], buf_[0], pixels * 3);
}

PreviewFrames::~PreviewFrames()
{
    for (auto &b : buf_) {
        free(b);
    }
    free(map_);
}

void PreviewFrames::publish()
{
    const uint8_t previous = latest_.exchange(uint8_t(write_) | FRESH);
    write_ = previous & 3;
}

int PreviewFrames::take()
{
    if (!(latest_.load() & FRESH)) {
        return -1;
    }
    const uint8_t previous = latest_.exchange(uint8_t(read_));
    read_ = previous & 3;
    return read_;
}

void PreviewFrames::fill_from_bgr(const uint8_t *bgr, int source_size)
{
    if (!ok() || !map_ || source_size <= 0) {
        return;
    }
    if (map_source_ != source_size) {
        for (int i = 0; i < size_; ++i) {
            map_[i] = uint16_t(i * source_size / size_);
        }
        map_source_ = source_size;
    }
    uint16_t *out = write_target();
    const float r = size_ / 2.0f;
    for (int y = 0; y < size_; ++y) {
        // Only the circle's span of the row: the alpha hides the rest.
        const float dy = y + 0.5f - r;
        const float half = r * r - dy * dy > 0 ? sqrtf(r * r - dy * dy) + 1.0f : 0.0f;
        const int x0 = int(fmaxf(r - half, 0.0f)), x1 = int(fminf(r + half, float(size_)));
        const uint8_t *row = bgr + size_t(map_[y]) * size_t(source_size) * 3;
        uint16_t *o = out + y * size_;
        for (int x = x0; x < x1; ++x) {
            const uint8_t *p = row + map_[x] * 3;
            o[x] = uint16_t((p[2] & 0xF8) << 8 | (p[1] & 0xFC) << 3 | p[0] >> 3);
        }
    }
    publish();
}

}  // namespace face_core

namespace {
using face_core::Status;

struct Preview {
    face_core::PreviewFrames *frames = nullptr;
    lv_obj_t *root = nullptr, *image = nullptr, *ring = nullptr, *pill = nullptr, *text = nullptr;
    lv_image_dsc_t dsc[3] = {};
    lv_timer_t *timer = nullptr;
    int size = 0, px = 16, pill_y = 0;
    Status status = Status::Error;
    bool status_set = false, visible = false;
};

// The status in a dark pill that hugs its text, in the circle's lower part;
// a long message steps the font down rather than run past the edge.
void show_status(Preview *p, Status s)
{
    if (p->status_set && s == p->status) {
        return;
    }
    p->status = s;
    p->status_set = true;
    const char *text = face_core::status_text(s);
    const int pad = p->px / 2 + 6, max_w = p->size * 4 / 5 - 2 * pad;
    int px = p->px, w = mui::text_width(text, px);
    while (w > max_w && px > 12) {
        px -= 2;
        w = mui::text_width(text, px);
    }
    const lv_color_t color = lv_color_hex(face_core::status_color(s));
    const int h = p->px + 14, pill_w = (w < max_w ? w : max_w) + 2 * pad;
    lv_obj_set_style_text_font(p->text, mui::font(px), 0);
    lv_obj_set_size(p->text, pill_w - 2 * pad, LV_SIZE_CONTENT);
    lv_obj_set_pos(p->text, pad, (h - px) / 2 - 2);
    mui::set_text(p->text, text);
    mui::set_text_color(p->text, color);
    lv_obj_set_size(p->pill, pill_w, h);
    lv_obj_set_pos(p->pill, (p->size - pill_w) / 2, p->pill_y);
    lv_obj_set_style_border_color(p->ring, s == Status::Idle || s == Status::Look ? lv_color_hex(mui::EDGE) : color, 0);
}

void tick(lv_timer_t *t)
{
    auto *p = static_cast<Preview *>(lv_timer_get_user_data(t));
    // Hidden behind another screen: the camera goes back to whoever wants it.
    const bool visible = lv_obj_get_screen(p->root) == lv_screen_active() && lv_obj_is_visible(p->root);
    if (visible != p->visible) {
        p->visible = visible;
        face_core::preview_visible(visible);
    }
    if (p->frames && p->frames->ok()) {
        const int index = p->frames->take();
        if (index >= 0) {
            // The same three buffers come round again: drop any cached copy.
            lv_image_cache_drop(&p->dsc[index]);
            lv_image_set_src(p->image, &p->dsc[index]);
        }
    }
    show_status(p, p->frames && p->frames->ok() ? face_core::status() : Status::Error);
}

void deleted(lv_event_t *e)
{
    auto *p = static_cast<Preview *>(lv_event_get_user_data(e));
    lv_timer_delete(p->timer);
    if (p->frames) {
        face_core::detach_preview(p->frames);
        lv_image_cache_drop(&p->dsc[0]);
        lv_image_cache_drop(&p->dsc[1]);
        lv_image_cache_drop(&p->dsc[2]);
        delete p->frames;
    }
    delete p;
}
}  // namespace

extern "C" lv_obj_t *face_id_preview_create(lv_obj_t *parent, int x, int y, int size)
{
    size = size < 80 ? 80 : size > 480 ? 480 : size;
    auto *p = new Preview;
    p->size = size;
    p->root = mui::box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(p->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_COVER, 0);
    p->frames = new face_core::PreviewFrames(size);
    if (p->frames->ok()) {
        for (int i = 0; i < 3; ++i) {
            lv_image_dsc_t &d = p->dsc[i];
            d.header.magic = LV_IMAGE_HEADER_MAGIC;
            d.header.cf = LV_COLOR_FORMAT_RGB565A8;
            d.header.w = uint32_t(size);
            d.header.h = uint32_t(size);
            d.header.stride = uint32_t(size) * 2;
            d.data = p->frames->buffer(i);
            d.data_size = uint32_t(size) * uint32_t(size) * 3;
        }
        p->image = lv_image_create(p->root);
        lv_obj_set_pos(p->image, 0, 0);
        lv_image_set_src(p->image, &p->dsc[2]);  // the reader's first buffer: grey until a frame comes
    }
    // The ring: its own object over the picture (a border on root would
    // move the picture in by its width).
    p->ring = mui::box(p->root, 0, 0, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(p->ring, size >= 200 ? 3 : 2, 0);
    lv_obj_set_style_border_color(p->ring, lv_color_hex(mui::EDGE), 0);
    p->px = size >= 300 ? 18 : size >= 200 ? 16 : 14;
    p->pill_y = size * 3 / 4 - (p->px + 14) / 2;
    p->pill = mui::box(p->root, 0, p->pill_y, 1, 1, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(p->pill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p->pill, LV_OPA_60, 0);
    p->text = mui::label(p->pill, "", 0, 0, 0, mui::TEXT, p->px, true, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(p->text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_event_cb(p->root, deleted, LV_EVENT_DELETE, p);
    p->timer = lv_timer_create(tick, 66, p);
    show_status(p, p->frames->ok() ? Status::Idle : Status::Error);
    if (p->frames->ok()) {
        face_core::attach_preview(p->frames);
    }
    tick(p->timer);
    return p->root;
}
