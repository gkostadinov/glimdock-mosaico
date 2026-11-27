// SPDX-License-Identifier: Apache-2.0
// Reminders (see reminders.h). Swifty's tools run on its agent task, whose
// stack is in PSRAM: they only change the list. Saving it (a flash write) and
// firing happen on the LVGL task, from the check timer.
#include "reminders.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "ArduinoJson.h"
#include "esp_log.h"
#include "glimdock_app.h"
#include "haptics.h"
#include "mui.h"
#include "nvs.h"
#include "swifty_app.h"

namespace {
using namespace mui;
const char *TAG = "reminders";
constexpr const char *NS = "reminders";
constexpr size_t MAX_COUNT = 40, MAX_TEXT = 160;
constexpr time_t CLOCK_SET = 1735689600;  // 2025-01-01: before this the clock is not set yet
constexpr int SNOOZE_MIN = 10;

enum class Repeat : uint8_t { None, Daily, Weekdays, Weekly };
const char *const REPEAT_NAMES[] = {"none", "daily", "weekdays", "weekly"};

struct Reminder {
    uint32_t id;
    time_t at;
    std::string text;
    Repeat repeat;
};

std::mutex lock;
std::vector<Reminder> list;
uint32_t next_id = 1;
bool dirty = false;  // changed since saved (the timer saves it)

struct {
    lv_obj_t *card = nullptr;  // the reminder showing, on the top layer
    Reminder shown;
    lv_timer_t *buzz = nullptr;
    int buzzes = 0;
} R;

// ---- time -----------------------------------------------------------------------------------
std::string when(time_t t)
{
    tm local;
    localtime_r(&t, &local);
    char text[32];
    strftime(text, sizeof(text), "%a %d %b, %H:%M", &local);
    return text;
}

time_t add_days(time_t t, int days)
{
    tm local;
    localtime_r(&t, &local);
    local.tm_mday += days;
    local.tm_isdst = -1;  // the same wall-clock time across a change of DST
    return mktime(&local);
}

bool weekend(time_t t)
{
    tm local;
    localtime_r(&t, &local);
    return local.tm_wday == 0 || local.tm_wday == 6;
}

// The first time it comes round after `after` (for one that repeats).
time_t next_after(time_t at, Repeat repeat, time_t after)
{
    for (int guard = 0; guard < 800 && (at <= after || (repeat == Repeat::Weekdays && weekend(at))); ++guard) {
        at = add_days(at, repeat == Repeat::Weekly ? 7 : 1);
    }
    return at;
}

const char *repeat_text(Repeat r)
{
    return r == Repeat::Daily ? "every day" : r == Repeat::Weekdays ? "every weekday" : r == Repeat::Weekly ? "every week" : "once";
}

// ---- storage (LVGL task) --------------------------------------------------------------------
void load()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = 0;
    std::string text;
    if (nvs_get_str(h, "list", nullptr, &n) == ESP_OK && n > 1) {
        text.resize(n);
        nvs_get_str(h, "list", text.data(), &n);
    }
    uint32_t next = 1;
    nvs_get_u32(h, "next", &next);
    nvs_close(h);
    JsonDocument doc;
    if (text.empty() || deserializeJson(doc, text.c_str())) {
        return;
    }
    std::lock_guard<std::mutex> g(lock);
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        const int r = std::clamp(o["repeat"] | 0, 0, 3);
        list.push_back({o["id"] | 0u, time_t(o["at"] | int64_t(0)), o["text"] | "", Repeat(r)});
    }
    next_id = std::max<uint32_t>(next, 1);
    ESP_LOGI(TAG, "%u reminders", unsigned(list.size()));
}

void save()
{
    std::string text;
    uint32_t next;
    {
        std::lock_guard<std::mutex> g(lock);
        if (!dirty) {
            return;
        }
        dirty = false;
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (const Reminder &r : list) {
            JsonObject o = arr.add<JsonObject>();
            o["id"] = r.id;
            o["at"] = int64_t(r.at);
            o["text"] = r.text;
            o["repeat"] = int(r.repeat);
        }
        serializeJson(doc, text);
        next = next_id;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_str(h, "list", text.c_str()) != ESP_OK || nvs_set_u32(h, "next", next) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the reminders");
    }
    nvs_close(h);
}

// ---- the card (LVGL task) -------------------------------------------------------------------
void show_due();

