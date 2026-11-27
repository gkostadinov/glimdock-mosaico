// SPDX-License-Identifier: Apache-2.0
// Games: the picker, each game's title screen (the game playing itself behind
// it), the HUD, pause and game-over cards, and best scores in NVS.
//
// The games draw a 240 x 240 frame into one of three buffers in a task of
// their own (below LVGL's draw threads, stack in PSRAM): one shown, one
// waiting to be, one being drawn, so the task never waits for the screen; an
// LVGL timer moves the waiting frame to the screen. The frame's object doubles it
// straight into LVGL's frame buffer while LVGL draws (scanlines, shading for
// the words over it, fades), and covers the screen so nothing under it is
// drawn: PSRAM bandwidth is what limits the frame rate, and a 480 x 480 image
// in between cost a whole extra write and copy per frame.
//
// Input: a finger dragged from where it went down steers; a quick tap (or a
// second finger) is the game's action. With Tilt on, the board's tilt steers
// (levelled to however it is held when a game starts or resumes), turning the
// board turns Ember Crypt's view (gyroscope), and a shake is the action. The
// Interaction sub-board's buttons are the action too, and its lights flash.
#include "games_app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

#include "esp_log.h"
#include "games_common.h"
#include "mui.h"
#include "nvs.h"
#include "src/core/lv_obj_event_private.h"  // the cover check: the frame hides what is under it
#include "src/misc/lv_area_private.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#else
#include <chrono>
#endif

namespace {
using namespace mui;
using games::H;
using games::W;
const char *TAG = "games";

// ---- the three games: names, words and colours -------------------------------------------------
struct Info {
    const char *name, *tagline, *touch_hint, *tilt_hint, *flipped_hint;
    Accent accent;
};
const Info INFO[games::GAME_COUNT] = {
    {
        "Ember Crypt", "Feed the ember, gather the shards, wake the gate.", "Drag to walk and turn  ·  tap for a flare",
        "Tilt back to walk, to the side to turn, or turn the board to look  ·  shake for a flare",
        "Tilt away to walk, to the side to turn, or turn the board to look  ·  shake for a flare",
        {0xFFD08A, 0xE0662A, 0xFF9A3C, 0x2A1004, 0x3A2010, 0x1A0C06}
    },
    {
        "Starbore", "Thread the gates of a neon bore, faster every sector.", "Drag to steer  ·  tap to pulse a gate away",
        "Tilt to place the ship: back for up, away for down  ·  shake to pulse a gate away",
        "Tilt to place the ship: away for up, back for down  ·  shake to pulse a gate away",
        {0x9AF4FF, 0xB43CFF, 0x46E6FF, 0x12062A, 0x2A1450, 0x10082A}
    },
    {
        "Skyglide", "Ride the thermals through the golden hoops.", "Drag to bank and pitch  ·  tap for a gust",
        "Tilt to fly: back climbs, away dives, to the side banks  ·  shake for a gust",
        "Tilt to fly: away climbs, back dives, to the side banks  ·  shake for a gust",
        {0xFFE9A8, 0x3A8CFF, 0x8FD8FF, 0x061428, 0x1E3A5A, 0x0A1828}
    },
    {
        "Jellystorm", "Hold back the glowing swarm, wave after wave.", "Drag to move  ·  it fires by itself  ·  tap for a pulse bomb",
        "Tilt to move  ·  it fires by itself  ·  shake for a pulse bomb",
        "Tilt to move  ·  it fires by itself  ·  shake for a pulse bomb",
        {0xFFB0E8, 0xE0409A, 0xFF6EC7, 0x2A0618, 0x3A1430, 0x1A0812}
    },
    {
        "Lunar Flip", "A moonbase pinball table: light the lanes, drop the targets.",
        "Hold the left or right half for the flippers  ·  tap to launch",
        "Hold the left or right half for the flippers  ·  tap to launch  ·  shake to nudge",
        "Hold the left or right half for the flippers  ·  tap to launch  ·  shake to nudge",
        {0xD0E4FF, 0x5A7CFF, 0x9AB8FF, 0x0A1430, 0x1E2A50, 0x0C1228}
    },
    {
        "Ridge Rally", "Gravel, pines and hairpins, against the clock.", "Drag to steer, up to speed up, down to brake  ·  tap for a boost",
        "Tilt to steer, back to speed up, away to brake  ·  shake for a boost",
        "Tilt to steer, away to speed up, back to brake  ·  shake for a boost",
        {0xFFC48A, 0xE8501E, 0xFF8040, 0x2A0E04, 0x3A1A0C, 0x1A0A04}
    },
};
constexpr int THUMB = 104, FRAME_MS = 33;
#ifndef ESP_PLATFORM
bool preview_autoplay = false;  // the host preview: played games steer themselves
#endif

int64_t now_us()
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

void *big_alloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, bytes);
#endif
}

// ---- what the UI and the game task share -------------------------------------------------------
struct Shared {
    std::mutex m;
    // from the UI
    int game = 0;
    bool demo = true, run = false, tilt = true, flip = false, thumbs_wanted = false;
    uint32_t reset_seq = 0, level_seq = 0;
    int64_t hold_until = 0;  // getting ready: the game waits, and the tilt levels to the board as held
    bool down = false;  // a finger steers
    bool hold_left = false, hold_right = false;  // fingers on the screen's halves (flippers)
    float pad_x = 0, pad_y = 0;
    int taps = 0;
    // from the game task, for the run reset_seq started
    games::Hud hud;
    uint32_t result_seq = 0;
    bool over = false;
    const char *ending = "";
    uint32_t events = 0;
    bool thumbs_ready = false;
};

struct Task {
    Shared s;
    std::atomic<bool> active{false};
    std::atomic<int> pending{-1}, shown{0};
    uint16_t *frame[3] = {}, *thumbs[games::GAME_COUNT] = {};
    std::atomic<bool> transposed[3] = {};  // each frame's layout (games_common.h)
    lv_image_dsc_t thumb_dsc[games::GAME_COUNT] = {};
    std::unique_ptr<games::Game> games[games::GAME_COUNT];
    // the task's own
    int game = -1;
    uint32_t reset_seq = 0, level_seq = 0;
    bool thumbs_done = false;
    float pitch0 = 0, roll0 = 0, yaw_last = 0, shake_wait = 0;
    bool level_now = true, buttons_last = false;
    int64_t last_us = 0, stats_us = 0, step_us = 0, render_us = 0;
    int frames = 0;
#ifdef ESP_PLATFORM
    TaskHandle_t handle = nullptr;
#endif
} T;

