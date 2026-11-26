// SPDX-License-Identifier: Apache-2.0
// Gallery: the camera's photos as a grid of thumbnails, and one at a time full
// screen: swipe left and right, tap to hide or show the controls, double-tap
// or pinch to zoom (then drag to look around), delete. Photos come from
// camera_app (memory, or the NAS for older ones); a worker task decodes them
// off the LVGL task.
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
#include "src/misc/cache/instance/lv_image_cache.h"  // the zoomed view changes in place

namespace {
using namespace mui;
const char *TAG = "gallery";
constexpr int THUMB = 128, GAP = 10, COLS = 3, FULL = 480;
// Zoom: up to 4x (the photo is 480 px, as the panel), a double-tap goes to 2.5x.
constexpr float ZOOM_MAX = 4.0f, ZOOM_TAP = 2.5f;
// A tap moves less than this (LVGL's scroll limit, set by the shell), and a
// double tap's second tap comes down this soon after the first.
constexpr int TAP_SLOP = 20, DOUBLE_TAP_MS = 260, DOUBLE_TAP_PX = 60;
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
    lv_obj_t *chrome[2] = {};  // the controls: the top bar and the buttons at the bottom
    bool chrome_shown = true;
    lv_timer_t *tap_timer = nullptr;  // a single tap waits this long for a second one
    std::vector<lv_obj_t *> cells;
    std::vector<Picture> thumbs;
    std::vector<bool> asked;
    Picture full;
    int shown = -1, pending_show = -2;
    uint32_t generation = 0, revision = ~0u, delete_armed_at = 0;
    // The photo's zoom (1: fills the panel) and where its centre is on the panel.
    float zoom = 1, cx = FULL / 2, cy = FULL / 2;
    const Picture *pic = nullptr;  // what the viewer shows: the photo, or its thumbnail until decoded
    // The zoomed view, drawn here: LVGL's own image transform took ~42 ms a frame.
    Picture view;
    bool view_smooth = false;  // filtered (once the photo settles), not the nearest pixels
    lv_timer_t *settle_timer = nullptr;
} U;

// One touch on the viewer, from the first finger down to the last one up.
struct {
    lv_point_t down = {};
    uint32_t down_at = 0, tap_at = 0;
    lv_point_t tap = {};    // the last single tap, while it may become a double tap
    bool moved = false;     // went past TAP_SLOP
    bool pinched = false;   // a second finger came down
    bool second = false;    // came down soon after a tap: may be a double tap
    int fingers = 0;        // fingers at the last update
    // The move so far is measured from these, taken when the finger count changes.
    float zoom = 1, cx = 0, cy = 0, mx = 0, my = 0, spread = 1;
    float last_mx = 0, last_my = 0;  // between the fingers at the last pinch update
    float vx = 0, vy = 0;            // a drag's speed (px/ms), for the glide after it
    lv_point_t last = {};
    uint32_t last_at = 0;
} G;

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

// ---- zoom ---------------------------------------------------------------------------------------
// The centre that keeps the zoomed photo over the whole panel (at 1x and
// below: the middle).
float clamp_centre(float c, float zoom)
{
    const float half = FULL / 2 * zoom;
    return std::clamp(c, std::min(FULL - half, FULL / 2.0f), std::max(half, FULL / 2.0f));
}

// RGB565 with its fields spread apart (G high, R and B low), so two colours
// blend with one multiply each: weights out of 32.
inline uint32_t spread(uint16_t c)
{
    return (c | uint32_t(c) << 16) & 0x07E0F81Fu;
}
inline uint32_t blend(uint32_t a, uint32_t b, uint32_t w)
{
    return ((a * (32 - w) + b * w) >> 5) & 0x07E0F81Fu;
}

