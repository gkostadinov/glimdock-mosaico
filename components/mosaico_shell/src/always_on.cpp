// SPDX-License-Identifier: Apache-2.0
// The always-on display (see always_on.h). Three stages: awake; dimmed (the
// app at low light for the last seconds before the clock, as a phone does);
// and the clock, a screen of its own: while it is loaded LVGL draws nothing
// of the app under it (an app's changes to its own hidden screen draw
// nothing), and the clock changes once a minute. All on the LVGL task, except
// the timeout's getter and setter and the stage queries.
#include "always_on.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include <algorithm>
#include <atomic>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mui.h"
#include "nvs.h"
#include "reminders.h"
#include "shell_internal.h"

namespace {
using namespace mui;
const char *TAG = "always-on";
constexpr const char *NS = "mshell", *KEY = "aod";  // the shell's settings; seconds (u16)
constexpr int DEFAULT_TIMEOUT = 120;
// Against burn-in: every minute the clock moves to the next of these offsets
// (px), so no pixel stays lit in the same place for long.
constexpr int8_t SHIFT[][2] = {
    {0, 0}, {6, -4}, {-5, 6}, {3, 8}, {-8, -3}, {8, 3}, {-3, -8}, {-6, 5}, {5, -6}, {9, 0}, {0, 9}, {-9, 0}, {0, -9},
};
constexpr int SHIFTS = int(sizeof(SHIFT) / sizeof(SHIFT[0]));
// Soft greys: an AMOLED pixel's light, power and wear follow its value.
constexpr uint32_t CLOCK = 0xDADADF, SOFT = 0x8E8E96;
constexpr int BLOCK_W = 420, CLOCK_PX = 112, BELL = 20, BELL_GAP = 8;
// The dimmed stage: up to this long before the clock (a third of a short timeout).
constexpr int DIM_S = 10;
// Fades: steps of the panel's light, this far apart (each step is a panel
// command, sent between frames).
constexpr int FADE_STEPS = 8, FADE_STEP_MS = 60;

enum class Stage : uint8_t { Awake, Dimmed, Clock };

struct {
    AlwaysOnHooks hooks{};
    std::atomic<int> timeout{DEFAULT_TIMEOUT};
    int saved = -1;  // what NVS holds
    std::atomic<Stage> stage{Stage::Awake};
    bool swallow = false;  // the touch that woke it, until it is lifted
    uint32_t dimmed_idle = 0;  // the time without use when it dimmed (less now: used again)
    // The fade under way: the panel's light from one level to another.
    lv_timer_t *fader = nullptr;
    int fade_from = 0, fade_to = 0, fade_step = 0, light = -1;
    lv_obj_t *screen = nullptr, *block = nullptr, *clock = nullptr, *date = nullptr, *reminder = nullptr,
              *bell = nullptr, *reminder_text = nullptr, *battery = nullptr;
    lv_obj_t *back = nullptr;  // the screen under it
    int64_t minute = -1;       // the minute shown
    int shift = 0;
    // For the report (RPC 49).
    int64_t since = 0, shown_us = 0;
    int entries = 0, redraws = 0;
} A;

int snap(int seconds)
{
    int best = ALWAYS_ON_CHOICES[0];
    for (int c : ALWAYS_ON_CHOICES)
        if (abs(c - seconds) < abs(best - seconds)) {
            best = c;
        }
    return best;
}

void save(int seconds)
{
    A.saved = seconds;  // once, even if the write fails: no retry every second
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, KEY, uint16_t(seconds));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save the timeout (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "saved: %d s (0: off)", seconds);
    }
}

void load()
{
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, KEY, &v) == ESP_OK) {
            A.timeout = snap(v);
        }
        nvs_close(h);
    }
    A.saved = A.timeout;  // the default stays unsaved until a choice is made
}

// The panel's light while dimmed and under the clock, from the person's brightness.
int clock_level()
{
    const int b = A.hooks.brightness ? A.hooks.brightness() : 80;
    return std::clamp((b + 5) / 10, 2, 6);
}

int dim_level()
{
    const int b = A.hooks.brightness ? A.hooks.brightness() : 80;
    return std::clamp(b * 3 / 10, clock_level(), std::max(b, clock_level()));
}

