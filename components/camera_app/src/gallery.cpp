// SPDX-License-Identifier: Apache-2.0
// Gallery: the camera's photos as a grid of thumbnails, and one at a time full
// screen (swipe left and right, delete). Photos come from camera_app (memory,
// or the NAS for older ones); a worker task decodes them off the LVGL task.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "camera_app.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mui.h"

namespace {
using namespace mui;
const char *TAG = "gallery";
constexpr int THUMB = 128, GAP = 10, COLS = 3, FULL = 480;
const Accent &ACC = AMBER;

struct Picture {
    uint16_t *px = nullptr;
    lv_image_dsc_t dsc = {};
    int w = 0, h = 0;
};

// Decodes a JPEG, scaled by the decoder to w x h (multiples of 8).
bool decode(const uint8_t *jpeg, size_t size, int w, int h, Picture &out)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t header = {};
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    io.inbuf = const_cast<uint8_t *>(jpeg);
    io.inbuf_len = int(size);
    const bool parsed = jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK;
    jpeg_dec_close(dec);
    if (!parsed || !header.width || !header.height) {
        return false;
    }
    if (header.width != w || header.height != h) cfg.scale = {uint16_t(w), uint16_t(h)};
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    io = {};
    io.inbuf = const_cast<uint8_t *>(jpeg);
    io.inbuf_len = int(size);
    int outlen = 0;
    bool ok = false;
    if (jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK && jpeg_dec_get_outbuf_len(dec, &outlen) == JPEG_ERR_OK &&
            size_t(outlen) == size_t(w) * h * 2) {
        auto *px = static_cast<uint16_t *>(jpeg_calloc_align(size_t(outlen), 16));
        if (px) {
            io.outbuf = reinterpret_cast<uint8_t *>(px);
            ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
            if (ok) {
                out.px = px;
                out.w = w, out.h = h;
                out.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                out.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                out.dsc.header.w = uint32_t(w);
                out.dsc.header.h = uint32_t(h);
                out.dsc.header.stride = uint32_t(w * 2);
                out.dsc.data = reinterpret_cast<const uint8_t *>(px);
                out.dsc.data_size = uint32_t(outlen);
            } else {
                jpeg_free_align(px);
            }
        }
    }
    jpeg_dec_close(dec);
    return ok;
}

void release(Picture &p)
{
    if (p.px) {
        jpeg_free_align(p.px);
    }
    p = Picture{};
}

// ---- worker ----------------------------------------------------------------------------------
struct Job {
    int index;
    bool full;
    uint32_t generation;
};
struct Done {
    int index;
    bool full;
    uint32_t generation;
    Picture pic;
    bool ok;
};
QueueHandle_t jobs = nullptr, done = nullptr;

void worker(void *)
{
    for (;;) {
        Job job;
        if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        Done d{job.index, job.full, job.generation, {}, false};
        uint8_t *jpeg = nullptr;
        size_t size = 0;
        if (camera_app_photo_jpeg(job.index, &jpeg, &size)) {
            d.ok = decode(jpeg, size, job.full ? FULL : THUMB, job.full ? FULL : THUMB, d.pic);
            heap_caps_free(jpeg);
        }
        if (xQueueSend(done, &d, pdMS_TO_TICKS(1000)) != pdTRUE) {
            release(d.pic);
        }
    }
}

// ---- UI ----------------------------------------------------------------------------------------
struct {
    void (*exit)(void) = nullptr;
    bool built = false, active = false;
    lv_obj_t *screen = nullptr, *grid = nullptr, *count = nullptr, *empty = nullptr;
    lv_obj_t *viewer = nullptr, *photo = nullptr, *title = nullptr, *subtitle = nullptr, *status = nullptr,
              *trash = nullptr, *trash_label = nullptr;
    std::vector<lv_obj_t *> cells;
    std::vector<Picture> thumbs;
    std::vector<bool> asked;
    Picture full;
    int shown = -1, pending_show = -2;
    uint32_t generation = 0, revision = ~0u, delete_armed_at = 0;
    lv_point_t press = {};
} U;

void post(int index, bool full)
{
    Job j{index, full, U.generation};
    xQueueSend(jobs, &j, 0);
}