enum class Mode { Picker, Title, Play, Paused, Over };

struct {
    games_hooks_t hooks{};
    bool built = false, sound = true, tilt = true, flip = false;
    Mode mode = Mode::Picker;
    int game = 0;
    uint32_t best[games::GAME_COUNT] = {};
    lv_obj_t *screen = nullptr, *frame = nullptr;
    int dim_top = 0, dim_bottom = H, level = 32, fade = 32;  // how the frame is shown
    // picker
    lv_obj_t *picker = nullptr, *thumb[games::GAME_COUNT] = {}, *card_best[games::GAME_COUNT] = {};
    // title
    lv_obj_t *title = nullptr, *t_name = nullptr, *t_tag = nullptr, *t_best = nullptr, *t_hint = nullptr, *t_play = nullptr;
    lv_obj_t *t_tilt = nullptr, *t_flip = nullptr, *t_sound = nullptr, *t_dots[games::GAME_COUNT] = {};
    lv_obj_t *h_ready = nullptr, *h_count = nullptr;  // getting ready
    int64_t ready_until = 0;
    // hud
    lv_obj_t *hud = nullptr, *h_score = nullptr, *h_line = nullptr, *h_fill = nullptr, *h_name = nullptr;
    // cards
    lv_obj_t *paused = nullptr, *over = nullptr, *o_title = nullptr, *o_reason = nullptr, *o_score = nullptr, *o_best = nullptr;
    lv_obj_t *resume = nullptr, *again = nullptr;
    lv_timer_t *timer = nullptr;
    int64_t press_at = 0;
    lv_point_t press{};
    int shown_score = -1, shown_fill = -1;
    uint32_t shown_color = 0;
    char shown_line[64] = "";
    bool thumbs_shown = false;
} G;

// ---- best scores and settings (NVS, on the LVGL task) ------------------------------------------
void load_settings()
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char key[8];
    for (int i = 0; i < games::GAME_COUNT; ++i) {
        snprintf(key, sizeof(key), "best%d", i);
        uint32_t v = 0;
        if (nvs_get_u32(h, key, &v) == ESP_OK) {
            G.best[i] = v;
        }
    }
    uint8_t v;
    if (nvs_get_u8(h, "sound", &v) == ESP_OK) {
        G.sound = v;
    }
    if (nvs_get_u8(h, "flip", &v) == ESP_OK) {
        G.flip = v;
    }
    if (nvs_get_u8(h, "tilt", &v) == ESP_OK) {
        G.tilt = v;
    }
    nvs_close(h);
}

void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void save_best(int game)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) == ESP_OK) {
        char key[8];
        snprintf(key, sizeof(key), "best%d", game);
        nvs_set_u32(h, key, G.best[game]);
        nvs_commit(h);
        nvs_close(h);
    }
}

// 12,340
void grouped(char *out, size_t cap, uint32_t v)
{
    char digits[16];
    snprintf(digits, sizeof(digits), "%u", unsigned(v));
    const int n = int(strlen(digits));
    size_t o = 0;
    for (int i = 0; i < n && o + 2 < cap; ++i) {
        if (i && (n - i) % 3 == 0) {
            out[o++] = ',';
        }
        out[o++] = digits[i];
    }
    out[o] = 0;
}

// ---- the game task -----------------------------------------------------------------------------
games::Game &game_at(int i)
{
    if (!T.games[i]) {
        T.games[i] = games::make_game(i);
    }
    return *T.games[i];
}

float dead_zone(float v)
{
    const float a = fabsf(v);
    if (a < 0.03f) {
        return 0;
    }
    return std::min(1.0f, (a - 0.03f) / 0.97f) * (v < 0 ? -1.0f : 1.0f);
}

// The board as a controller: a finger, else the tilt (levelled where the board
// was held when the game started or resumed), the gyroscope's turn, a shake,
// a tap or a second finger, the sub-board's buttons.
// The board's top tilted back towards the player is up (forward in the
// maze), in every game; flip: the player's Flip, the other way round.
games::Input read_input(float dt, bool tilt, bool down, float pad_x, float pad_y, int taps, bool flip, bool hold_left,
                        bool hold_right)
{
    games::Input in;
    in.left = hold_left, in.right = hold_right;
    games_motion_t m{};
    if (tilt && G.hooks.motion && G.hooks.motion(&m)) {
        const float *g = m.gravity;  // the way down: x right, y down, z into the glass
        // pitch: 0 lying flat, 90 degrees standing up, on past that, so a
        // board held upright reads both ways; roll: like a steering wheel
        const float pitch = atan2f(g[1], g[2]);
        const float roll = atan2f(g[0], sqrtf(g[1] * g[1] + g[2] * g[2]));
        if (T.level_now) {
            T.pitch0 = pitch, T.roll0 = roll, T.yaw_last = m.yaw;
            T.level_now = false;
        }
        // straight in proportion: ~29 degrees is full steering, past a dead
        // zone of under a degree (a curve here felt dead, then too fast)
        constexpr float FULL = 0.5f;
        in.x = dead_zone((roll - T.roll0) / FULL);
        in.y = dead_zone(games::angle_diff(T.pitch0, pitch) / FULL * (flip ? -1.0f : 1.0f));
        in.absolute = true;
        if (m.gyro) {
            in.turn = m.yaw - T.yaw_last;
        }
        T.yaw_last = m.yaw;
        const float mag = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        T.shake_wait = std::max(0.0f, T.shake_wait - dt);
        if (fabsf(mag - 1) > 0.7f && T.shake_wait <= 0) {
            in.action = in.shake = true;
            T.shake_wait = 0.8f;
        }
    }
    if (down) {  // the finger wins over the tilt
        in.x = pad_x, in.y = pad_y;
        in.absolute = false;
    }
    if (taps > 0) {
        in.action = true;
    }
    bool left = false, right = false;
    if (G.hooks.board_buttons && G.hooks.board_buttons(&left, &right)) {
        in.left = in.left || left, in.right = in.right || right;  // the module's buttons are flippers too
        const bool pressed = left || right;
        if (pressed && !T.buttons_last) {
            in.action = true;
        }
        T.buttons_last = pressed;
    }
    return in;
}

