// SPDX-License-Identifier: Apache-2.0
// Swifty's screen: the 3D figure at the top, the chat below it, and a bottom
// row with the camera, a hold-to-talk pill and the keyboard. Worker tasks
// (figure, speech, agent, image loading) post closures to the LVGL task.
#include "swifty_app.h"

#include <math.h>
#include <string.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "board_mic.h"
#include "camera_app.h"
#include "esp_iris.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mui.h"
#include "png.h"
#include "swifty_agent.h"
#include "swifty_asr.h"
#include "swifty_config.h"
#include "swifty_figure.h"
#include "swifty_md.h"
#include "swifty_paseo.h"

using namespace mui;
using namespace swifty;

namespace {
const char *TAG = "swifty";
// The chat starts just below the bird (y -4 to 131): the bird redraws ~12
// times a second, and any overlap redrew the chat's top bubbles with it, a cost
// that grew with the conversation.
constexpr int W = 480, FIG = 136, CHAT_X = 24, CHAT_Y = 132, CHAT_W = 432, CHAT_H = 266, BUBBLE = 360;
constexpr uint32_t SKY = 0x35B2FF, SKY_HI = 0x7DF3FF, SKY_DEEP = 0x2B5CF2, BUBBLE_ME = 0x1F4FD8, BUBBLE_SWIFTY = 0x161A22;
constexpr int BARS = 15;
const int BAR_MAX[BARS] = {18, 22, 28, 36, 44, 52, 58, 64, 58, 52, 44, 36, 28, 22, 18};
const int BAR_ORDER[BARS] = {7, 6, 8, 5, 9, 4, 10, 3, 11, 2, 12, 1, 13, 0, 14};

// ---- images -------------------------------------------------------------------------------
struct Image {
    lv_image_dsc_t dsc = {};
    uint8_t *px = nullptr;
};

void free_image(lv_event_t *e)
{
    auto *img = static_cast<Image *>(lv_event_get_user_data(e));
    heap_caps_free(img->px);
    delete img;
}

// Box-filters RGB565 (or ARGB8888 with bpp 4) down to fit max_w x max_h.
Image *make_image(const uint8_t *src, int w, int h, int bpp, int max_w, int max_h)
{
    const float k = std::min({1.f, float(max_w) / w, float(max_h) / h});
    const int ow = std::max(1, int(w * k)), oh = std::max(1, int(h * k));
    auto *img = new Image;
    img->px = static_cast<uint8_t *>(heap_caps_malloc(size_t(ow) * oh * bpp, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!img->px) {
        delete img;
        return nullptr;
    }
    for (int y = 0; y < oh; ++y) {
        const int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; ++x) {
            const int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 3)
                for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 3, ++n) {
                    const uint8_t *p = src + (size_t(yy) * w + xx) * bpp;
                    if (bpp == 2) {
                        const uint16_t v = uint16_t(p[0] | p[1] << 8);
                        acc[0] += v >> 11, acc[1] += (v >> 5) & 63, acc[2] += v & 31;
                    } else {
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += p[c];
                        }
                    }
                }
            uint8_t *o = img->px + (size_t(y) * ow + x) * bpp;
            if (bpp == 2) {
                const uint16_t v = uint16_t((acc[0] / n) << 11 | (acc[1] / n) << 5 | (acc[2] / n));
                o[0] = uint8_t(v), o[1] = uint8_t(v >> 8);
            } else {
                for (int c = 0; c < 4; ++c) {
                    o[c] = uint8_t(acc[c] / n);
                }
            }
        }
    }
    img->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img->dsc.header.cf = bpp == 2 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;
    img->dsc.header.w = uint32_t(ow);
    img->dsc.header.h = uint32_t(oh);
    img->dsc.header.stride = uint32_t(ow * bpp);
    img->dsc.data = img->px;
    img->dsc.data_size = uint32_t(ow * oh * bpp);
    return img;
}

Image *decode_jpeg(const uint8_t *data, size_t size, int max_w, int max_h)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return nullptr;
    }
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t *>(data);
    io.inbuf_len = int(size);
    jpeg_dec_header_info_t header = {};
    int outlen = 0;
    Image *img = nullptr;
    if (jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK && jpeg_dec_get_outbuf_len(dec, &outlen) == JPEG_ERR_OK &&
            header.width * header.height <= 2048 * 2048) {
        auto *raw = static_cast<uint8_t *>(jpeg_calloc_align(size_t(outlen), 16));
        if (raw) {
            io.outbuf = raw;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                img = make_image(raw, header.width, header.height, 2, max_w, max_h);
            }
            jpeg_free_align(raw);
        }
    }
    jpeg_dec_close(dec);
    return img;
}

Image *decode_png(const uint8_t *data, size_t size, int max_w, int max_h)
{
    png_image png = {};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, data, size)) {
        return nullptr;
    }
    if (png.width * png.height > 1024 * 1024) {
        png_image_free(&png);
        return nullptr;
    }
    png.format = PNG_FORMAT_BGRA;  // LVGL ARGB8888 in memory
    const size_t bytes = PNG_IMAGE_SIZE(png);
    auto *raw = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    Image *img = nullptr;
    if (raw && png_image_finish_read(&png, nullptr, raw, 0, nullptr)) {
        img = make_image(raw, int(png.width), int(png.height), 4, max_w, max_h);
    }
    heap_caps_free(raw);
    png_image_free(&png);
    return img;
}