void set_light(int percent)  // -1: the person's setting
{
    if (percent != A.light && A.hooks.light) {
        A.hooks.light(percent);
    }
    A.light = percent;
}

void fade_tick(lv_timer_t *t)
{
    ++A.fade_step;
    // Even steps in log space: the eye sees light that way.
    const float k = float(A.fade_step) / FADE_STEPS;
    const float v = float(A.fade_from) * powf(float(A.fade_to) / float(std::max(1, A.fade_from)), k);
    set_light(A.fade_step >= FADE_STEPS ? A.fade_to : std::max(1, int(lroundf(v))));
    if (A.fade_step >= FADE_STEPS) {
        lv_timer_pause(t);
    }
}

// Fades the panel down to percent (from where it is).
void fade_to(int percent)
{
    const int from = A.light >= 0 ? A.light : A.hooks.brightness ? A.hooks.brightness() : 80;
    A.fade_from = std::max(1, from), A.fade_to = percent, A.fade_step = 0;
    if (!A.fader) {
        A.fader = lv_timer_create(fade_tick, FADE_STEP_MS, nullptr);
    } else {
        lv_timer_reset(A.fader);
        lv_timer_resume(A.fader);
    }
}

// Back to the person's light at once (a touch should see the app straight away).
void full_light()
{
    if (A.fader) {
        lv_timer_pause(A.fader);
    }
    set_light(-1);
}

void build()
{
    lv_obj_t *s = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    A.screen = s;
    const int clock_h = lv_font_get_line_height(font(CLOCK_PX, false));
    A.block = box(s, (PANEL - BLOCK_W) / 2, 0, BLOCK_W, clock_h);
    // Inter Regular: thin strokes light fewer pixels than the semibold of the apps.
    A.clock = label(A.block, "--:--", 0, 0, BLOCK_W, CLOCK, CLOCK_PX, false, LV_TEXT_ALIGN_CENTER);
    A.date = label(A.block, "", 0, clock_h - 6, BLOCK_W, SOFT, 20, true, LV_TEXT_ALIGN_CENTER);
    A.reminder = box(A.block, 0, 0, BLOCK_W, 26);
    A.bell = icon(A.reminder, "bell", 0, 3, BELL, AMBER.glow, 2.0f);
    A.reminder_text = label(A.reminder, "", BELL + BELL_GAP, 2, BLOCK_W - BELL - BELL_GAP, SOFT, 16, true);
    lv_label_set_long_mode(A.reminder_text, LV_LABEL_LONG_MODE_DOTS);
    A.battery = label(A.block, "", 0, 0, BLOCK_W, SOFT, 15, true, LV_TEXT_ALIGN_CENTER);
    set_hidden(A.reminder, true);
    set_hidden(A.battery, true);
}

// The next reminder, when it comes within a day: "18:30 · Water the plants"
// (or "Fri 08:00 · ..." on another day).
bool reminder_line(char *out, size_t cap)
{
    time_t at = 0;
    char what[96];
    const time_t now = time(nullptr);
    if (!reminders_next(&at, what, sizeof(what)) || at < now - 60 || at - now > 24 * 3600) {
        return false;
    }
    tm when{}, today{};
    localtime_r(&at, &when);
    localtime_r(&now, &today);
    static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (when.tm_yday == today.tm_yday) {
        snprintf(out, cap, "%02d:%02d \xC2\xB7 %s", when.tm_hour, when.tm_min, what);
    } else {
        snprintf(out, cap, "%s %02d:%02d \xC2\xB7 %s", DAYS[when.tm_wday % 7], when.tm_hour, when.tm_min, what);
    }
    return true;
}