void make_thumbnails()
{
    for (int i = 0; i < games::GAME_COUNT; ++i) {
        games::Game &g = game_at(i);
        g.reset(7 + uint32_t(i), true);
        for (int k = 0; k < (i == 0 ? 50 : 80); ++k) {
            g.step(1.0f / 30, {});
            g.take_events();
        }
        uint16_t *scratch = T.frame[(T.shown.load() + 1) % 3];  // the picker shows no frame
        g.render(scratch);
        games::thumbnail(scratch, g.transposed(), T.thumbs[i], THUMB);
    }
    T.game = -1;  // whatever runs next starts afresh
    T.thumbs_done = true;
    std::lock_guard<std::mutex> l(T.s.m);
    T.s.thumbs_ready = true;
}

// One turn of the task: step, draw, hand the frame over. False: nothing to do.
bool frame_once()
{
    int game, taps;
    bool demo, run, tilt, flip, down, thumbs_wanted, hold_left, hold_right;
    int64_t hold_until;
    float pad_x, pad_y;
    uint32_t reset_seq, level_seq;
    {
        std::lock_guard<std::mutex> l(T.s.m);
        game = T.s.game, demo = T.s.demo, run = T.s.run, tilt = T.s.tilt, flip = T.s.flip;
        hold_until = T.s.hold_until;
        hold_left = T.s.hold_left, hold_right = T.s.hold_right;
        reset_seq = T.s.reset_seq, level_seq = T.s.level_seq;
        down = T.s.down, pad_x = T.s.pad_x, pad_y = T.s.pad_y, taps = T.s.taps;
        T.s.taps = 0;
        thumbs_wanted = T.s.thumbs_wanted;
    }
    if (thumbs_wanted && !T.thumbs_done) {
        make_thumbnails();
    }
    const int64_t t0 = now_us();
#ifdef ESP_PLATFORM
    const float dt = T.last_us ? std::min(0.1f, float(t0 - T.last_us) / 1e6f) : 1.0f / 30;
#else
    const float dt = 1.0f / 30;  // the preview's frames are UI time, not real time
    demo = demo || preview_autoplay;
#endif
    T.last_us = t0;
    games::Game &g = game_at(game);
    if (game != T.game || reset_seq != T.reset_seq) {
        T.game = game;
        T.reset_seq = reset_seq;
        g.reset(uint32_t(t0) ^ 0x5A5A1234u, demo);
        g.take_events();
        T.level_now = true;
    }
    if (level_seq != T.level_seq) {
        T.level_seq = level_seq;
        T.level_now = true;
    }
    if (!run) {
        T.last_us = 0;  // paused: the next step after it is an ordinary one
        return false;
    }
    // getting ready: nothing moves, and the tilt levels to the board as held
    const bool holding = !demo && t0 < hold_until;
    if (holding) {
        T.level_now = true;
    }
    const bool live = !demo && !holding;
    const games::Input in = read_input(dt, tilt && !demo, down && !demo, pad_x, pad_y, live ? taps : 0, flip, live && hold_left,
                                       live && hold_right);
    if (!holding) {
        g.step(dt, in);
    }
    games::Hud hud;
    g.hud(&hud);
    const uint32_t ev = g.take_events();
    {
        std::lock_guard<std::mutex> l(T.s.m);
        if (T.s.reset_seq != reset_seq) {
            return false;  // a new run was asked for meanwhile: this one is stale
        }
        T.s.hud = hud;
        T.s.result_seq = reset_seq;
        T.s.events |= ev;
        T.s.over = g.over();
        T.s.ending = g.ending();
    }
    // draw into the frame neither shown nor waiting to be shown. The waiting
    // one is read first: the UI only ever moves it to shown (tick()).
    const int64_t t1 = now_us();
    const int waiting = T.pending.load(), shown = T.shown.load();
    int back = 0;
    while (back == shown || back == waiting) {
        ++back;
    }
    g.render(T.frame[back]);
    T.transposed[back] = g.transposed();
    T.step_us += t1 - t0;
    T.render_us += now_us() - t1;
    T.pending = back;  // the newest wins; one never shown is simply dropped
    ++T.frames;
    return true;
}

#ifdef ESP_PLATFORM
void game_task(void *)
{
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        if (!T.active.load()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
            wake = xTaskGetTickCount();
            T.last_us = 0;
            continue;
        }
        const bool drew = frame_once();
        const int64_t now = now_us();
        if (!T.stats_us) {
            T.stats_us = now;
        }
        if (now - T.stats_us > 10000000) {
            if (T.frames) {
                char sections[96];
                game_at(T.game).profile(sections, sizeof(sections));
                ESP_LOGI(TAG, "game %d: %.1f fps; a frame: step %.1f ms, draw %.1f ms (%s)", T.game,
                         T.frames * 1e6 / double(now - T.stats_us), T.step_us / 1000.0 / T.frames, T.render_us / 1000.0 / T.frames,
                         sections);
            }
            T.stats_us = now, T.frames = 0, T.step_us = T.render_us = 0;
        }
        if (!drew) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  // paused: wait for a change
            wake = xTaskGetTickCount();
            continue;
        }
        // ~30 fps, and always a rest: lower-priority work keeps running
        if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS)) == pdFALSE) {  // late: no sleep happened
            wake = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}
#endif

void wake_task()
{
#ifdef ESP_PLATFORM
    if (T.handle) {
        xTaskNotifyGive(T.handle);
    }
#endif
}