Image *decode(const uint8_t *data, size_t size, int max_w, int max_h)
{
    if (size > 3 && data[0] == 0xFF && data[1] == 0xD8) {
        return decode_jpeg(data, size, max_w, max_h);
    }
    if (size > 8 && !memcmp(data, "\x89PNG", 4)) {
        return decode_png(data, size, max_w, max_h);
    }
    return nullptr;
}

lv_obj_t *show_image(lv_obj_t *parent, Image *img, int radius)
{
    lv_obj_t *o = lv_image_create(parent);
    lv_image_set_src(o, &img->dsc);
    lv_obj_add_event_cb(o, free_image, LV_EVENT_DELETE, img);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_clip_corner(o, true, 0);
    return o;
}

// ---- state --------------------------------------------------------------------------------
struct {
    bool built = false;
    swifty_hooks_t hooks = {};
    std::atomic<bool> active{false};
    lv_obj_t *screen = nullptr, *figure = nullptr, *chat = nullptr, *fade = nullptr, *empty = nullptr, *empty_hint = nullptr,
              *pill = nullptr, *pill_label = nullptr, *pill_icon = nullptr, *wave = nullptr, *bars[BARS] = {},
                                                                                                           *camera_btn = nullptr, *keys_btn = nullptr, *attach_row = nullptr, *entry = nullptr, *entry_ta = nullptr;
    // the reply being written
    lv_obj_t *reply = nullptr, *reply_body = nullptr, *reply_tool = nullptr, *draft = nullptr;
    // While a reply streams in: its finished paragraphs (rendered once each) and
    // the one still growing (rendered again on each update).
    lv_obj_t *reply_done = nullptr, *reply_live = nullptr;
    size_t reply_done_len = 0;
    std::string reply_text;
    bool reply_dirty = false;
    uint32_t reply_drawn = 0;
    std::vector<Jpeg> attached;
    bool listening = false, recognizing = false, ai_key = false;
    std::string status = "Ready", last;
    unsigned conversation = 0;  // a new one (another person) drops replies still coming
    // posted from other tasks
    std::mutex lock;
    std::vector<std::function<void()>> posts;
    // figure
    std::atomic<int> mood{0}, shot{-1}, react_mood{-1}, react_len{0};
    std::atomic<bool> react_failed{false};
    std::atomic<float> level{0};
    lv_image_dsc_t fig_dsc[2] = {};
    uint8_t *fig_px[2] = {};
    std::atomic<int> fig_shown{0};
    float levels[BARS] = {};
} U;

void post(std::function<void()> fn)
{
    std::lock_guard<std::mutex> g(U.lock);
    U.posts.push_back(std::move(fn));
}

void set_mood(Mood m)
{
    U.mood = int(m);
}
void shoot(Shot s)
{
    U.shot = int(s);
}

// ---- figure task ------------------------------------------------------------------------
void figure_task(void *)
{
    Figure fig(FIG, 1);
    int64_t last = esp_timer_get_time(), stats = last;
    int frames = 0;
    int64_t busy_us = 0;
    for (;;) {
        if (!U.active) {
            vTaskDelay(pdMS_TO_TICKS(120));
            last = esp_timer_get_time();
            continue;
        }
        const int64_t start = esp_timer_get_time();
        fig.set_mood(Mood(U.mood.load()));
        fig.set_level(U.level);
        const int s = U.shot.exchange(-1);
        if (s >= 0) {
            fig.shoot(Shot(s));
        }
        const int r = U.react_mood.exchange(-1);
        if (r >= 0) {
            fig.react(Shot(r), U.react_failed, U.react_len);
        }
        fig.step(float(start - last) / 1e6f);
        last = start;
        const int back = U.fig_shown ^ 1;
        fig.render_premultiplied(U.fig_px[back]);
        busy_us += esp_timer_get_time() - start;
        ++frames;
        post([back] {
            U.fig_shown = back;
            if (U.figure) {
                lv_image_set_src(U.figure, &U.fig_dsc[back]);
                lv_obj_invalidate(U.figure);
            }
        });
        if (start - stats > 10000000) {
            ESP_LOGI(TAG, "figure: %.1f fps, %.1f ms a frame (%s)", frames * 1e6 / double(start - stats), busy_us / 1000.0 / frames,
                     fig.stats(true).c_str());
            stats = start, frames = 0, busy_us = 0;
        }
        // A frame takes ~40 ms of core 0. 20 fps while the bird does something,
        // 12.5 at rest, and always a real rest between frames (back to back it
        // held core 0 at ~90 %, slowing the speech connection and the agent).
        const int64_t spent = (esp_timer_get_time() - start) / 1000;
        const int64_t period = fig.busy() ? 50 : 80;
        vTaskDelay(pdMS_TO_TICKS(std::max<int64_t>(10, period - spent)));
    }
}

// ---- chat -----------------------------------------------------------------------------------
lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