// Fills the clock in for this minute and moves it to its next place. Nothing
// when the minute shown is still the one now (unless forced).
void refresh(bool force)
{
    const int64_t minute = int64_t(time(nullptr)) / 60;
    if (!force && minute == A.minute) {
        return;
    }
    A.minute = minute;
    AlwaysOnInfo info;
    if (A.hooks.info) {
        A.hooks.info(&info);
    }
    static const char *const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const MONTHS[] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"
                                        };
    char text[160];
    if (info.timed) {
        snprintf(text, sizeof(text), "%02d:%02d", info.local.tm_hour, info.local.tm_min);
        set_text(A.clock, text);
        snprintf(text, sizeof(text), "%s %d %s", DAYS[info.local.tm_wday % 7], info.local.tm_mday,
                 MONTHS[info.local.tm_mon % 12]);
        set_text(A.date, text);
    } else {
        set_text(A.clock, "--:--");
        set_text(A.date, "");
    }
    const int clock_h = lv_font_get_line_height(font(CLOCK_PX, false));
    int y = clock_h + 34;  // under the date
    const bool soon = info.timed && reminder_line(text, sizeof(text));
    set_hidden(A.reminder, !soon);
    if (soon) {
        // The bell and its text centred together.
        set_text(A.reminder_text, text);
        const int w = std::min(text_width(text, 16, true) + 2, BLOCK_W - BELL - BELL_GAP);
        const int x = (BLOCK_W - (BELL + BELL_GAP + w)) / 2;
        lv_obj_set_x(A.bell, x);
        lv_obj_set_x(A.reminder_text, x + BELL + BELL_GAP);
        lv_obj_set_width(A.reminder_text, w);
        lv_obj_set_y(A.reminder, y);
        y += 32;
    }
    set_hidden(A.battery, info.battery < 0);
    if (info.battery >= 0) {
        const int b = std::clamp(info.battery, 0, 100);
        const char *glyph = info.charging ? LV_SYMBOL_CHARGE
                            : b > 80      ? LV_SYMBOL_BATTERY_FULL
                            : b > 55      ? LV_SYMBOL_BATTERY_3
                            : b > 30      ? LV_SYMBOL_BATTERY_2
                            : b > 10      ? LV_SYMBOL_BATTERY_1
                            : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(text, sizeof(text), "%s  %d%%", glyph, b);
        set_text(A.battery, text);
        set_text_color(A.battery, lv_color_hex(info.charging ? OK : b <= 15 ? RED : SOFT));
        lv_obj_set_y(A.battery, y);
        y += 26;
    }
    // A little above the middle (where the eye puts it), shifted.
    const int8_t *d = SHIFT[A.shift];
    A.shift = (A.shift + 1) % SHIFTS;
    lv_obj_set_height(A.block, y);
    lv_obj_set_pos(A.block, (PANEL - BLOCK_W) / 2 + d[0], (PANEL - y) / 2 - 12 + d[1]);
    ++A.redraws;
}

void enter()
{
    if (A.stage == Stage::Clock || !A.screen) {
        return;
    }
    A.back = lv_screen_active();
    if (A.hooks.pause) {
        A.hooks.pause(true);
    }
    // The top layer stays as it is: a reminder wakes the board before its
    // card goes up, and LVGL 9.5 cannot show a hidden layer again (showing an
    // object marks its parent's layout, and a layer has none).
    refresh(true);
    lv_screen_load(A.screen);
    A.stage = Stage::Clock;
    lv_refr_now(nullptr);  // the clock on the glass, then the light down
    fade_to(clock_level());
    A.since = esp_timer_get_time();
    ++A.entries;
    ESP_LOGI(TAG, "on (after %d s without use)", A.timeout.load());
}

// The dimmed stage: the app stays as it is, at low light.
void dim()
{
    if (A.stage != Stage::Awake) {
        return;
    }
    A.stage = Stage::Dimmed;
    A.dimmed_idle = lv_display_get_inactive_time(nullptr);
    fade_to(dim_level());
    ESP_LOGI(TAG, "dimmed");
}

void wake()
{
    if (A.stage == Stage::Dimmed) {
        A.stage = Stage::Awake;
        full_light();
        lv_display_trigger_activity(nullptr);
        ESP_LOGI(TAG, "undimmed");
        return;
    }
    if (A.stage != Stage::Clock) {
        return;
    }
    A.stage = Stage::Awake;
    A.shown_us += esp_timer_get_time() - A.since;
    if (A.hooks.pause) {
        A.hooks.pause(false);
    }
    // shell_show may have loaded another screen already (an app opened while
    // the clock was up): that one stays.
    if (lv_screen_active() == A.screen) {
        if (A.back && lv_obj_is_valid(A.back) && A.back != A.screen) {
            lv_screen_load(A.back);
        } else {
            shell_show(ShellApp::Launcher);
        }
    }
    lv_refr_now(nullptr);  // the app on the glass before its light comes back
    full_light();
    lv_display_trigger_activity(nullptr);
    ESP_LOGI(TAG, "woken after %lld s", (long long)((esp_timer_get_time() - A.since) / 1000000));
}