// ---- telling the task what to do (LVGL task) ---------------------------------------------------
// What the task runs, and how the frame is shown: rows of the frame above
// dim_top and from dim_bottom are shaded for the words over them, and the
// whole is scaled by level (0..32). A reset starts a new run, faded in.
void command(int game, bool demo, bool run, bool reset, int dim_top, int dim_bottom, int level)
{
    {
        std::lock_guard<std::mutex> l(T.s.m);
        T.s.game = game, T.s.demo = demo, T.s.run = run;
        T.s.tilt = G.tilt;
        T.s.flip = G.flip;
        if (reset) {
            ++T.s.reset_seq;
            T.s.over = false;
            T.s.events = 0;
        }
        T.s.down = false;
        T.s.taps = 0;
    }
    if (reset) {
        G.fade = 0;
    }
    G.dim_top = dim_top, G.dim_bottom = dim_bottom, G.level = level;
    if (G.frame) {
        lv_obj_invalidate(G.frame);
    }
    wake_task();
}

// ---- the frame on the screen (LVGL task, while LVGL draws) ---------------------------------------
// The frame covers whatever is under it, so LVGL draws nothing there.
void frame_cover_cb(lv_event_t *e)
{
    auto *info = static_cast<lv_cover_check_info_t *>(lv_event_get_param(e));
    lv_area_t c;
    lv_obj_get_coords(G.frame, &c);
    if (info->res != LV_COVER_RES_MASKED && lv_area_is_in(info->area, &c, 0)) {
        info->res = LV_COVER_RES_COVER;
    }
}

// The frame doubled into LVGL's buffer: every second line a little darker
// (scanlines), shaded towards black where words go, faded by level.
void frame_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_buf_t *buf = layer ? layer->draw_buf : nullptr;
    if (!buf || !T.frame[0] || buf->header.cf != LV_COLOR_FORMAT_RGB565) {
        return;
    }
    lv_area_t obj, clip;
    lv_obj_get_coords(G.frame, &obj);
    if (!lv_area_intersect(&clip, &obj, &layer->_clip_area) || !lv_area_intersect(&clip, &clip, &layer->buf_area)) {
        return;
    }
    const int shown = T.shown.load();
    const uint16_t *frame = T.frame[shown];
    const bool transposed = T.transposed[shown];
    const int level = G.level * G.fade / 32;
    for (int y = clip.y1; y <= clip.y2; ++y) {
        const int fy = y - obj.y1, row = fy >> 1;
        int k = level;
        if (row < G.dim_top) {
            const float t = float(row) / float(G.dim_top);
            k = int(float(level) * (0.22f + 0.78f * t * t * t));
        } else if (row >= G.dim_bottom) {
            const float t = float(H - 1 - row) / float(std::max(1, H - G.dim_bottom));
            k = int(float(level) * (0.3f + 0.7f * t * t));
        }
        if (fy & 1) {
            k = k * 26 / 32;
        }
        uint16_t *dst = reinterpret_cast<uint16_t *>(buf->data + size_t(y - layer->buf_area.y1) * buf->header.stride) +
                        (clip.x1 - layer->buf_area.x1);
        int fx = clip.x1 - obj.x1, n = clip.x2 - clip.x1 + 1;
        const auto px = [k](uint16_t c) {
            return k >= 32 ? c : games::scale565(c, k);
        };
        if (transposed) {  // the frame's row runs across its columns: every H-th pixel
            const uint16_t *col = frame + row;
            if (fx & 1) {
                *dst++ = px(col[(fx >> 1) * H]);
                ++fx, --n;
            }
            const uint16_t *s = col + (fx >> 1) * H;
            const int pairs = n >> 1;
            if ((reinterpret_cast<uintptr_t>(dst) & 3) == 0) {
                uint32_t *d = reinterpret_cast<uint32_t *>(dst);
                for (int i = 0; i < pairs; ++i) {
                    d[i] = uint32_t(px(s[i * H])) * 0x10001u;
                }
            } else {
                for (int i = 0; i < pairs; ++i) {
                    dst[2 * i] = dst[2 * i + 1] = px(s[i * H]);
                }
            }
            if (n & 1) {
                dst[2 * pairs] = px(s[pairs * H]);
            }
            continue;
        }
        const uint16_t *src = frame + row * W;
        if (fx & 1) {  // an odd first column: the second half of a doubled pixel
            *dst++ = px(src[fx >> 1]);
            ++fx, --n;
        }
        if ((reinterpret_cast<uintptr_t>(dst) & 3) == 0) {
            uint32_t *d = reinterpret_cast<uint32_t *>(dst);
            const uint16_t *s = src + (fx >> 1);
            const int pairs = n >> 1;
            if (k >= 32) {
                for (int i = 0; i < pairs; ++i) {
                    d[i] = uint32_t(s[i]) * 0x10001u;
                }
            } else {
                for (int i = 0; i < pairs; ++i) {
                    d[i] = uint32_t(games::scale565(s[i], k)) * 0x10001u;
                }
            }
            if (n & 1) {
                dst[2 * pairs] = px(s[pairs]);
            }
        } else {
            for (int i = 0; i < n; ++i) {
                dst[i] = px(src[(fx + i) >> 1]);
            }
        }
    }
}

void relevel()
{
    std::lock_guard<std::mutex> l(T.s.m);
    ++T.s.level_seq;
}