// Draws the shown picture into the view as the panel shows it, zoomed about
// (cx, cy): the nearest pixels while it moves (~4 ms), filtered once it
// settles (~20 ms). Outside the picture is black.
void render_view(bool smooth)
{
    const Picture &p = *U.pic;
    const float s = U.zoom * FULL / p.w;  // panel pixels per picture pixel
    const float left = U.cx - FULL / 2 * U.zoom, top = U.cy - FULL / 2 * U.zoom;
    static int16_t xi[FULL];
    static uint8_t xf[FULL];
    int xa = FULL, xb = 0;  // the columns over the picture
    for (int x = 0; x < FULL; ++x) {
        // Where this panel pixel's centre falls on the picture (filtering
        // blends the two picture pixels whose centres are either side of it).
        const float at = (x + 0.5f - left) / s, u = smooth ? at - 0.5f : at;
        const int i = int(floorf(u));
        xi[x] = int16_t(std::clamp(i, 0, p.w - 1));
        xf[x] = uint8_t(smooth && i >= 0 && i < p.w - 1 ? lroundf((u - i) * 32) : 0);
        if (at >= 0 && at < p.w) {
            xa = std::min(xa, x), xb = std::max(xb, x + 1);
        }
    }
    for (int y = 0; y < FULL; ++y) {
        uint16_t *out = U.view.px + y * FULL;
        const float at = (y + 0.5f - top) / s, v = smooth ? at - 0.5f : at;
        const int j = int(floorf(v));
        if (at < 0 || at >= p.h || xa >= xb) {
            memset(out, 0, FULL * 2);
            continue;
        }
        memset(out, 0, size_t(xa) * 2);
        memset(out + xb, 0, size_t(FULL - xb) * 2);
        if (!smooth) {
            const uint16_t *row = p.px + std::clamp(j, 0, p.h - 1) * p.w;
            for (int x = xa; x < xb; ++x) {
                out[x] = row[xi[x]];
            }
            continue;
        }
        const int j0 = std::clamp(j, 0, p.h - 1), j1 = std::min(j0 + 1, p.h - 1);
        const uint32_t fy = j < 0 || j >= p.h - 1 ? 0 : uint32_t(lroundf((v - j) * 32));
        const uint16_t *r0 = p.px + j0 * p.w, *r1 = p.px + j1 * p.w;
        for (int x = xa; x < xb; ++x) {
            const int i0 = xi[x], i1 = std::min(i0 + 1, p.w - 1);
            const uint32_t fx = xf[x];
            const uint32_t c = blend(blend(spread(r0[i0]), spread(r0[i1]), fx), blend(spread(r1[i0]), spread(r1[i1]), fx), fy);
            out[x] = uint16_t(c | c >> 16);
        }
    }
    U.view_smooth = smooth;
}

// Shows the picture at the zoom: as it is when it fills the panel, else
// drawn into the view (and filtered a moment after the last move).
void apply_zoom()
{
    if (!U.pic || !U.pic->px) {
        lv_image_set_src(U.photo, nullptr);
        return;
    }
    if (fabsf(U.zoom - 1) < 0.001f && U.pic->w == FULL) {
        if (lv_image_get_src(U.photo) != &U.pic->dsc) {
            lv_image_set_src(U.photo, &U.pic->dsc);
        }
        lv_timer_pause(U.settle_timer);
        return;
    }
    if (!U.view.px) {
        auto *px = static_cast<uint16_t *>(heap_caps_aligned_alloc(64, FULL * FULL * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!px) {
            ESP_LOGW(TAG, "no memory to zoom");
            return;
        }
        U.view.px = px;
        U.view.w = U.view.h = FULL;
        U.view.dsc = U.pic->dsc;
        U.view.dsc.header.w = U.view.dsc.header.h = FULL;
        U.view.dsc.header.stride = FULL * 2;
        U.view.dsc.data = reinterpret_cast<const uint8_t *>(px);
        U.view.dsc.data_size = FULL * FULL * 2;
    }
    render_view(false);
    if (lv_image_get_src(U.photo) != &U.view.dsc) {
        lv_image_set_src(U.photo, &U.view.dsc);
    } else {
        lv_image_cache_drop(&U.view.dsc);
        lv_obj_invalidate(U.photo);
    }
    if (U.pic->w == FULL) {  // a thumbnail is about to be replaced: not worth filtering
        lv_timer_reset(U.settle_timer);
        lv_timer_resume(U.settle_timer);
    }
}

void settle_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (U.pic && U.pic->px && lv_image_get_src(U.photo) == &U.view.dsc && !U.view_smooth) {
        render_view(true);
        lv_image_cache_drop(&U.view.dsc);
        lv_obj_invalidate(U.photo);
    }
}