void clear_thumbs()
{
    for (Picture &p : U.thumbs) {
        release(p);
    }
    U.thumbs.clear();
    U.asked.clear();
}

void show_photo(int index);
void close_viewer();

void cell_cb(lv_event_t *e)
{
    show_photo(int(intptr_t(lv_event_get_user_data(e))));
}

// The grid, newest first.
void build_grid()
{
    ++U.generation;
    lv_obj_clean(U.grid);
    U.cells.clear();
    clear_thumbs();
    const int n = camera_app_photo_count();
    U.thumbs.resize(size_t(n));
    U.asked.assign(size_t(n), false);
    char text[32];
    snprintf(text, sizeof(text), n == 1 ? "1 photo" : "%d photos", n);
    lv_label_set_text(U.count, text);
    if (!n) {
        const int state = camera_app_photos_state();
        lv_label_set_text(U.empty, state == 0    ? "Loading photos from the NAS\xE2\x80\xA6"
                          : state == -1 ? "The NAS is unreachable. Photos will show when it answers."
                          : "No photos yet. Take some with the Camera.");
        lv_obj_remove_flag(U.empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(U.empty, LV_OBJ_FLAG_HIDDEN);
    }
    for (int k = 0; k < n; ++k) {
        const int index = n - 1 - k;
        lv_obj_t *c = box(U.grid, (k % COLS) * (THUMB + GAP), (k / COLS) * (THUMB + GAP), THUMB, THUMB, 14);
        lv_obj_set_style_bg_color(c, lv_color_hex(CARD), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(c, true, 0);
        lv_obj_add_flag(c, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
        press_fx(c);
        lv_obj_add_event_cb(c, cell_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(index)));
        U.cells.push_back(c);
    }
    box(U.grid, 0, ((n + COLS - 1) / COLS) * (THUMB + GAP), 1, 8);  // room below the last row
    U.revision = camera_app_photos_revision();
}

// Thumbnails for the visible rows (and the next), a few at a time.
void request_visible()
{
    const int n = int(U.thumbs.size());
    const int top = lv_obj_get_scroll_y(U.grid) / (THUMB + GAP);
    int asked = 0;
    for (int row = std::max(0, top - 1); row <= top + 4 && asked < 6; ++row)
        for (int col = 0; col < COLS; ++col) {
            const int k = row * COLS + col;
            if (k >= n) {
                break;
            }
            const int index = n - 1 - k;
            if (U.asked[size_t(index)]) {
                continue;
            }
            U.asked[size_t(index)] = true;
            post(index, false);
            ++asked;
        }
}

void place_thumb(int index)
{
    const int n = int(U.thumbs.size()), k = n - 1 - index;
    if (k < 0 || k >= int(U.cells.size())) {
        return;
    }
    lv_obj_t *img = lv_image_create(U.cells[size_t(k)]);
    lv_image_set_src(img, &U.thumbs[size_t(index)].dsc);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    bool on_nas = false;
    if (camera_app_photo_info(index, nullptr, &on_nas, nullptr) && on_nas) {
        lv_obj_t *mark = box(U.cells[size_t(k)], THUMB - 30, THUMB - 30, 24, 24, 12);
        lv_obj_set_style_bg_color(mark, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(mark, 150, 0);
        icon(mark, "cloud", 3, 3, 18, TEXT, 1.6f);
    }
}

// ---- viewer -----------------------------------------------------------------------------------
void viewer_text(int index)
{
    const int n = camera_app_photo_count();
    char title[32], sub[64] = "";
    snprintf(title, sizeof(title), "%d of %d", n - index, n);
    uint32_t at = 0, size = 0;
    bool on_nas = false;
    if (camera_app_photo_info(index, &at, &on_nas, &size)) {
        char when[32] = "";
        if (at) {
            const time_t t = at;
            struct tm local;
            localtime_r(&t, &local);
            strftime(when, sizeof(when), "%d %b, %H:%M \xC2\xB7 ", &local);
        }
        snprintf(sub, sizeof(sub), "%s%u KB \xC2\xB7 %s", when, unsigned(size / 1024), on_nas ? "on the NAS" : "on the board");
    }
    lv_label_set_text(U.title, title);
    lv_label_set_text(U.subtitle, sub);
}

void disarm_delete()
{
    U.delete_armed_at = 0;
    lv_obj_set_style_bg_color(U.trash, lv_color_hex(FIELD), 0);
    lv_label_set_text(U.trash_label, "Delete");
    lv_obj_set_style_text_color(U.trash_label, lv_color_hex(TEXT), 0);
}

void show_photo(int index)
{
    const int n = camera_app_photo_count();
    if (!n) {
        close_viewer();
        return;
    }
    index = std::clamp(index, 0, n - 1);
    U.shown = index;
    lv_obj_remove_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(U.viewer);
    disarm_delete();
    viewer_text(index);
    // The thumbnail at once (if there is one), the full photo when decoded.
    lv_image_set_src(U.photo, nullptr);
    if (index < int(U.thumbs.size()) && U.thumbs[size_t(index)].px) {
        lv_image_set_src(U.photo, &U.thumbs[size_t(index)].dsc);
        lv_image_set_scale(U.photo, 256 * FULL / THUMB);
    }
    lv_label_set_text(U.status, "");
    post(index, true);
}

void close_viewer()
{
    U.shown = -1;
    lv_obj_add_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(U.photo, nullptr);
    release(U.full);
}

void step(int by)
{
    const int n = camera_app_photo_count();
    if (U.shown < 0 || !n) {
        return;
    }
    const int next = U.shown - by;  // left: older (the grid is newest first)
    if (next < 0 || next >= n) {
        return;
    }
    show_photo(next);
}

// Swipes: from where the finger went down to where it came up.
void viewer_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        U.press = p;
        return;
    }
    const int dx = p.x - U.press.x, dy = p.y - U.press.y;
    if (abs(dx) > 50 && abs(dx) > 2 * abs(dy)) {
        step(dx < 0 ? 1 : -1);
    }
}

void trash_cb(lv_event_t *)
{
    const uint32_t now = lv_tick_get();
    if (!U.delete_armed_at || now - U.delete_armed_at > 4000) {  // first tap: ask
        U.delete_armed_at = now;
        lv_obj_set_style_bg_color(U.trash, lv_color_hex(0x3A1210), 0);
        lv_label_set_text(U.trash_label, "Tap again to delete");
        lv_obj_set_style_text_color(U.trash_label, lv_color_hex(RED), 0);
        return;
    }
    const int index = U.shown;
    disarm_delete();
    if (index < 0 || !camera_app_delete_photo(index)) {
        return;
    }
    build_grid();
    const int n = camera_app_photo_count();
    if (!n) {
        close_viewer();
    } else {
        show_photo(std::min(index, n - 1));
    }
}

lv_obj_t *back_button(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent, 56, 14, 44, 44, 22);
    lv_obj_set_style_bg_color(b, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    icon(b, "back", 10, 10, 24, TEXT, 2.0f);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

void tick(lv_timer_t *)
{
    if (!U.active) {
        return;
    }
    if (camera_app_photos_revision() != U.revision && U.shown < 0) {
        build_grid();
    }
    Done d;
    while (done && xQueueReceive(done, &d, 0) == pdTRUE) {
        if (d.generation != U.generation || !d.ok) {
            release(d.pic);
            if (d.full && d.index == U.shown && d.generation == U.generation) {
                lv_label_set_text(U.status, "This photo could not be opened.");
            }
            continue;
        }
        if (d.full) {
            if (d.index != U.shown) {
                release(d.pic);
                continue;
            }
            release(U.full);
            U.full = d.pic;
            lv_image_set_src(U.photo, &U.full.dsc);
            lv_image_set_scale(U.photo, 256);
        } else if (d.index < int(U.thumbs.size())) {
            release(U.thumbs[size_t(d.index)]);
            U.thumbs[size_t(d.index)] = d.pic;
            place_thumb(d.index);
        } else {
            release(d.pic);
        }
    }
    if (U.shown < 0) {
        request_visible();
    }
}

void build(lv_obj_t *s)
{
    U.screen = s;
    back_button(s, [](lv_event_t *) {
        if (U.exit) {
            U.exit();
        }
    });
    label(s, "Gallery", 112, 12, 300, TEXT, 24, true);
    U.count = label(s, "", 112, 44, 300, TEXT3, 13);
    U.grid = box(s, 38, 74, COLS * THUMB + (COLS - 1) * GAP, 406);
    lv_obj_add_flag(U.grid, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM));
    lv_obj_set_scroll_dir(U.grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.grid, LV_SCROLLBAR_MODE_OFF);
    U.empty = label(s, "No photos yet. Take some with the Camera.", 60, 220, 360, TEXT3, 16, false, LV_TEXT_ALIGN_CENTER);
    wrap_label(U.empty);
    // The viewer: the photo full screen, the same bar as every app.
    U.viewer = box(s, 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(U.viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.viewer, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.viewer, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_event_cb(U.viewer, viewer_touch_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(U.viewer, viewer_touch_cb, LV_EVENT_RELEASED, nullptr);
    U.photo = lv_image_create(U.viewer);
    lv_obj_center(U.photo);
    lv_obj_remove_flag(U.photo, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_antialias(U.photo, false);
    // A dark band behind the bar so it reads over any photo.
    lv_obj_t *band = box(U.viewer, 0, 0, PANEL, 66);
    lv_obj_set_style_bg_color(band, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(band, 150, 0);
    back_button(U.viewer, [](lv_event_t *) {
        close_viewer();
    });
    U.title = label(U.viewer, "", 112, 10, 280, TEXT, 20, true);
    U.subtitle = label(U.viewer, "", 112, 38, 300, TEXT2, 13);
    U.status = label(U.viewer, "", 40, 230, 400, WARN, 16, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *left = box(U.viewer, 74, 394, 52, 52, 26);
    lv_obj_t *right = box(U.viewer, 354, 394, 52, 52, 26);
    for (lv_obj_t *b : {
                left, right
            }) {
        lv_obj_set_style_bg_color(b, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(b, 170, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        press_fx(b);
    }
    icon(left, "back", 14, 14, 24, TEXT, 2.0f);
    icon(right, "chev", 14, 14, 24, TEXT, 2.0f);
    lv_obj_add_event_cb(left, [](lv_event_t *) {
        step(-1);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(right, [](lv_event_t *) {
        step(1);
    }, LV_EVENT_CLICKED, nullptr);
    U.trash = box(U.viewer, 140, 394, 200, 52, 26);
    lv_obj_set_style_bg_color(U.trash, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(U.trash, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.trash, LV_OBJ_FLAG_CLICKABLE);
    press_fx(U.trash);
    icon(U.trash, "trash", 18, 14, 24, RED, 1.8f);
    U.trash_label = label(U.trash, "Delete", 46, 16, 150, TEXT, 15, true);
    lv_obj_add_event_cb(U.trash, trash_cb, LV_EVENT_CLICKED, nullptr);
    lv_timer_create(tick, 60, nullptr);
    U.built = true;
    (void)ACC;
}
}  // namespace

extern "C" void gallery_app_set_exit(void (*exit)(void))
{
    U.exit = exit;
}

extern "C" void gallery_app_show(int index)
{
    U.pending_show = index;
}

extern "C" void gallery_app_open(lv_obj_t *screen)
{
    if (!jobs) {
        jobs = xQueueCreate(16, sizeof(Job));
        done = xQueueCreate(8, sizeof(Done));
        // Network (NAS photos) and decoding only: its stack can live in PSRAM.
        xTaskCreatePinnedToCoreWithCaps(worker, "gallery", 10 * 1024, nullptr, 3, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!U.built) {
        build(screen);
    }
    U.active = true;
    camera_app_refresh_photos();  // photos added to the NAS folder meanwhile
    build_grid();
    if (U.pending_show >= -1) {
        const int n = camera_app_photo_count();
        if (n) {
            show_photo(U.pending_show < 0 ? n - 1 : U.pending_show);
        }
    } else {
        close_viewer();
    }
    U.pending_show = -2;
}

extern "C" void gallery_app_set_active(bool active)
{
    U.active = active && U.built;
    if (!U.active && U.built) {
        close_viewer();
        clear_thumbs();  // memory back (~32 KB each)
        lv_obj_clean(U.grid);
        U.cells.clear();
        U.revision = ~0u;
    }
}