// ---- small UI pieces ---------------------------------------------------------------------------
lv_obj_t *layer(lv_obj_t *parent, bool opaque)
{
    lv_obj_t *o = box(parent, 0, 0, PANEL, PANEL);
    if (opaque) {
        lv_obj_set_style_bg_color(o, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    }
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

lv_obj_t *round_button(lv_obj_t *parent, int x, int y, int size, const char *glyph)
{
    lv_obj_t *b = box(parent, x, y, size, size, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    const int s = size * 11 / 20;
    icon(b, glyph, (size - s) / 2, (size - s) / 2, s, TEXT, 1.9f);
    return b;
}

void on_click(lv_obj_t *o, lv_event_cb_t cb, intptr_t data = 0)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(data));
}

void show(Mode mode);

void fill_title()
{
    const Info &info = INFO[G.game];
    set_text(G.t_name, info.name);
    set_text_color(G.t_name, lv_color_hex(info.accent.hi));
    set_text(G.t_tag, info.tagline);
    char best[40], n[16];
    grouped(n, sizeof(n), G.best[G.game]);
    if (G.best[G.game]) {
        snprintf(best, sizeof(best), "Best  %s", n);
    } else {
        snprintf(best, sizeof(best), "No best score yet");
    }
    set_text(G.t_best, best);
    set_text(G.t_hint, !G.tilt ? info.touch_hint : G.flip ? info.flipped_hint : info.tilt_hint);
    style_fill(G.t_play, info.accent, true);
    for (int i = 0; i < games::GAME_COUNT; ++i) {
        set_bg_color(G.t_dots[i], lv_color_hex(i == G.game ? info.accent.hi : 0x505058));
    }
}

void fill_picker()
{
    for (int i = 0; i < games::GAME_COUNT; ++i) {
        char best[40], n[16];
        grouped(n, sizeof(n), G.best[i]);
        if (G.best[i]) {
            snprintf(best, sizeof(best), "Best  %s", n);
        } else {
            snprintf(best, sizeof(best), "New");
        }
        set_text(G.card_best[i], best);
    }
}

// ---- events ------------------------------------------------------------------------------------
void back_cb(lv_event_t *)
{
    if (G.mode == Mode::Title) {
        show(Mode::Picker);
    } else if (G.hooks.exit) {
        G.hooks.exit();
    }
}

void card_cb(lv_event_t *e)
{
    G.game = int(intptr_t(lv_event_get_user_data(e)));
    show(Mode::Title);
}

void play_cb(lv_event_t *)
{
    show(Mode::Play);
}

void pause_cb(lv_event_t *)
{
    if (G.mode == Mode::Play) {
        show(Mode::Paused);
    }
}

void resume_cb(lv_event_t *)
{
    G.mode = Mode::Paused;  // show() resumes without a new run
    show(Mode::Play);
}

void title_cb(lv_event_t *)
{
    show(Mode::Title);
}

void again_cb(lv_event_t *)
{
    G.mode = Mode::Title;  // a new run
    show(Mode::Play);
}

void tilt_cb(lv_event_t *e)
{
    G.tilt = toggle_on(static_cast<lv_obj_t *>(lv_event_get_target(e)));
    save_u8("tilt", G.tilt);
    fill_title();
}

// Tilting the top away the other way round: up for down.
void flip_cb(lv_event_t *e)
{
    G.flip = toggle_on(static_cast<lv_obj_t *>(lv_event_get_target(e)));
    save_u8("flip", G.flip);
    fill_title();
}

void sound_cb(lv_event_t *e)
{
    G.sound = toggle_on(static_cast<lv_obj_t *>(lv_event_get_target(e)));
    save_u8("sound", G.sound);
}

void swipe_cb(lv_event_t *)
{
    if (G.mode != Mode::Title) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        G.game = (G.game + (dir == LV_DIR_LEFT ? 1 : games::GAME_COUNT - 1)) % games::GAME_COUNT;
        show(Mode::Title);
    }
}

// Which halves of the screen have a finger on them (flippers): every finger
// the panel reports, or LVGL's one pointer where only that is known.
// Called with T.s.m held.
void holds_from_fingers(bool pointer_down, lv_point_t pointer)
{
    lv_point_t pts[2];
    const int n = touches(pts);
    bool left = false, right = false;
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            (pts[i].x < PANEL / 2 ? left : right) = true;
        }
    } else if (pointer_down) {
        (pointer.x < PANEL / 2 ? left : right) = true;
    }
    T.s.hold_left = left, T.s.hold_right = right;
}

// The frame takes the fingers while playing: a drag steers from where it
// went down; a quick tap or a second finger is the action.
void frame_touch_cb(lv_event_t *e)
{
    if (G.mode != Mode::Play) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const lv_event_code_t code = lv_event_get_code(e);
    std::lock_guard<std::mutex> l(T.s.m);
    holds_from_fingers(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING, p);
    if (code == LV_EVENT_PRESSED) {
        G.press = p;
        G.press_at = now_us();
        T.s.down = true;
        T.s.pad_x = T.s.pad_y = 0;
    } else if (code == LV_EVENT_PRESSING) {
        constexpr float REACH = 70;  // px of drag for full steering
        T.s.pad_x = std::clamp(float(p.x - G.press.x) / REACH, -1.0f, 1.0f);
        T.s.pad_y = std::clamp(float(G.press.y - p.y) / REACH, -1.0f, 1.0f);
        lv_point_t two[2];
        static bool second = false;
        const bool now_two = touches(two) >= 2;
        if (now_two && !second) {
            ++T.s.taps;
        }
        second = now_two;
    } else {  // released or lost
        T.s.down = false;
        T.s.pad_x = T.s.pad_y = 0;
        const int moved = abs(int(p.x - G.press.x)) + abs(int(p.y - G.press.y));
        if (code == LV_EVENT_RELEASED && now_us() - G.press_at < 260000 && moved < 18) {
            ++T.s.taps;
        }
    }
}