void free_view()
{
    if (U.view.px) {
        if (lv_image_get_src(U.photo) == &U.view.dsc) {
            lv_image_set_src(U.photo, nullptr);
        }
        heap_caps_free(U.view.px);
        U.view = Picture{};
    }
}

void set_zoom(float zoom, float cx, float cy)
{
    U.zoom = zoom;
    U.cx = clamp_centre(cx, zoom);
    U.cy = clamp_centre(cy, zoom);
    apply_zoom();
}

// Animated: from where the photo is to (zoom, cx, cy).
struct {
    float zoom, cx, cy;  // from
    float to_zoom, to_cx, to_cy;
} Z;

void zoom_anim_cb(void *, int32_t t)
{
    const float k = t / 1024.0f;
    set_zoom(Z.zoom + (Z.to_zoom - Z.zoom) * k, Z.cx + (Z.to_cx - Z.cx) * k, Z.cy + (Z.to_cy - Z.cy) * k);
}

void zoom_to(float zoom, float cx, float cy, uint32_t ms = 220)
{
    lv_anim_delete(&Z, zoom_anim_cb);
    Z = {U.zoom, U.cx, U.cy, zoom, clamp_centre(cx, zoom), clamp_centre(cy, zoom)};
    if (fabsf(Z.to_zoom - Z.zoom) < 0.002f && fabsf(Z.to_cx - Z.cx) < 0.5f && fabsf(Z.to_cy - Z.cy) < 0.5f) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &Z);
    lv_anim_set_exec_cb(&a, zoom_anim_cb);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// A double tap: in at the tapped point (it stays under the finger), or back out.
void toggle_zoom(lv_point_t at)
{
    if (U.zoom > 1.01f) {
        zoom_to(1, FULL / 2, FULL / 2);
        return;
    }
    const float k = ZOOM_TAP / U.zoom;
    zoom_to(ZOOM_TAP, at.x - (at.x - U.cx) * k, at.y - (at.y - U.cy) * k);
}

// ---- controls -----------------------------------------------------------------------------------
void chrome_opa_cb(void *o, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(o), lv_opa_t(v), 0);
}

void chrome_hidden_cb(lv_anim_t *a)
{
    if (!U.chrome_shown) {
        lv_obj_add_flag(static_cast<lv_obj_t *>(a->var), LV_OBJ_FLAG_HIDDEN);
    }
}