void close_card()
{
    if (R.buzz) {
        lv_timer_delete(R.buzz);
        R.buzz = nullptr;
    }
    if (R.card) {
        lv_obj_delete(R.card);
        R.card = nullptr;
    }
}

void done_cb(lv_event_t *)
{
    close_card();
    show_due();  // the next one, if another is due
}

void snooze_cb(lv_event_t *)
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (list.size() < MAX_COUNT) {
            list.push_back({next_id++, time(nullptr) + SNOOZE_MIN * 60, R.shown.text, Repeat::None});
            dirty = true;
        }
    }
    close_card();
    show_due();
}

void show(const Reminder &r)
{
    R.shown = r;
    lv_obj_t *back = box(lv_layer_top(), 0, 0, PANEL, PANEL);  // dims the app and takes its taps
    lv_obj_set_style_bg_color(back, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(back, 150, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    R.card = back;
    lv_obj_t *c = glass(back, 36, 96, 408, 288, 30, AMBER);
    lv_obj_t *badge = box(c, 24, 24, 48, 48, 24);
    lv_obj_set_style_bg_color(badge, lv_color_hex(AMBER.glow), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    icon(badge, "bell", 12, 12, 24, AMBER.ink, 2.2f);
    label(c, "Reminder", 88, 24, 280, TEXT, 20, true);
    label(c, when(r.at).c_str(), 88, 52, 280, TEXT3, 14);
    lv_obj_t *text = label(c, r.text.c_str(), 24, 96, 360, TEXT, 22, true);
    wrap_label(text);
    lv_obj_set_height(text, 104);  // up to four lines
    lv_obj_t *snooze = field_button(c, 24, 214, 170, 50, "Snooze 10 min", 16);
    lv_obj_add_event_cb(snooze, snooze_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *done = fill_button(c, 210, 214, 174, 50, "Done", AMBER, 17);
    lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, nullptr);
    stagger(c, 0, 20);
    // A chime and three buzzes, to be noticed.
    glimdock_app_play_reminder();
    haptics_pulse(HapticPulse::Tap);
    R.buzzes = 0;
    R.buzz = lv_timer_create([](lv_timer_t *) {
        haptics_pulse(HapticPulse::Tap);
        if (++R.buzzes >= 2) {
            R.buzz = nullptr;  // its last run: LVGL deletes it
        }
    }, 280, nullptr);
    lv_timer_set_repeat_count(R.buzz, 2);
    lv_timer_set_auto_delete(R.buzz, true);
    ESP_LOGI(TAG, "reminder %u is due", unsigned(r.id));
}

// The earliest due reminder: shown, and moved on (one that repeats) or
// removed (one that does not).
void show_due()
{
    const time_t now = time(nullptr);
    if (R.card || now < CLOCK_SET) {
        return;
    }
    Reminder due;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = std::min_element(list.begin(), list.end(), [](const Reminder & a, const Reminder & b) {
            return a.at < b.at;
        });
        if (it == list.end() || it->at > now) {
            return;
        }
        due = *it;
        if (it->repeat == Repeat::None) {
            list.erase(it);
        } else {
            it->at = next_after(it->at, it->repeat, now);
        }
        dirty = true;
    }
    show(due);
}

void check(lv_timer_t *)
{
    show_due();
    save();
}

// ---- Swifty's tools (its agent task) --------------------------------------------------------
// "2026-11-27 18:30" (or with a T), or "18:30" for the next time it comes.
bool parse_at(const char *text, time_t now, time_t *out)
{
    tm t = {};
    localtime_r(&now, &t);
    int y, mo, d, h, mi;
    if (sscanf(text, "%d-%d-%d%*[ T]%d:%d", &y, &mo, &d, &h, &mi) == 5) {
        t.tm_year = y - 1900, t.tm_mon = mo - 1, t.tm_mday = d;
    } else if (sscanf(text, "%d:%d", &h, &mi) == 2) {
        // today, or tomorrow when that time has gone
    } else {
        return false;
    }
    if (h < 0 || h > 23 || mi < 0 || mi > 59) {
        return false;
    }
    t.tm_hour = h, t.tm_min = mi, t.tm_sec = 0, t.tm_isdst = -1;
    time_t at = mktime(&t);
    if (at == time_t(-1)) {
        return false;
    }
    if (!strchr(text, '-') && at <= now) {
        at = add_days(at, 1);
    }
    *out = at;
    return true;
}

void add_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    deserializeJson(a, args);
    const time_t now = time(nullptr);
    if (now < CLOCK_SET) {
        snprintf(out, cap, "The clock is not set yet (no network time), so reminders cannot be set.");
        return;
    }
    std::string text = a["text"] | "";
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) {
        text.pop_back();
    }
    if (text.empty()) {
        snprintf(out, cap, "Say what to remind about (text).");
        return;
    }
    if (text.size() > MAX_TEXT) {
        text.resize(MAX_TEXT);
    }
    const std::string rep = a["repeat"] | "none";
    Repeat repeat = Repeat::None;
    for (int i = 0; i < 4; ++i)
        if (rep == REPEAT_NAMES[i]) {
            repeat = Repeat(i);
        }
    time_t at;
    if (a["in_minutes"].is<float>() && a["in_minutes"].as<float>() > 0) {
        at = now + time_t(a["in_minutes"].as<float>() * 60 + 0.5f);
    } else if (!parse_at(a["at"] | "", now, &at)) {
        snprintf(out, cap, "Give the time as at (\"YYYY-MM-DD HH:MM\" or \"HH:MM\", local time) or in_minutes.");
        return;
    }
    if (repeat != Repeat::None) {
        at = next_after(at, repeat, now);
    } else if (at <= now) {
        snprintf(out, cap, "%s has already passed.", when(at).c_str());
        return;
    }
    if (at > now + 400LL * 86400) {
        snprintf(out, cap, "That is too far ahead (a year at most).");
        return;
    }
    uint32_t id;
    {
        std::lock_guard<std::mutex> g(lock);
        if (list.size() >= MAX_COUNT) {
            snprintf(out, cap, "There are already %u reminders: delete some first.", unsigned(MAX_COUNT));
            return;
        }
        id = next_id++;
        list.push_back({id, at, text, repeat});
        dirty = true;
    }
    snprintf(out, cap, "Reminder %u set for %s (%s): %s", unsigned(id), when(at).c_str(), repeat_text(repeat), text.c_str());
}