// ---- the UI's own beat: swap images, the HUD, sounds, game over ---------------------------------
void tick(lv_timer_t *)
{
    int ready = T.pending.load();
    if (ready >= 0) {
        // shown first, so the task, which reads pending and then shown, never
        // picks it to draw into; a newer frame set meanwhile stays waiting
        T.shown = ready;
        T.pending.compare_exchange_strong(ready, -1);
        G.fade = std::min(32, G.fade + 4);
        lv_obj_invalidate(G.frame);
        wake_task();
    }
    games::Hud hud;
    uint32_t events;
    bool over, thumbs;
    const char *ending;
    {
        std::lock_guard<std::mutex> l(T.s.m);
        hud = T.s.hud;
        events = T.s.events;
        T.s.events = 0;
        over = T.s.over && T.s.result_seq == T.s.reset_seq;
        ending = T.s.ending;
        thumbs = T.s.thumbs_ready;
    }
    if (thumbs && !G.thumbs_shown) {
        G.thumbs_shown = true;
        for (lv_obj_t *t : G.thumb) {
            lv_obj_invalidate(t);
        }
    }
    if (G.mode != Mode::Play) {
        return;
    }
    {
        // fingers come and go between LVGL's events (a second one lifting sends none)
        lv_point_t pts[2];
        const int fingers = touches(pts);
        std::lock_guard<std::mutex> l(T.s.m);
        if (fingers > 0 || !T.s.down) {
            holds_from_fingers(false, pts[0]);
        }
    }
    if (G.ready_until) {  // getting ready: 3, 2, 1
        const int64_t left = G.ready_until - now_us();
        if (left <= 0) {
            G.ready_until = 0;
            set_hidden(G.h_ready, true);
            set_hidden(G.h_count, true);
        } else {
            char n[12];
            snprintf(n, sizeof(n), "%d", int(left / 600000) + 1);
            set_text(G.h_count, n);
        }
    }
    // the HUD, touched only where it changed
    if (hud.score != G.shown_score) {
        char s[16];
        grouped(s, sizeof(s), uint32_t(std::max(0, hud.score)));
        set_text(G.h_score, s);
        G.shown_score = hud.score;
    }
    if (strcmp(hud.line, G.shown_line)) {
        set_text(G.h_line, hud.line);
        snprintf(G.shown_line, sizeof(G.shown_line), "%s", hud.line);
    }
    set_text(G.h_name, hud.meter_name);
    const int fill = std::max(2, int(lroundf(std::clamp(hud.meter, 0.0f, 1.0f) * 120)));
    if (fill != G.shown_fill) {
        lv_obj_set_width(G.h_fill, fill);
        G.shown_fill = fill;
    }
    if (hud.meter_color != G.shown_color) {
        set_bg_color(G.h_fill, lv_color_hex(hud.meter_color));
        G.shown_color = hud.meter_color;
    }
    // what happened: one sound, a buzz, the sub-board's lights
    if (events) {
        const Accent &a = INFO[G.game].accent;
        struct {
            uint32_t bit;
            games_sound_t sound;
            bool strong;
            uint32_t colour;
        } const order[] = {
            {games::EV_OVER, GAMES_SOUND_OVER, true, 0xFF2000},  {games::EV_HIT, GAMES_SOUND_HIT, true, 0xFF0000},
            {games::EV_LEVEL, GAMES_SOUND_LEVEL, false, 0xFFFFFF}, {games::EV_POWER, GAMES_SOUND_POWER, false, a.hi},
            {games::EV_PICKUP, GAMES_SOUND_PICKUP, false, a.glow}, {games::EV_WARN, GAMES_SOUND_WARN, false, 0xFF8000},
        };
        for (const auto &o : order) {
            if (!(events & o.bit)) {
                continue;
            }
            if (G.sound && G.hooks.sound) {
                G.hooks.sound(o.sound);
            }
            if (G.hooks.haptic && o.bit != games::EV_WARN) {
                G.hooks.haptic(o.strong);
            }
            if (G.hooks.board_flash) {
                G.hooks.board_flash(o.colour);
            }
            break;
        }
    }
    if (over) {
        const int score = std::max(0, hud.score);
        const bool record = uint32_t(score) > G.best[G.game];
        if (record) {
            G.best[G.game] = uint32_t(score);
            save_best(G.game);
        }
        char s[24], n[16];
        grouped(s, sizeof(s), uint32_t(score));
        set_text(G.o_score, s);
        set_text(G.o_reason, ending);
        set_text_color(G.o_title, lv_color_hex(INFO[G.game].accent.hi));
        if (record && score > 0) {
            set_text(G.o_best, "New best!");
            set_text_color(G.o_best, lv_color_hex(INFO[G.game].accent.glow));
        } else {
            grouped(n, sizeof(n), G.best[G.game]);
            snprintf(s, sizeof(s), "Best  %s", n);
            set_text(G.o_best, s);
            set_text_color(G.o_best, lv_color_hex(TEXT2));
        }
        show(Mode::Over);
    }
}

// ---- modes -------------------------------------------------------------------------------------
void show(Mode mode)
{
    const Mode previous = G.mode;
    G.mode = mode;
    set_hidden(G.picker, mode != Mode::Picker);
    set_hidden(G.frame, mode == Mode::Picker);
    set_hidden(G.title, mode != Mode::Title);
    set_hidden(G.hud, mode != Mode::Play);
    set_hidden(G.paused, mode != Mode::Paused);
    set_hidden(G.over, mode != Mode::Over);
    if (G.hooks.hold) {
        G.hooks.hold(mode == Mode::Play);
    }
    switch (mode) {
    case Mode::Picker:
        fill_picker();
        command(G.game, true, false, false, 0, H, 32);
        break;
    case Mode::Title:
        fill_title();
        // the game plays itself behind, darker where the words are
        command(G.game, true, true, true, 100, 150, 32);
        break;
    case Mode::Play: {
        const bool resume = previous == Mode::Paused;
        G.shown_score = -1, G.shown_fill = -1, G.shown_color = 0, G.shown_line[0] = 0;
        command(G.game, false, true, !resume, 30, H, 32);
        relevel();
        // With Tilt: a moment to hold the board the way you'll play; the tilt
        // levels to that when it ends (not to how it was held for the tap).
        bool ready = G.tilt;
#ifndef ESP_PLATFORM
        ready = ready && !preview_autoplay;  // the preview's games steer themselves at once
#endif
        G.ready_until = ready ? now_us() + 1800000 : 0;
        {
            std::lock_guard<std::mutex> l(T.s.m);
            T.s.hold_until = G.ready_until;
        }
        set_hidden(G.h_ready, !ready);
        set_hidden(G.h_count, !ready);
        if (!resume && G.sound && G.hooks.sound) {
            G.hooks.sound(GAMES_SOUND_START);
        }
        break;
    }
    case Mode::Paused:
        style_fill(G.resume, INFO[G.game].accent, true);
        command(G.game, false, false, false, 0, H, 9);
        break;
    case Mode::Over:
        style_fill(G.again, INFO[G.game].accent, true);
        command(G.game, false, false, false, 0, H, 10);
        break;
    }
}