// The controls fade out of the way of the photo (hidden, they take no taps), and back.
void show_chrome(bool shown, bool animate = true)
{
    if (shown == U.chrome_shown && animate) {
        return;
    }
    U.chrome_shown = shown;
    for (lv_obj_t *o : U.chrome) {
        lv_anim_delete(o, chrome_opa_cb);
        const lv_opa_t from = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? lv_opa_t(LV_OPA_TRANSP) : lv_obj_get_style_opa(o, LV_PART_MAIN);
        const lv_opa_t to = shown ? LV_OPA_COVER : LV_OPA_TRANSP;
        if (shown) {
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
        if (!animate) {
            lv_obj_set_style_opa(o, to, 0);
            mui::set_hidden(o, !shown);
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_exec_cb(&a, chrome_opa_cb);
        lv_anim_set_values(&a, from, to);
        lv_anim_set_duration(&a, 180);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, chrome_hidden_cb);
        lv_anim_start(&a);
    }
}

void cancel_tap()
{
    G.tap_at = 0;
    lv_timer_pause(U.tap_timer);
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
    if (lv_obj_has_flag(U.viewer, LV_OBJ_FLAG_HIDDEN)) {
        show_chrome(true, false);  // opening from the grid: with the controls
    }
    lv_obj_remove_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(U.viewer);
    disarm_delete();
    viewer_text(index);
    lv_anim_delete(&Z, zoom_anim_cb);
    U.zoom = 1, U.cx = U.cy = FULL / 2;
    // The thumbnail at once (if there is one), the full photo when decoded.
    lv_image_set_src(U.photo, nullptr);  // before the picture it shows goes
    release(U.full);
    U.pic = index < int(U.thumbs.size()) && U.thumbs[size_t(index)].px ? &U.thumbs[size_t(index)] : nullptr;
    apply_zoom();
    lv_label_set_text(U.status, "");
    post(index, true);
}

void close_viewer()
{
    U.shown = -1;
    lv_obj_add_flag(U.viewer, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(U.photo, nullptr);
    U.pic = nullptr;
    release(U.full);
    free_view();
    lv_anim_delete(&Z, zoom_anim_cb);
    if (U.tap_timer) {
        cancel_tap();
    }
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

// ---- touch on the photo -------------------------------------------------------------------------
// The fingers now: both from the touch panel while two are down, or LVGL's pointer.
int fingers(lv_point_t pointer, lv_point_t t[2])
{
    const int n = mui::touches(t);
    if (n < 1) {
        t[0] = pointer;
        return 1;
    }
    return std::min(n, 2);
}

// Measure the move from here on (the finger count changed, or it began).
void anchor(int n, const lv_point_t *t)
{
    G.fingers = n;
    G.zoom = U.zoom, G.cx = U.cx, G.cy = U.cy;
    G.mx = n == 2 ? (t[0].x + t[1].x) / 2.0f : t[0].x;
    G.my = n == 2 ? (t[0].y + t[1].y) / 2.0f : t[0].y;
    G.spread = n == 2 ? std::max(20.0f, hypotf(float(t[1].x - t[0].x), float(t[1].y - t[0].y))) : 1;
}

void touch_move(lv_point_t pointer)
{
    lv_point_t t[2];
    const int n = fingers(pointer, t);
    if (abs(pointer.x - G.down.x) > TAP_SLOP || abs(pointer.y - G.down.y) > TAP_SLOP) {
        G.moved = true;
    }
    if (n != G.fingers) {
        if (n == 2 && !G.pinched) {
            G.pinched = true;
            show_chrome(false);
        }
        anchor(n, t);
        return;
    }
    if (n == 2) {
        // Pinch: the photo point between the fingers stays between them, and
        // grows or shrinks with their spread (a little past the limits, then
        // it springs back on release).
        const float mx = (t[0].x + t[1].x) / 2.0f, my = (t[0].y + t[1].y) / 2.0f;
        const float spread = hypotf(float(t[1].x - t[0].x), float(t[1].y - t[0].y));
        const float zoom = std::clamp(G.zoom * spread / G.spread, 0.7f, ZOOM_MAX * 1.25f);
        const float k = zoom / G.zoom;
        set_zoom(zoom, mx - (G.mx - G.cx) * k, my - (G.my - G.cy) * k);
        G.last_mx = mx, G.last_my = my;
    } else if (U.zoom > 1.001f && (G.moved || G.pinched)) {
        set_zoom(U.zoom, G.cx + (t[0].x - G.mx), G.cy + (t[0].y - G.my));  // drag the zoomed photo
    }
    // The finger's speed, smoothed: it fades while the finger rests.
    const uint32_t now = lv_tick_get();
    if (n == 1 && G.last_at && now > G.last_at) {
        const float dt = float(now - G.last_at);
        G.vx = G.vx * 0.6f + (t[0].x - G.last.x) / dt * 0.4f;
        G.vy = G.vy * 0.6f + (t[0].y - G.last.y) / dt * 0.4f;
    } else if (n != 1) {
        G.vx = G.vy = 0;
    }
    G.last = t[0], G.last_at = now;
}

void tap_timer_cb(lv_timer_t *)
{
    cancel_tap();
    show_chrome(!U.chrome_shown);
}

void viewer_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const uint32_t now = lv_tick_get();
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_anim_delete(&Z, zoom_anim_cb);  // a finger stops the photo where it is
        G.down = p, G.down_at = now;
        G.moved = G.pinched = false;
        G.second = G.tap_at && now - G.tap_at <= DOUBLE_TAP_MS;
        if (G.second) {
            lv_timer_pause(U.tap_timer);  // decided when this one comes up
        }
        G.fingers = 0;
        G.vx = G.vy = 0, G.last_at = 0;
        touch_move(p);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        touch_move(p);
        return;
    }
    // Released (or lost).
    const bool tap = !G.moved && !G.pinched && now - G.down_at < 400;
    if (G.second) {
        const bool near = abs(p.x - G.tap.x) < DOUBLE_TAP_PX && abs(p.y - G.tap.y) < DOUBLE_TAP_PX;
        const bool was_tap = G.tap_at != 0;
        cancel_tap();
        G.second = false;
        if (tap && near) {
            if (U.zoom <= 1.01f) {
                show_chrome(false);
            }
            toggle_zoom(G.tap);
            return;
        }
        if (was_tap) {
            show_chrome(!U.chrome_shown);  // the first tap stood alone
        }
    }
    if (tap) {
        G.tap = p, G.tap_at = now;
        lv_timer_reset(U.tap_timer);
        lv_timer_resume(U.tap_timer);
        return;
    }
    if (G.pinched || U.zoom > 1.001f) {
        // Spring back inside the limits (past the largest: about where the
        // fingers were) and over the whole panel.
        const float zoom = std::clamp(U.zoom, 1.0f, ZOOM_MAX), k = zoom / U.zoom;
        if (U.zoom > ZOOM_MAX) {
            zoom_to(zoom, G.last_mx - (G.last_mx - U.cx) * k, G.last_my - (G.last_my - U.cy) * k);
        } else if (G.fingers == 1 && hypotf(G.vx, G.vy) > 0.3f) {
            // Let go while dragging: it glides on and slows to a stop (an
            // ease-out over 450 ms starts at three times its average speed).
            zoom_to(zoom, U.cx + G.vx * 150, U.cy + G.vy * 150, 450);
        } else {
            zoom_to(zoom, U.cx, U.cy);
        }
        return;
    }
    const int dx = p.x - G.down.x, dy = p.y - G.down.y;  // a swipe at 1x: the next or previous photo
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
            lv_image_set_src(U.photo, nullptr);  // before the picture it shows goes
            release(U.full);
            U.full = d.pic;
            U.pic = &U.full;
            apply_zoom();  // over the thumbnail, at the same zoom
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
    // Its own drags (pan, pinch, swipe): never a scroll of the screen behind.
    lv_obj_remove_flag(U.viewer, LV_OBJ_FLAG_SCROLL_CHAIN);
    for (lv_event_code_t code : {
                LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST
            }) {
        lv_obj_add_event_cb(U.viewer, viewer_touch_cb, code, nullptr);
    }
    U.tap_timer = lv_timer_create(tap_timer_cb, DOUBLE_TAP_MS, nullptr);
    lv_timer_pause(U.tap_timer);
    U.settle_timer = lv_timer_create(settle_cb, 120, nullptr);
    lv_timer_pause(U.settle_timer);
    U.photo = lv_image_create(U.viewer);
    lv_obj_center(U.photo);
    lv_obj_remove_flag(U.photo, LV_OBJ_FLAG_CLICKABLE);
    U.status = label(U.viewer, "", 40, 230, 400, WARN, 16, true, LV_TEXT_ALIGN_CENTER);
    // The controls, in two groups that fade together: the bar on a dark band
    // so it reads over any photo, and the buttons at the bottom.
    lv_obj_t *top = U.chrome[0] = box(U.viewer, 0, 0, PANEL, 66);
    lv_obj_set_style_bg_color(top, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(top, 150, 0);
    back_button(top, [](lv_event_t *) {
        close_viewer();
    });
    U.title = label(top, "", 112, 10, 280, TEXT, 20, true);
    U.subtitle = label(top, "", 112, 38, 300, TEXT2, 13);
    lv_obj_t *bottom = U.chrome[1] = box(U.viewer, 74, 394, 332, 52);
    lv_obj_t *left = box(bottom, 0, 0, 52, 52, 26);
    lv_obj_t *right = box(bottom, 280, 0, 52, 52, 26);
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
    U.trash = box(bottom, 66, 0, 200, 52, 26);
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