void scroll_end(bool animate = true)
{
    if (!U.chat) {
        return;
    }
    lv_obj_update_layout(U.chat);
    lv_obj_scroll_to_y(U.chat, LV_COORD_MAX, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

void update_empty()
{
    const bool empty = lv_obj_get_child_count(U.chat) == 0;
    if (empty && U.fade) {
        set_hidden(U.fade, true);
    }
    if (empty) {
        lv_obj_remove_flag(U.empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(U.empty, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *bubble(bool mine)
{
    // The chat keeps the latest bubbles only: every one stays in LVGL's tree
    // (layout, hit tests, its photos in PSRAM), so a long session slowed down.
    // The agent keeps its own, shorter history.
    constexpr uint32_t KEEP = 30;
    while (lv_obj_get_child_count(U.chat) >= KEEP) {
        lv_obj_t *old = lv_obj_get_child(U.chat, 0);
        const auto holds = [old](lv_obj_t *o) {
            for (; o; o = lv_obj_get_parent(o))
                if (o == old) {
                    return true;
                }
            return false;
        };
        if (holds(U.reply) || holds(U.draft)) {
            break;
        }
        lv_obj_delete(old);
    }
    lv_obj_t *row = plain(U.chat);
    lv_obj_set_size(row, CHAT_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *b = plain(row);
    lv_obj_set_size(b, mine ? LV_SIZE_CONTENT : BUBBLE, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, BUBBLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(mine ? BUBBLE_ME : BUBBLE_SWIFTY), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, 8, 0);
    if (!mine) {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(EDGE), 0);
    }
    update_empty();
    // rise in
    lv_obj_set_style_translate_y(row, 14, 0);
    lv_obj_set_style_opa(row, 0, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, row);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(o), lv_opa_t(std::min<int32_t>(255, v)), 0);
        lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(o), 14 - 14 * v / 256, 0);
    });
    lv_anim_start(&a);
    return b;
}

lv_obj_t *text_label(lv_obj_t *parent, const std::string &text, uint32_t color, int px = 16, int max_w = BUBBLE - 28)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text.c_str());
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(l, max_w, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, font(px, false), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// A user message: photos, then the words. Returns the words' label.
lv_obj_t *add_user(const std::string &text, const std::vector<Jpeg> &photos)
{
    lv_obj_t *b = bubble(true);
    for (const Jpeg &p : photos)
        if (Image *img = decode(p->data(), p->size(), 150, 150)) {
            show_image(b, img, 12);
        }
    lv_obj_t *l = text_label(b, text, 0xFFFFFF);
    scroll_end();
    return l;
}

void add_note(const std::string &text)
{
    lv_obj_t *b = bubble(false);
    text_label(b, text, TEXT2, 15);
    scroll_end();
}

// ---- images in the chat ---------------------------------------------------------------
struct Fetch {
    std::string url;
    lv_obj_t *holder;
    int photo;
};

void fetch_task(void *arg)
{
    std::unique_ptr<Fetch> job(static_cast<Fetch *>(arg));
    Image *img = nullptr;
    std::string why;
    if (job->photo >= 0) {
        uint8_t *jpeg = nullptr;
        size_t size = 0;
        if (camera_app_photo_jpeg(job->photo, &jpeg, &size)) {
            img = decode(jpeg, size, BUBBLE - 28, 300);
            heap_caps_free(jpeg);
        }
    } else {
        esp_http_client_config_t cfg = {};
        cfg.url = job->url.c_str();
        cfg.timeout_ms = 12000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.buffer_size = 4096;
        cfg.max_redirection_count = 3;
        cfg.user_agent = "swifty-mosaico";
        esp_http_client_handle_t h = esp_http_client_init(&cfg);
        std::vector<uint8_t> body;
        if (h && esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0) {
            const int status = esp_http_client_get_status_code(h);
            if (status == 200) {
                uint8_t buf[2048];
                int n;
                while ((n = esp_http_client_read(h, reinterpret_cast<char *>(buf), sizeof(buf))) > 0 && body.size() < 900 * 1024) {
                    body.insert(body.end(), buf, buf + n);
                }
            } else {
                why = "HTTP " + std::to_string(status);
            }
        } else {
            why = "unreachable";
        }
        if (h) {
            esp_http_client_cleanup(h);
        }
        if (!body.empty()) {
            img = decode(body.data(), body.size(), BUBBLE - 28, 300);
        }
        if (!img && why.empty()) {
            why = body.empty() ? "empty" : "not a JPEG or PNG";
        }
    }
    lv_obj_t *holder = job->holder;
    post([holder, img, why] {
        if (!lv_obj_is_valid(holder)) {
            if (img) {
                heap_caps_free(img->px);
                delete img;
            }
            return;
        }
        lv_obj_clean(holder);
        if (!img)
        {
            lv_obj_t *l = label(holder, ("Image unavailable (" + why + ")").c_str(), 14, 20, BUBBLE - 56, TEXT3, 14);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            return;
        }
        lv_obj_set_size(holder, int(img->dsc.header.w), int(img->dsc.header.h));
        show_image(holder, img, 14);
        scroll_end();
    });
    vTaskDeleteWithCaps(nullptr);  // its stack is in PSRAM
}

void fetch(const std::string &url, lv_obj_t *holder, int photo = -1)
{
    auto *job = new Fetch{url, holder, photo};
    if (xTaskCreatePinnedToCoreWithCaps(fetch_task, "swifty-img", 16 * 1024, job, 3, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete job;
    }
}

const ImageLoader LOAD_IMAGES = [](const std::string &url, const std::string &, lv_obj_t *holder)
{
    if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0) {
        fetch(url, holder);
    }
};

// ---- the reply -----------------------------------------------------------------------
void begin_reply()
{
    U.reply = bubble(false);
    U.reply_tool = text_label(U.reply, "", TEXT3, 13);
    lv_obj_add_flag(U.reply_tool, LV_OBJ_FLAG_HIDDEN);
    U.reply_body = plain(U.reply);
    lv_obj_set_size(U.reply_body, BUBBLE - 28, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.reply_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.reply_body, 6, 0);
    text_label(U.reply_body, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2", SKY, 18);
    U.reply_done = U.reply_live = nullptr;
    U.reply_done_len = 0;
    U.reply_text.clear();
    U.reply_dirty = false;
    scroll_end();
}

void reply_photo(int index)
{
    if (!U.reply) {
        begin_reply();
    }
    lv_obj_t *holder = plain(U.reply);
    lv_obj_set_size(holder, BUBBLE - 28, 120);
    lv_obj_set_style_bg_color(holder, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(holder, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(holder, 14, 0);
    lv_obj_move_to_index(holder, 1);  // after the tool line, before the words
    fetch("", holder, index);
}

lv_obj_t *reply_column(lv_obj_t *parent)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_size(o, BUBBLE - 28, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, 6, 0);
    return o;
}

// Where the finished paragraphs end, from a paragraph start: just after the
// last blank line that is not inside a code block (npos-free: from if none).
size_t finished_paragraphs(const std::string &t, size_t from)
{
    size_t end = from, line = from;
    bool fence = false;
    while (line < t.size()) {
        const size_t nl = t.find('\n', line);
        if (nl == std::string::npos) {
            break;    // the last line may still be growing
        }
        const std::string l = t.substr(line, nl - line);
        size_t k = 0;
        while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) {
            ++k;
        }
        if (l.compare(k, 3, "```") == 0) {
            fence = !fence;
        } else if (!fence && k == l.size()) {
            end = nl + 1;    // a blank line ends a paragraph
        }
        line = nl + 1;
    }
    return end;
}

void render_reply(bool final)
{
    if (!U.reply_body) {
        return;
    }
    U.reply_dirty = false;
    U.reply_drawn = lv_tick_get();
    const std::string &t = U.reply_text;
    // A streamed reply without images is already laid out but for its tail:
    // finish it in place rather than building it all again.
    const bool images = t.find("![") != std::string::npos || t.find("<img") != std::string::npos;
    if (final && (images || !U.reply_live)) {  // once, in full, with its images
        md_render(U.reply_body, t.empty() ? std::string("\xE2\x80\xA6") : t, BUBBLE - 28, LOAD_IMAGES);
        U.reply_done = U.reply_live = nullptr;
        U.reply_done_len = 0;
        scroll_end();
        return;
    }
    // Streaming: re-rendering the whole reply each update grew with its length
    // and restarted an animated scroll (the chat redrawn every frame meanwhile).
    if (!U.reply_live) {
        lv_obj_clean(U.reply_body);  // the "..." placeholder
        U.reply_done = reply_column(U.reply_body);
        U.reply_live = reply_column(U.reply_body);
        U.reply_done_len = 0;
    }
    const size_t done = finished_paragraphs(t, U.reply_done_len);
    if (done > U.reply_done_len) {
        md_render(reply_column(U.reply_done), t.substr(U.reply_done_len, done - U.reply_done_len), BUBBLE - 28, ImageLoader());
        U.reply_done_len = done;
    }
    const std::string live = t.substr(U.reply_done_len);
    if (live.empty()) {
        lv_obj_clean(U.reply_live);
    } else {
        md_render(U.reply_live, live, BUBBLE - 28, ImageLoader());
    }
    if (final) {
        U.reply_done = U.reply_live = nullptr;
        U.reply_done_len = 0;
    }
    scroll_end(!final ? false : true);
}

void set_pill(const char *text, bool live);

void send(const std::string &text, bool spoken, lv_obj_t *draft)
{
    if (text.empty() || agent_busy()) {
        return;
    }
    std::vector<Jpeg> photos;
    photos.swap(U.attached);
    lv_obj_clean(U.attach_row);
    if (draft) {
        lv_label_set_text(draft, text.c_str());
    } else {
        add_user(text, photos);
    }
    begin_reply();
    shoot(Shot::Send);
    set_mood(Mood::Think);
    U.status = "Thinking";
    set_pill("Stop", false);
    AgentEvents ev;
    const unsigned conversation = U.conversation;
    ev.text = [conversation](const std::string & t) {
        post([t, conversation] {
            if (conversation != U.conversation) return;
            U.reply_text = t;
            U.reply_dirty = true;
            set_mood(Mood::Talk);
        });
    };
    ev.tool = [](const std::string & what) {
        post([what] {
            if (!U.reply_tool) return;
            lv_label_set_text(U.reply_tool, what.c_str());
            lv_obj_remove_flag(U.reply_tool, LV_OBJ_FLAG_HIDDEN);
        });
    };
    ev.show_photo = [](int index) {
        post([index] { reply_photo(index); });
    };
    // The camera app starts on its first use, not with Swifty: its capture task
    // and NAS list hold internal RAM the speech connection's TLS needs.
    ev.camera = [] {  // from the agent's task, one tool at a time
        static SemaphoreHandle_t done = xSemaphoreCreateBinary();
        xSemaphoreTake(done, 0);  // a late give from an earlier call
        post([] {
            if (U.hooks.camera_ready) U.hooks.camera_ready();
            xSemaphoreGive(done);
        });
        xSemaphoreTake(done, pdMS_TO_TICKS(3000));
        for (int i = 0; i < 40 && camera_app_photos_state() == 0; ++i)
        {
            vTaskDelay(pdMS_TO_TICKS(100));    // the NAS list
        }
    };
    ev.done = [conversation](const std::string & t, Shot mood, bool failed) {
        post([t, mood, failed, conversation] {
            if (conversation != U.conversation) return;
            const std::string text = failed ? "*" + t + "*" : t;
            // What was laid out while streaming no longer matches (a failure in
            // italics, or a final text that differs): build it all again.
            if (text.compare(0, U.reply_done_len, U.reply_text, 0, U.reply_done_len) != 0) U.reply_live = nullptr;
            U.reply_text = text;
            render_reply(true);
            if (U.reply_tool) lv_obj_add_flag(U.reply_tool, LV_OBJ_FLAG_HIDDEN);
            U.reply = U.reply_body = U.reply_tool = nullptr;
            set_mood(Mood::Idle);
            U.react_failed = failed;
            U.react_len = int(t.size());
            U.react_mood = int(mood);
            U.last = t;
            U.status = failed ? "Something went wrong" : "Ready";
            set_pill(nullptr, false);
        });
    };
    if (!agent_send(text, photos, spoken, ev)) {
        add_note("Swifty is still answering.");
    }
}

// ---- listening ------------------------------------------------------------------------
void set_pill(const char *text, bool live)
{
    if (!U.pill) {
        return;
    }
    const bool busy = agent_busy();
    if (!text) {
        text = busy ? "Stop" : "Hold to talk";
    }
    lv_label_set_text(U.pill_label, text);
    lv_obj_set_style_bg_color(U.pill, lv_color_hex(live ? SKY_DEEP : FIELD), 0);
    if (live) {
        lv_obj_add_flag(U.pill_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.pill_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.wave, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(U.pill_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.pill_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.wave, LV_OBJ_FLAG_HIDDEN);
    }
}

void start_listening()
{
    if (U.listening || U.recognizing) {
        return;
    }
    if (agent_busy()) {
        agent_stop();
        return;
    }
    if (!config().asr_ready()) {
        add_note("Speech isn't set up yet. Add a speech server in Settings, Web settings.");
        shoot(Shot::Confused);
        return;
    }
    AsrEvents ev;
    ev.partial = [](const std::string & t) {
        post([t] {
            if (U.draft && lv_obj_is_valid(U.draft)) lv_label_set_text(U.draft, t.c_str());
            scroll_end();
        });
    };
    ev.level = [](float l) {
        U.level = l;
    };
    const int64_t t0 = esp_timer_get_time();
    const bool started = asr_start(ev);
    ESP_LOGI(TAG, "listening: microphone open in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    if (!started) {
        const char *owner = board_mic_owner();
        add_note(owner && !strcmp(owner, "webcam") ? "The computer is using the microphone (webcam mode)."
                 : "The microphone isn't available right now.");
        shoot(Shot::Sorry);
        return;
    }
    U.listening = true;
    U.status = "Listening";
    set_mood(Mood::Listen);
    U.draft = add_user("\xE2\x80\xA6", U.attached);
    set_pill(nullptr, true);
}

void stop_listening()
{
    if (!U.listening) {
        return;
    }
    U.listening = false;
    U.recognizing = true;
    U.level = 0;
    set_mood(Mood::Think);
    set_pill("Recognizing\xE2\x80\xA6", false);
    xTaskCreatePinnedToCoreWithCaps(
    [](void *) {
        std::string error;
        const std::string text = asr_finish(&error);
        post([text, error] {
            U.recognizing = false;
            lv_obj_t *draft = U.draft;
            U.draft = nullptr;
            if (text.empty()) {
                if (draft && lv_obj_is_valid(draft)) {
                    lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(draft)));
                }
                update_empty();
                add_note(error.empty() ? "I didn't catch that." : error);
                set_mood(Mood::Idle);
                U.react_failed = true, U.react_len = 0, U.react_mood = int(Shot::Sorry);
                set_pill(nullptr, false);
                return;
            }
            if (!error.empty())
            {
                add_note(error + " Sent what I heard until then.");
            }
            send(text, true, draft && lv_obj_is_valid(draft) ? draft : nullptr);
        });
        vTaskDeleteWithCaps(nullptr);  // its stack is in PSRAM
    },
    "swifty-fin", 6144, nullptr, 4, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void pill_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        start_listening();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        stop_listening();
    }
}

void wave_timer(lv_timer_t *)
{
    if (!U.listening) {
        return;
    }
    for (int i = BARS - 1; i > 0; --i) {
        U.levels[i] = U.levels[i - 1];
    }
    U.levels[0] = U.level;
    for (int i = 0; i < BARS; ++i) {
        const int bar = BAR_ORDER[i];
        const int h = std::max(6, int(BAR_MAX[bar] * std::min(1.f, U.levels[i] * 1.4f + .04f)));
        if (abs(lv_obj_get_height(U.bars[bar]) - h) >= 2) {
            lv_obj_set_height(U.bars[bar], h);
            lv_obj_set_y(U.bars[bar], (48 - h) / 2);
        }
    }
}

// ---- attachments -----------------------------------------------------------------------
void attach(const Jpeg &jpeg)
{
    if (U.attached.size() >= 2) {
        U.attached.erase(U.attached.begin());
    }
    U.attached.push_back(jpeg);
    lv_obj_clean(U.attach_row);
    for (const Jpeg &p : U.attached) {
        Image *img = decode(p->data(), p->size(), 56, 56);
        if (!img) {
            continue;
        }
        lv_obj_t *t = show_image(U.attach_row, img, 12);
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(SKY), 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, [](lv_event_t *) {  // tap a thumbnail to drop all attachments
            U.attached.clear();
            lv_obj_clean(U.attach_row);
        }, LV_EVENT_CLICKED, nullptr);
    }
}

void camera_cb(lv_event_t *e)
{
    static std::atomic<bool> taking{false};
    const bool latest = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    if (!latest && U.hooks.pick_photo) {  // frame it in the real viewfinder
        U.hooks.pick_photo();
        return;
    }
    if (taking.exchange(true)) {
        return;
    }
    if (U.hooks.camera_ready) {
        U.hooks.camera_ready();
    }
    U.status = "Taking a photo";
    shoot(Shot::Send);
    xTaskCreatePinnedToCoreWithCaps(
    [](void *arg) {
        const bool newest = arg != nullptr;
        const int index = newest ? -1 : camera_app_take_photo(15000);
        uint8_t *jpeg = nullptr;
        size_t size = 0;
        Jpeg photo;
        if ((newest || index >= 0) && camera_app_photo_jpeg(newest ? -1 : index, &jpeg, &size)) {
            photo = std::make_shared<std::vector<uint8_t>>(jpeg, jpeg + size);
            heap_caps_free(jpeg);
        }
        post([photo] {
            taking = false;
            U.status = "Ready";
            if (photo) attach(photo);
            else {
                add_note("The camera didn't take a photo. Is the camera module in the left slot?");
            }
        });
        vTaskDeleteWithCaps(nullptr);  // its stack is in PSRAM
    },
    "swifty-cam", 6144, latest ? reinterpret_cast<void *>(1) : nullptr, 4, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// ---- typing ----------------------------------------------------------------------------
void close_entry()
{
    if (U.entry) {
        lv_obj_delete(U.entry);
    }
    U.entry = U.entry_ta = nullptr;
}

const Accent SKY_ACCENT = {SKY_HI, SKY_DEEP, SKY, 0x041428, 0x12305A, 0x060E1C};

void open_entry(lv_event_t *)
{
    if (U.entry) {
        return;
    }
    U.entry = box(U.screen, 0, 0, W, W);
    lv_obj_set_style_bg_color(U.entry, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.entry, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *close = field_button(U.entry, 56, 30, 92, 36, "Cancel", 15, TEXT2);
    lv_obj_add_event_cb(close, [](lv_event_t *) {
        post(close_entry);
    }, LV_EVENT_CLICKED, nullptr);
    label(U.entry, "Ask Swifty", 156, 34, 220, TEXT, 22, true, LV_TEXT_ALIGN_CENTER);
    U.entry_ta = lv_textarea_create(U.entry);
    lv_obj_set_pos(U.entry_ta, 24, 84);
    lv_obj_set_size(U.entry_ta, 432, 136);
    lv_textarea_set_placeholder_text(U.entry_ta, "Message");
    lv_obj_set_style_bg_color(U.entry_ta, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(U.entry_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(U.entry_ta, 0, 0);
    lv_obj_set_style_radius(U.entry_ta, 20, 0);
    lv_obj_set_style_text_color(U.entry_ta, lv_color_hex(TEXT), 0);
    lv_obj_set_style_text_font(U.entry_ta, font(18, false), 0);
    lv_obj_set_style_pad_all(U.entry_ta, 14, 0);
    lv_obj_set_style_bg_color(U.entry_ta, lv_color_hex(SKY), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(U.entry_ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(U.entry_ta, 2, LV_PART_CURSOR);
    lv_obj_set_style_text_color(U.entry_ta, lv_color_hex(TEXT3), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_state(U.entry_ta, LV_STATE_FOCUSED);
    KeyboardConfig kc;
    kc.action = "Send";
    kc.accent = &SKY_ACCENT;
    keyboard(U.entry, U.entry_ta, 0, 236, W, 234, kc);
    // Closing deletes the text area, so not from inside its own event.
    lv_obj_add_event_cb(U.entry_ta, [](lv_event_t *) {
        const std::string text = lv_textarea_get_text(U.entry_ta);
        post([text] {
            close_entry();
            send(text, false, nullptr);
        });
    }, LV_EVENT_READY, nullptr);
}

// ---- build -----------------------------------------------------------------------------
lv_obj_t *round_button(lv_obj_t *parent, int x, int y, int size, const char *glyph)
{
    lv_obj_t *b = box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    const int s = size * 11 / 20;
    icon(b, glyph, (size - s) / 2, (size - s) / 2, s, TEXT, 1.9f);
    return b;
}

void new_chat(lv_event_t *)
{
    if (agent_busy() || U.listening || U.recognizing) {
        return;
    }
    agent_reset();
    lv_obj_clean(U.chat);
    U.attached.clear();
    lv_obj_clean(U.attach_row);
    update_empty();
    shoot(Shot::Hello);
}

void refresh_hint()
{
    const Config c = config();
    lv_label_set_text(U.empty_hint, !c.llm_ready() ? "Set me up first: Settings, Web settings, then open the page it shows on a phone or computer."
                      : agent_developer() ? "Developer mode: ask for code, scripts, configs and longer answers."
                      : c.asr_ready() ? "Hold the mic or the AI key and ask me anything. Tap the camera to show me something."
                      : "Type to me, or tap the camera to show me something.");
}

void build(lv_obj_t *s)
{
    U.screen = s;
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; ++i) {
        U.fig_px[i] = static_cast<uint8_t *>(heap_caps_calloc(1, FIG * FIG * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        lv_image_dsc_t &d = U.fig_dsc[i];
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED;  // render_premultiplied
        d.header.w = FIG;
        d.header.h = FIG;
        d.header.stride = FIG * 4;
        d.data = U.fig_px[i];
        d.data_size = FIG * FIG * 4;
    }
    U.figure = lv_image_create(s);
    lv_obj_set_pos(U.figure, (W - FIG) / 2, -4);
    lv_image_set_src(U.figure, &U.fig_dsc[0]);
    lv_obj_add_flag(U.figure, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.figure, [](lv_event_t *) {
        shoot(Shot::Poke);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *back = round_button(s, 56, 40, 48, "back");
    lv_obj_add_event_cb(back, [](lv_event_t *) {
        if (U.hooks.exit) {
            U.hooks.exit();
        }
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *fresh = round_button(s, 376, 40, 48, "pencil");
    lv_obj_add_event_cb(fresh, new_chat, LV_EVENT_CLICKED, nullptr);

    U.chat = box(s, CHAT_X, CHAT_Y, CHAT_W, CHAT_H);
    lv_obj_add_flag(U.chat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(U.chat, LV_OBJ_FLAG_CLICKABLE);  // a drag on text or a gap scrolls too
    lv_obj_set_scroll_dir(U.chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(U.chat, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(U.chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.chat, 10, 0);
    lv_obj_set_style_pad_bottom(U.chat, 8, 0);
    // The chat's top edge fades out while there is more above, instead of
    // cutting a line of text in half. Black to clear, over the chat.
    U.fade = box(s, CHAT_X, CHAT_Y, CHAT_W, 18);
    lv_obj_set_style_bg_color(U.fade, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(U.fade, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(U.fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(U.fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(U.fade, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(U.fade, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.fade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(U.chat, [](lv_event_t *) {
        set_hidden(U.fade, lv_obj_get_scroll_top(U.chat) <= 0);
    }, LV_EVENT_SCROLL, nullptr);

    U.empty = box(s, CHAT_X, CHAT_Y + 6, CHAT_W, CHAT_H - 6);
    label(U.empty, "Hi, I'm Swifty", 0, 4, CHAT_W, TEXT, 26, true, LV_TEXT_ALIGN_CENTER);
    U.empty_hint = label(U.empty, "", 40, 44, CHAT_W - 80, TEXT2, 15, false, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(U.empty_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(U.empty_hint, 60);
    static const char *const CHIPS[] = {"What do you see?", "How's the home?", "Tell me a fun fact"};
    for (int i = 0; i < 3; ++i) {
        const int w = text_width(CHIPS[i], 15) + 36;
        lv_obj_t *chip = field_button(U.empty, (CHAT_W - w) / 2, 112 + i * 50, w, 40, CHIPS[i], 15, SKY_HI);
        lv_obj_set_style_radius(chip, 20, 0);
        lv_obj_add_event_cb(chip, [](lv_event_t *e) {
            send(static_cast<const char *>(lv_event_get_user_data(e)), false, nullptr);
        }, LV_EVENT_CLICKED, const_cast<char *>(CHIPS[i]));
    }

    U.attach_row = box(s, 92, 336, 300, 60);
    lv_obj_set_flex_flow(U.attach_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(U.attach_row, 8, 0);
    lv_obj_add_flag(U.attach_row, LV_OBJ_FLAG_FLOATING);

    // bottom row, between the lower corners
    U.camera_btn = round_button(s, 84, 404, 56, "camera");
    lv_obj_add_event_cb(U.camera_btn, camera_cb, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(U.camera_btn, camera_cb, LV_EVENT_LONG_PRESSED, nullptr);
    U.pill = box(s, 150, 404, 180, 56, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(U.pill, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(U.pill, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.pill, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    lv_obj_add_event_cb(U.pill, pill_cb, LV_EVENT_ALL, nullptr);
    U.pill_icon = icon(U.pill, "mic", 22, 15, 26, SKY_HI, 2.0f);
    U.pill_label = label(U.pill, "Hold to talk", 54, 17, 116, TEXT, 16, true);
    U.wave = box(U.pill, 8, 4, 164, 48);
    for (int i = 0; i < BARS; ++i) {
        // the centre bar brightest, as ESP-Claw's waveform (in Swifty's blues)
        const float k = 1 - fabsf(i - 7) / 7.f;
        const lv_color_t c = lv_color_mix(lv_color_hex(SKY_HI), lv_color_hex(0x1B38B8), lv_opa_t(255 * k));
        U.bars[i] = box(U.wave, 4 + i * 11, 21, 6, 6, 3);
        lv_obj_set_style_bg_color(U.bars[i], c, 0);
        lv_obj_set_style_bg_opa(U.bars[i], LV_OPA_COVER, 0);
    }
    lv_obj_add_flag(U.wave, LV_OBJ_FLAG_HIDDEN);
    U.keys_btn = round_button(s, 340, 404, 56, "pencil");
    lv_obj_add_event_cb(U.keys_btn, open_entry, LV_EVENT_CLICKED, nullptr);

    refresh_hint();
    update_empty();
    lv_timer_create(wave_timer, 66, nullptr);
    lv_timer_create([](lv_timer_t *) {
        std::vector<std::function<void()>> todo;
        {
            std::lock_guard<std::mutex> g(U.lock);
            todo.swap(U.posts);
        }
        for (auto &fn : todo) {
            fn();
        }
        if (U.reply_dirty && lv_tick_elaps(U.reply_drawn) > 140) {
            render_reply(false);
        }
        // While Swifty is open, keep a speech connection ready for the next press
        // (asr_warm alone lapses 3 minutes after Swifty opened).
        static uint32_t warmed = 0;
        if (U.active && lv_tick_elaps(warmed) > 20000) {
            asr_warm(true);
            warmed = lv_tick_get();
        }
    }, 30, nullptr);
    // The bird takes the CPU that is left: below LVGL's draw threads (3) and
    // the camera (4), which it starved at or above them (the display lock then
    // stayed taken for seconds), and below the speech connection and network
    // (2), whose TLS handshakes it slowed sharing their priority. Core 0, as
    // core 1 carries LVGL.
    xTaskCreatePinnedToCore(figure_task, "swifty-fig", 6144, nullptr, 1, nullptr, 0);
}
}  // namespace

// Test hook: service 0x4744 method 41 sends its payload to Swifty as if typed
// (Swifty must have been opened once). Replies "sent" or "busy".
esp_err_t say_rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    const std::string text(reinterpret_cast<const char *>(request->payload), request->payload_size);
    const char *reply = !U.built ? "closed" : agent_busy() || text.empty() ? "busy" : "sent";
    if (!strcmp(reply, "sent")) post([text] { send(text, false, nullptr); });
    *size = std::min(capacity, strlen(reply));
    memcpy(out, reply, *size);
    return ESP_OK;
}

extern "C" void swifty_app_init(const swifty_hooks_t *hooks)
{
    if (hooks) {
        U.hooks = *hooks;
    }
    config_start();
    agent_start();
    swifty_paseo_start();
    esp_iris_rpc_register(0x4744, 41, say_rpc, nullptr);
    // Test hook: method 42, payload 1 starts listening and 0 stops, as the mic pill.
    esp_iris_rpc_register(0x4744, 42, [](const esp_iris_rpc_request_t *request, uint8_t *, size_t, size_t *size, void *) {
        *size = 0;
        if (!U.built || request->payload_size < 1) {
            return ESP_ERR_INVALID_STATE;
        }
        const bool on = request->payload[0];
        post([on] { on ? start_listening() : stop_listening(); });
        return ESP_OK;
    }, nullptr);
}

extern "C" void swifty_app_new_conversation(void)
{
    if (agent_busy()) {
        agent_stop();
    }
    ++U.conversation;
    agent_reset();
    U.last.clear();
    if (!U.built) {
        return;
    }
    // The reply under way goes with the chat.
    U.reply = U.reply_body = U.reply_tool = U.reply_done = U.reply_live = U.draft = nullptr;
    U.reply_text.clear();
    U.reply_dirty = false;
    lv_obj_clean(U.chat);
    U.attached.clear();
    lv_obj_clean(U.attach_row);
    U.status = "Ready";
    set_pill(nullptr, false);
    update_empty();
    refresh_hint();
}

extern "C" void swifty_app_open(lv_obj_t *screen)
{
    if (!U.built) {
        U.built = true;
        build(screen);
        shoot(Shot::Hello);
    } else {
        refresh_hint();
    }
    swifty_app_set_active(true);
}

extern "C" void swifty_app_config_section(const char *name, swifty_section_apply_t apply, swifty_section_status_t status)
{
    config_section(name, apply, status);
}

extern "C" char *swifty_app_config_status(void)
{
    const std::string json = config_status_json();
    return strdup(json.c_str());
}

extern "C" void swifty_app_config_apply(char *json, size_t length, char *reply, size_t capacity)
{
    std::string text(json, length);
    for (size_t i = 0; i < length; ++i) {
        reinterpret_cast<volatile char *>(json)[i] = 0;
    }
    const std::string r = config_apply_json(text);
    strlcpy(reply, r.c_str(), capacity);
}

extern "C" void swifty_app_attach_photo(int index)
{
    if (!U.built) {
        return;
    }
    uint8_t *jpeg = nullptr;
    size_t size = 0;
    if (!camera_app_photo_jpeg(index, &jpeg, &size)) {
        return;
    }
    attach(std::make_shared<std::vector<uint8_t>>(jpeg, jpeg + size));
    heap_caps_free(jpeg);
    shoot(Shot::Pleased);
}

extern "C" void swifty_app_set_active(bool active)
{
    U.active = active && U.built;
    // A speech connection ready for the next press. Leaving Swifty does not drop
    // it: the shell keeps one while the board is in use (swifty_app_prewarm).
    if (U.active) {
        asr_warm(true);
    }
    if (!U.active && U.listening) {
        asr_cancel();
        U.listening = false;
        post([] {
            if (U.draft && lv_obj_is_valid(U.draft)) lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(U.draft)));
            U.draft = nullptr;
            update_empty();
            set_mood(Mood::Idle);
            set_pill(nullptr, false);
        });
    }
}

extern "C" void swifty_app_prewarm(bool on)
{
    if (on) {
        asr_warm(true);
    } else if (!U.active) {
        asr_warm(false);
    }
}

extern "C" void swifty_app_talk(bool pressed)
{
    if (!U.built) {
        return;
    }
    if (pressed) {
        start_listening();
    } else {
        stop_listening();
    }
}

extern "C" void swifty_app_summary(swifty_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    const Config c = config();
    out->configured = c.llm_ready();
    out->busy = agent_busy();
    out->listening = U.listening;
    strlcpy(out->status, !out->configured ? "Not set up" : U.listening ? "Listening" : out->busy ? "Thinking" : U.status.c_str(),
            sizeof(out->status));
    strlcpy(out->last, U.last.c_str(), sizeof(out->last));
}

extern "C" const lv_image_dsc_t *swifty_app_portrait(int size)
{
    static lv_image_dsc_t dsc = {};
    if (dsc.data) {
        return &dsc;
    }
    auto *px = static_cast<uint8_t *>(heap_caps_calloc(1, size_t(size) * size * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!px) {
        return nullptr;
    }
    Figure fig(size, 2);
    for (int i = 0; i < 90; ++i) {
        fig.step(1 / 30.f);    // settled
    }
    fig.render(px);
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc.header.w = uint32_t(size);
    dsc.header.h = uint32_t(size);
    dsc.header.stride = uint32_t(size * 4);
    dsc.data_size = uint32_t(size * size * 4);
    dsc.data = px;
    return &dsc;
}