void list_tool(const char *, char *out, size_t cap)
{
    std::vector<Reminder> copy;
    {
        std::lock_guard<std::mutex> g(lock);
        copy = list;
    }
    if (copy.empty()) {
        snprintf(out, cap, "No reminders.");
        return;
    }
    std::sort(copy.begin(), copy.end(), [](const Reminder & a, const Reminder & b) {
        return a.at < b.at;
    });
    std::string text;
    for (const Reminder &r : copy) {
        char line[256];
        snprintf(line, sizeof(line), "%u: %s, %s: %s\n", unsigned(r.id), when(r.at).c_str(), repeat_text(r.repeat), r.text.c_str());
        text += line;
    }
    strlcpy(out, text.c_str(), cap);
}

void delete_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    deserializeJson(a, args);
    const uint32_t id = a["id"] | 0u;
    std::lock_guard<std::mutex> g(lock);
    auto it = std::find_if(list.begin(), list.end(), [id](const Reminder & r) {
        return r.id == id;
    });
    if (it == list.end()) {
        snprintf(out, cap, "There is no reminder %u (list_reminders shows them).", unsigned(id));
        return;
    }
    snprintf(out, cap, "Deleted reminder %u: %s", unsigned(id), it->text.c_str());
    list.erase(it);
    dirty = true;
}
}  // namespace

void reminders_start()
{
    load();
    swifty_app_add_tool("add_reminder",
                        "Remind the person at a time: a card comes up on the board with a buzz, whatever app is open. Once, "
                        "or repeating (a schedule: daily, weekdays, weekly). Give at in local time, or in_minutes from now.",
                        R"({"type":"object","properties":{"text":{"type":"string","description":"What to remind about, short."},)"
                        R"("at":{"type":"string","description":"Local time: YYYY-MM-DD HH:MM, or HH:MM for the next time it comes."},)"
                        R"("in_minutes":{"type":"number","description":"Instead of at: minutes from now."},)"
                        R"("repeat":{"type":"string","enum":["none","daily","weekdays","weekly"]}},"required":["text"]})",
                        add_tool);
    swifty_app_add_tool("list_reminders", "The reminders set, soonest first, with their numbers.",
                        R"({"type":"object","properties":{},"required":[]})", list_tool);
    swifty_app_add_tool("delete_reminder", "Delete a reminder by its number (from list_reminders).",
                        R"({"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]})", delete_tool);
    lv_timer_create(check, 2000, nullptr);
}