// Every second: the setting to NVS, the wait, and the minute while it shows.
void tick(lv_timer_t *)
{
    const int timeout = A.timeout;
    if (timeout != A.saved) {
        save(timeout);
    }
    const bool busy = A.hooks.busy && A.hooks.busy();
    const Stage stage = A.stage;
    if (stage != Stage::Awake && (busy || timeout <= 0)) {
        if (busy) {
            ESP_LOGI(TAG, "the screen is needed");
        }
        wake();
        return;
    }
    if (stage == Stage::Clock) {
        refresh(false);
        return;
    }
    if (busy || timeout <= 0) {
        lv_display_trigger_activity(nullptr);  // the wait starts when it is free (or switched on)
        return;
    }
    const uint32_t idle = lv_display_get_inactive_time(nullptr);
    const uint32_t clock_at = uint32_t(timeout) * 1000u, dim_at = clock_at - uint32_t(std::min(DIM_S, timeout / 3)) * 1000u;
    if (stage == Stage::Dimmed) {
        if (idle < A.dimmed_idle) {
            wake();  // used again (not by a touch, which undims at once)
        } else if (idle >= clock_at) {
            enter();
        }
    } else if (idle >= dim_at) {
        dim();
    }
}
}  // namespace

void always_on_start(const AlwaysOnHooks &hooks)
{
    A.hooks = hooks;
    load();
    build();
    lv_timer_create(tick, 1000, nullptr);
    ESP_LOGI(TAG, "after %d s without use (0: off)", A.timeout.load());
}

int always_on_timeout()
{
    return A.timeout;
}

void always_on_set_timeout(int seconds)
{
    A.timeout = snap(seconds);
}

bool always_on_showing()
{
    return A.stage == Stage::Clock;
}

bool always_on_resting()
{
    return A.stage != Stage::Awake;
}

void always_on_dim()
{
    dim();
}

void always_on_show()
{
    enter();
}

void always_on_wake()
{
    wake();
    lv_display_trigger_activity(nullptr);  // an app opened, a reminder: the wait starts again
}

bool always_on_touch(bool pressed)
{
    if (!pressed) {
        A.swallow = false;
        return false;
    }
    // Dimmed too: a tap to see the app again (the screen being dim) is not
    // meant for what it lands on.
    if (A.stage != Stage::Awake && !A.swallow) {
        A.swallow = true;
        // Not from inside the touch read: LVGL is still handling this read.
        lv_async_call([](void *) {
            wake();
        }, nullptr);
    }
    return A.swallow;
}

const char *always_on_config_apply(const char *json, size_t length)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, length) || !doc.is<JsonObject>()) {
        return "expected an object";
    }
    for (JsonPair kv : doc.as<JsonObject>())
        if (strcmp(kv.key().c_str(), "always_on")) {
            return "unknown key (use always_on)";
        }
    JsonVariant v = doc["always_on"];
    if (v.isNull()) {
        return nullptr;
    }
    const int seconds = v.is<int>() ? v.as<int>() : -1;
    if (std::find(std::begin(ALWAYS_ON_CHOICES), std::end(ALWAYS_ON_CHOICES), seconds) == std::end(ALWAYS_ON_CHOICES)) {
        return "always_on: seconds, one of 0 (off), 30, 60, 120, 300, 600";
    }
    always_on_set_timeout(seconds);
    return nullptr;  // applies at once
}

void always_on_config_status(char *json, size_t capacity)
{
    snprintf(json, capacity, "{\"always_on\":%d}", always_on_timeout());
}

void always_on_report(char *out, size_t capacity)
{
    const int64_t now = esp_timer_get_time();
    const Stage stage = A.stage;
    const int64_t shown = A.shown_us + (stage == Stage::Clock ? now - A.since : 0);
    snprintf(out, capacity, "always-on: %s; timeout %d s; idle %u s; shown %d times, %lld s in all; %d clock draws",
             stage == Stage::Clock ? "showing" : stage == Stage::Dimmed ? "dimmed" : "off", A.timeout.load(),
             unsigned(lv_display_get_inactive_time(nullptr) / 1000),
             A.entries, (long long)(shown / 1000000), A.redraws);
}