// ---- building ------------------------------------------------------------------------------------
void build_picker(lv_obj_t *s)
{
    G.picker = layer(s, true);
    label(G.picker, "Games", 0, 30, PANEL, TEXT, 32, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *back = round_button(G.picker, 56, 32, 48, "back");
    on_click(back, back_cb);
    // the cards scroll under the title
    lv_obj_t *list = box(G.picker, 0, 84, PANEL, PANEL - 84);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(list, 40, 0);  // the last card clears the rounded corners
    for (int i = 0; i < games::GAME_COUNT; ++i) {
        const Info &info = INFO[i];
        const int y = 8 + i * 124;
        lv_obj_t *card = glass(list, 36, y, 408, 116, 26, info.accent);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        press_fx(card);
        on_click(card, card_cb, i);
        lv_obj_t *frame = box(card, 6, 6, THUMB, THUMB, 20);
        lv_obj_set_style_clip_corner(frame, true, 0);
        lv_obj_set_style_bg_color(frame, lv_color_hex(info.accent.glass_b), 0);
        lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
        G.thumb[i] = lv_image_create(frame);
        lv_image_set_src(G.thumb[i], &T.thumb_dsc[i]);
        lv_obj_set_pos(G.thumb[i], 0, 0);
        label(card, info.name, 126, 14, 270, info.accent.hi, 24, true);
        lv_obj_t *tag = label(card, info.tagline, 126, 46, 268, TEXT2, 14, false);
        wrap_label(tag);
        G.card_best[i] = label(card, "", 126, 86, 270, TEXT, 14, true);
    }
}

void build_title(lv_obj_t *s)
{
    G.title = layer(s, false);
    lv_obj_add_flag(G.title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(G.title, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(G.title, swipe_cb, LV_EVENT_GESTURE, nullptr);
    lv_obj_t *back = round_button(G.title, 56, 32, 48, "back");
    on_click(back, back_cb);
    G.t_name = label(G.title, "", 0, 80, PANEL, TEXT, 46, true, LV_TEXT_ALIGN_CENTER);
    G.t_tag = label(G.title, "", 60, 138, PANEL - 120, TEXT2, 17, false, LV_TEXT_ALIGN_CENTER);
    wrap_label(G.t_tag);
    G.t_best = label(G.title, "", 0, 192, PANEL, TEXT, 16, true, LV_TEXT_ALIGN_CENTER);
    // tilt, its up and down flipped, and sound
    label(G.title, "Tilt", 44, 306, 44, TEXT, 16, true);
    G.t_tilt = toggle(G.title, 84, 300, G.tilt, INFO[0].accent);
    lv_obj_add_event_cb(G.t_tilt, tilt_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    label(G.title, "Flip", 174, 306, 44, TEXT, 16, true);
    G.t_flip = toggle(G.title, 212, 300, G.flip, INFO[0].accent);
    lv_obj_add_event_cb(G.t_flip, flip_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    label(G.title, "Sound", 298, 306, 60, TEXT, 16, true);
    G.t_sound = toggle(G.title, 356, 300, G.sound, INFO[0].accent);
    lv_obj_add_event_cb(G.t_sound, sound_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    G.t_play = fill_button(G.title, 150, 352, 180, 56, "Play", INFO[0].accent, 20);
    on_click(G.t_play, play_cb);
    G.t_hint = label(G.title, "", 60, 414, PANEL - 120, TEXT2, 13, false, LV_TEXT_ALIGN_CENTER);
    wrap_label(G.t_hint);  // two lines at most
    for (int i = 0; i < games::GAME_COUNT; ++i) {
        G.t_dots[i] = box(G.title, PANEL / 2 - (games::GAME_COUNT * 14 - 6) / 2 + i * 14, 458, 8, 8, 4);
        lv_obj_set_style_bg_opa(G.t_dots[i], LV_OPA_COVER, 0);
    }
}

void build_hud(lv_obj_t *s)
{
    G.hud = layer(s, false);
    G.h_score = label(G.hud, "0", 0, 14, PANEL, TEXT, 30, true, LV_TEXT_ALIGN_CENTER);
    G.h_name = label(G.hud, "", 96, 52, 76, TEXT2, 12, true, LV_TEXT_ALIGN_RIGHT);
    lv_obj_t *track = box(G.hud, 180, 56, 120, 7, 4);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_60, 0);
    G.h_fill = box(track, 0, 0, 120, 7, 4);
    lv_obj_set_style_bg_opa(G.h_fill, LV_OPA_COVER, 0);
    G.h_line = label(G.hud, "", 60, 70, PANEL - 120, TEXT, 13, false, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *pause = round_button(G.hud, 400, 28, 46, "pause");
    on_click(pause, pause_cb);
    G.h_ready = label(G.hud, "Hold the board the way you'll play", 40, 170, PANEL - 80, TEXT, 20, true, LV_TEXT_ALIGN_CENTER);
    G.h_count = label(G.hud, "3", 0, 206, PANEL, TEXT, 64, true, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(G.h_ready, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(G.h_count, LV_OBJ_FLAG_HIDDEN);
}

void build_cards(lv_obj_t *s)
{
    G.paused = layer(s, false);
    lv_obj_add_flag(G.paused, LV_OBJ_FLAG_CLICKABLE);
    label(G.paused, "Paused", 0, 132, PANEL, TEXT, 38, true, LV_TEXT_ALIGN_CENTER);
    G.resume = fill_button(G.paused, 140, 204, 200, 54, "Resume", AMBER, 18);
    on_click(G.resume, resume_cb);
    lv_obj_t *restart = field_button(G.paused, 140, 270, 200, 50, "Restart", 16);
    on_click(restart, again_cb);
    lv_obj_t *quit = field_button(G.paused, 140, 332, 200, 50, "Quit to title", 16);
    on_click(quit, title_cb);
    label(G.paused, "Tilt levels to how you hold the board on resume.", 60, 404, PANEL - 120, TEXT2, 13, false, LV_TEXT_ALIGN_CENTER);

    G.over = layer(s, false);
    lv_obj_add_flag(G.over, LV_OBJ_FLAG_CLICKABLE);
    G.o_title = label(G.over, "Game over", 0, 104, PANEL, TEXT, 34, true, LV_TEXT_ALIGN_CENTER);
    G.o_reason = label(G.over, "", 0, 150, PANEL, TEXT2, 17, false, LV_TEXT_ALIGN_CENTER);
    G.o_score = label(G.over, "0", 0, 184, PANEL, TEXT, 60, true, LV_TEXT_ALIGN_CENTER);
    G.o_best = label(G.over, "", 0, 262, PANEL, TEXT2, 17, true, LV_TEXT_ALIGN_CENTER);
    G.again = fill_button(G.over, 140, 310, 200, 54, "Play again", AMBER, 18);
    on_click(G.again, again_cb);
    lv_obj_t *title = field_button(G.over, 140, 376, 200, 50, "Title", 16);
    on_click(title, title_cb);
}

bool alloc_buffers()
{
    if (T.frame[0]) {
        return true;
    }
    for (uint16_t * &f : T.frame) {
        f = static_cast<uint16_t *>(big_alloc(size_t(W) * H * 2));
    }
    for (int i = 0; i < games::GAME_COUNT; ++i) {
        T.thumbs[i] = static_cast<uint16_t *>(big_alloc(size_t(THUMB) * THUMB * 2));
    }
    bool ok = T.frame[0] && T.frame[1] && T.frame[2];
    for (uint16_t *t : T.thumbs) {
        ok = ok && t;
    }
    if (!ok) {
        ESP_LOGE(TAG, "no memory for the frames");
        return false;
    }
    for (int i = 0; i < games::GAME_COUNT; ++i) {
        lv_image_dsc_t &d = T.thumb_dsc[i];
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_RGB565;
        d.header.w = THUMB;
        d.header.h = THUMB;
        d.header.stride = THUMB * 2;
        d.data = reinterpret_cast<const uint8_t *>(T.thumbs[i]);
        d.data_size = THUMB * THUMB * 2;
    }
    return true;
}

void build(lv_obj_t *s)
{
    G.screen = s;
    load_settings();
    if (!alloc_buffers()) {
        label(s, "Not enough memory for the games.", 0, 220, PANEL, TEXT, 18, true, LV_TEXT_ALIGN_CENTER);
        return;
    }
    G.frame = box(s, 0, 0, PANEL, PANEL);
    lv_obj_add_flag(G.frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(G.frame, frame_cover_cb, LV_EVENT_COVER_CHECK, nullptr);
    lv_obj_add_event_cb(G.frame, frame_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    for (lv_event_code_t c : {
                LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST
            }) {
        lv_obj_add_event_cb(G.frame, frame_touch_cb, c, nullptr);
    }
    build_title(s);
    build_hud(s);
    build_cards(s);
    build_picker(s);
    {
        std::lock_guard<std::mutex> l(T.s.m);
        T.s.thumbs_wanted = true;
    }
    G.timer = lv_timer_create(tick, 15, nullptr);
    lv_timer_pause(G.timer);
#ifdef ESP_PLATFORM
    games::clock_us = [] {
        return int64_t(esp_timer_get_time());
    };
    // Below LVGL's draw threads (3); its stack in PSRAM (it never touches flash).
    if (xTaskCreatePinnedToCoreWithCaps(game_task, "games-3d", 24 * 1024, nullptr, 2, &T.handle, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
            pdPASS) {
        ESP_LOGE(TAG, "game task not started");
    }
#endif
    G.built = true;
    show(G.mode == Mode::Title ? Mode::Title : Mode::Picker);
}
}  // namespace

extern "C" void games_app_init(const games_hooks_t *hooks)
{
    if (hooks) {
        G.hooks = *hooks;
    }
}

extern "C" void games_app_open(lv_obj_t *screen)
{
    if (!G.built) {
        build(screen);
    }
    games_app_set_active(true);
}

extern "C" void games_app_set_active(bool active)
{
    if (!G.built) {
        return;
    }
    const bool was = T.active.exchange(active);
    if (G.hooks.board_link) {
        G.hooks.board_link(active);
    }
    if (active) {
        lv_timer_resume(G.timer);
        if (!was) {
            T.last_us = 0;
            if (G.mode == Mode::Play) {
                show(Mode::Paused);  // left mid-game: wait for Resume
            } else {
                show(G.mode);
            }
        }
        wake_task();
    } else {
        if (G.mode == Mode::Play) {
            G.mode = Mode::Paused;
            set_hidden(G.hud, true);
            set_hidden(G.paused, false);
            command(G.game, false, false, false, 0, H, 9);
        }
        if (G.hooks.hold) {
            G.hooks.hold(false);
        }
        lv_timer_pause(G.timer);
    }
}

extern "C" bool games_app_playing(void)
{
    return G.built && T.active.load() && G.mode == Mode::Play;
}

extern "C" void games_app_select(int game)
{
    if (game >= 0 && game < games::GAME_COUNT) {
        G.game = game;
        if (G.built) {
            show(Mode::Title);
        } else {
            G.mode = Mode::Title;
        }
    } else if (G.built) {
        show(Mode::Picker);
    } else {
        G.mode = Mode::Picker;
    }
}

#ifndef ESP_PLATFORM
// The host preview: one synchronous frame (step, draw, swap) per call.
extern "C" void games_app_preview_frame(void)
{
    if (!G.built) {
        return;
    }
    T.active = true;
    frame_once();
    tick(nullptr);
}
// The preview's screens: 0 picker, 1 title, 2 playing (steering itself), 3 paused, 4 game over.
extern "C" void games_app_preview_mode(int mode)
{
    preview_autoplay = mode >= 2;
    if (mode == 4) {
        {
            std::lock_guard<std::mutex> l(T.s.m);
            T.s.over = true;
            T.s.result_seq = T.s.reset_seq;
            T.s.ending = "Out of time";
        }
        tick(nullptr);  // the card, as the board shows it at a game's end
        return;
    }
    show(mode == 0 ? Mode::Picker : mode == 1 ? Mode::Title : mode == 2 ? Mode::Play : Mode::Paused);
}
extern "C" void games_app_preview_best(int game, uint32_t score)
{
    G.best[game] = score;
}
// The preview drives a played game with a fixed input.
extern "C" void games_app_preview_touch(bool down, float x, float y, bool tap)
{
    std::lock_guard<std::mutex> l(T.s.m);
    T.s.down = down, T.s.pad_x = x, T.s.pad_y = y;
    T.s.taps += tap ? 1 : 0;
}
#endif
