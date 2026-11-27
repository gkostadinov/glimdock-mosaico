// SPDX-License-Identifier: Apache-2.0
// Shortcuts (see shortcuts.h). The list lives in memory behind a mutex: Web
// settings, Swifty's agent task and RPC 55 change it; the LVGL task saves it
// (a flash write) from its timer, as reminders do, and hands the home screen
// its tiles. A run is a script on its own task (stack in PSRAM, below LVGL's
// drawing, no flash writes): Swifty Home commands start on the LVGL task (with
// the display lock) and are waited for, waits sleep, and the Interaction
// board's steps, which can take seconds, block there and not the screen.
#include "shortcuts.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "ArduinoJson.h"
#include "always_on.h"
#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "haptics.h"
#include "home_app.h"
#include "launcher_ui.h"
#include "mui.h"
#include "nvs.h"
#include "swifty_app.h"
#include "tools_app.h"

namespace {
using namespace mui;
const char *TAG = "shortcuts";
constexpr const char *NS = "shortcuts";
constexpr size_t MAX_COUNT = 24, MAX_STEPS = 10, MAX_NAME = 32, MAX_STORED = 12 * 1024;
constexpr int MAX_WAIT_MS = 60000, MAX_QUEUE = 4;

enum class Do : uint8_t { Home, Wait, Ir, BoardLights, Brightness, Open, Flashlight };
const char *const DO_NAMES[] = {"home", "wait", "ir", "board_lights", "brightness", "open", "flashlight"};
constexpr int DO_COUNT = 7;

const char *const HOME_VERBS[] = {"lights_on", "lights_off", "lights_toggle", "brightness", "color", "scene",
                                  "plug_on", "plug_off", "plug_toggle", "tv_on", "tv_off", "tv_toggle",
                                  "volume", "play", "pause", "mute", "unmute"
                                 };
const char *const IR_KEYS[] = {"power", "volume_up", "volume_down", "mute", "input", "ok", "up", "down", "left",
                               "right", "back", "home", "channel_up", "channel_down"
                              };
const char *const BOARD_MODES[] = {"off", "react", "rainbow", "night", "colour"};
const char *const APPS[] = {"launcher", "glimdock", "home", "camera", "swifty", "files", "tools", "gallery",
                            "webcam", "interaction", "calculator"
                           };
const char *const TOOLS[] = {"torch", "level", "compass"};
const char *const ICONS[] = {"bulb", "power", "house", "rooms", "tv", "speaker", "play", "pause", "snow", "bell",
                             "clock", "lock", "key", "heart", "paw", "camera", "mic", "cloud", "printer", "laptop",
                             "nas", "puzzle", "pin", "person"
                            };
const char *const ACCENTS[] = {"violet", "cyan", "amber", "green"};
const Accent *const ACCENT_STYLES[] = {&VIOLET, &CYAN, &AMBER, &GREEN};

template <size_t N> int index_of(const char *const(&names)[N], const std::string &name)
{
    for (size_t i = 0; i < N; ++i)
        if (!strcasecmp(names[i], name.c_str())) {
            return int(i);
        }
    return -1;
}

template <size_t N> std::string joined(const char *const(&names)[N])
{
    std::string out;
    for (size_t i = 0; i < N; ++i) {
        out += (i ? ", " : "") + std::string(names[i]);
    }
    return out;
}

// One step. home: verb, target, value. ir: value (the key). board_lights:
// verb (the mode), value (the colour). open: verb (the app), value (a tool).
// wait: n ms. brightness: n percent. flashlight: n 1 on, 0 off.
struct Step {
    Do kind = Do::Wait;
    std::string verb, target, value;
    int n = 0;
};

struct Shortcut {
    uint32_t id = 0;
    std::string name, icon;
    uint8_t accent = 0;
    std::vector<Step> steps;
};

std::mutex lock;
std::vector<Shortcut> list;
uint32_t next_id = 1;
bool seeded = false;         // the starters were offered once (deleting them is final)
bool dirty = false;          // changed since saved (the timer saves it)
uint32_t revision = 1;       // bumped by every change: the tiles follow it
std::string last_result;     // how the last run went, for RPC 55
std::atomic<bool> dry_all{false};  // RPC 55: every run is a dry run until a restart
std::atomic<bool> starters_wanted{false};  // RPC 55 asked for them: the LVGL timer adds them
ShortcutHooks H;

// ---- JSON (the stored form is the one the API takes) ------------------------------------
void step_json(const Step &s, JsonObject o)
{
    o["do"] = DO_NAMES[int(s.kind)];
    switch (s.kind) {
    case Do::Home:
        o["verb"] = s.verb;
        if (!s.target.empty()) {
            o["target"] = s.target;
        }
        if (!s.value.empty()) {
            o["value"] = s.value;
        }
        break;
    case Do::Wait:
        o["seconds"] = s.n / 1000.0f;
        break;
    case Do::Ir:
        o["key"] = s.value;
        break;
    case Do::BoardLights:
        o["mode"] = s.verb;
        if (!s.value.empty()) {
            o["colour"] = s.value;
        }
        break;
    case Do::Brightness:
        o["percent"] = s.n;
        break;
    case Do::Open:
        o["app"] = s.verb;
        if (!s.value.empty()) {
            o["tool"] = s.value;
        }
        break;
    case Do::Flashlight:
        o["on"] = s.n != 0;
        break;
    }
}

void shortcut_json(const Shortcut &sc, JsonObject o)
{
    o["id"] = sc.id;
    o["name"] = sc.name;
    o["icon"] = sc.icon;
    o["accent"] = ACCENTS[sc.accent % 4];
    JsonArray steps = o["steps"].to<JsonArray>();
    for (const Step &s : sc.steps) {
        step_json(s, steps.add<JsonObject>());
    }
}

std::string text_of(JsonVariantConst v)
{
    if (v.is<const char *>()) {
        return v.as<const char *>();
    }
    if (v.is<int>()) {
        return std::to_string(v.as<int>());
    }
    if (v.is<float>()) {
        char t[24];
        snprintf(t, sizeof(t), "%g", double(v.as<float>()));
        return t;
    }
    return "";
}

std::string trimmed(std::string s, size_t max_bytes)
{
    while (!s.empty() && isspace(uint8_t(s.back()))) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && isspace(uint8_t(s[start]))) {
        ++start;
    }
    s.erase(0, start);
    for (char &c : s)
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    if (s.size() > max_bytes) {
        size_t cut = max_bytes;
        while (cut > 0 && (uint8_t(s[cut]) & 0xC0) == 0x80) {
            --cut;  // never inside a UTF-8 character
        }
        s.resize(cut);
    }
    return s;
}

bool parse_step(JsonObjectConst o, Step &s, std::string &error)
{
    const int kind = index_of(DO_NAMES, o["do"] | "");
    if (kind < 0) {
        error = "each step's do is one of " + joined(DO_NAMES);
        return false;
    }
    s = Step();
    s.kind = Do(kind);
    switch (s.kind) {
    case Do::Home: {
        s.verb = o["verb"] | (o["action"] | "");
        if (index_of(HOME_VERBS, s.verb) < 0) {
            error = "home verb \"" + s.verb + "\": one of " + joined(HOME_VERBS);
            return false;
        }
        s.target = trimmed(o["target"] | "", 64);
        s.value = trimmed(text_of(o["value"]), 40);
        if ((s.verb == "brightness" || s.verb == "volume") && s.value.empty()) {
            error = s.verb + " needs value, 0 to 100";
            return false;
        }
        if ((s.verb == "color" || s.verb == "scene") && s.value.empty() && (s.verb == "color" || s.target.empty())) {
            error = s.verb == "color" ? "color needs value (a colour name or a hue)" : "scene needs value (the scene's name)";
            return false;
        }
        return true;
    }
    case Do::Wait: {
        const float seconds = o["seconds"].is<float>() ? o["seconds"].as<float>() : o["ms"].is<int>() ? o["ms"].as<int>() / 1000.0f : -1;
        if (seconds < 0.1f || seconds * 1000 > MAX_WAIT_MS) {
            error = "wait needs seconds, 0.1 to 60";
            return false;
        }
        s.n = int(seconds * 1000 + 0.5f);
        return true;
    }
    case Do::Ir:
        s.value = o["key"] | "";
        if (index_of(IR_KEYS, s.value) < 0) {
            error = "ir key: one of " + joined(IR_KEYS);
            return false;
        }
        return true;
    case Do::BoardLights:
        s.verb = o["mode"] | "";
        s.value = trimmed(o["colour"] | (o["color"] | ""), 16);
        if (s.verb.empty() && !s.value.empty()) {
            s.verb = "colour";
        }
        if (index_of(BOARD_MODES, s.verb) < 0) {
            error = "board_lights mode: one of " + joined(BOARD_MODES);
            return false;
        }
        return true;
    case Do::Brightness:
        s.n = o["percent"] | -1;
        if (s.n < 5 || s.n > 100) {
            error = "brightness needs percent, 5 to 100";
            return false;
        }
        return true;
    case Do::Open:
        s.verb = o["app"] | "";
        s.value = o["tool"] | "";
        if (s.verb == "tools" && (s.value == "calculator" || s.value == "interact")) {
            s.verb = s.value == "interact" ? "interaction" : s.value;  // Tools' tabs once, apps now
            s.value.clear();
        }
        if (index_of(APPS, s.verb) < 0) {
            error = "open app: one of " + joined(APPS);
            return false;
        }
        if (!s.value.empty() && index_of(TOOLS, s.value) < 0) {
            error = "open tool: one of " + joined(TOOLS);
            return false;
        }
        return true;
    case Do::Flashlight:
        if (!o["on"].is<bool>()) {
            error = "flashlight needs on (true or false)";
            return false;
        }
        s.n = o["on"].as<bool>() ? 1 : 0;
        return true;
    }
    return false;
}

// An icon and a colour that suit the first step, when none is given.
void defaults_for(Shortcut &sc)
{
    const Step &s = sc.steps.front();
    const bool off = s.verb.find("off") != std::string::npos;
    switch (s.kind) {
    case Do::Home:
        sc.icon = s.verb.rfind("lights", 0) == 0 || s.verb == "brightness" || s.verb == "color" ? (off ? "power" : "bulb")
                  : s.verb == "scene" ? "house" : s.verb.rfind("tv", 0) == 0 ? "tv" : s.verb.rfind("plug", 0) == 0 ? "power" : "speaker";
        sc.accent = off ? 0 : s.verb.rfind("tv", 0) == 0 ? 1 : 2;
        break;
    case Do::Ir:
        sc.icon = "tv", sc.accent = 1;
        break;
    case Do::Wait:
        sc.icon = "clock", sc.accent = 3;
        break;
    default:
        sc.icon = "bulb", sc.accent = 3;
    }
}

bool parse_shortcut(JsonObjectConst o, Shortcut &sc, std::string &error)
{
    sc = Shortcut();
    sc.id = o["id"] | 0u;
    sc.name = trimmed(o["name"] | "", MAX_NAME);
    if (sc.name.empty()) {
        error = "a shortcut needs a name";
        return false;
    }
    JsonArrayConst steps = o["steps"].as<JsonArrayConst>();
    if (steps.size() == 0 || steps.size() > MAX_STEPS) {
        error = "a shortcut has 1 to 10 steps";
        return false;
    }
    for (JsonObjectConst so : steps) {
        Step s;
        if (!parse_step(so, s, error)) {
            error = "step " + std::to_string(sc.steps.size() + 1) + ": " + error;
            return false;
        }
        sc.steps.push_back(std::move(s));
    }
    defaults_for(sc);
    const std::string icon = o["icon"] | "";
    if (!icon.empty()) {
        if (index_of(ICONS, icon) < 0) {
            error = "icon: one of " + joined(ICONS);
            return false;
        }
        sc.icon = ICONS[index_of(ICONS, icon)];
    }
    JsonVariantConst accent = o["accent"];
    if (accent.is<int>()) {
        sc.accent = uint8_t(std::clamp(accent.as<int>(), 0, 3));
    } else if (accent.is<const char *>() && *accent.as<const char *>()) {
        const int a = index_of(ACCENTS, accent.as<const char *>());
        if (a < 0) {
            error = "accent: one of " + joined(ACCENTS);
            return false;
        }
        sc.accent = uint8_t(a);
    }
    return true;
}

// ---- the display lock, from the other tasks ----------------------------------------------
template <typename F> bool on_ui(F f, int timeout_ms = 3000)
{
    if (!bsp_display_lock(timeout_ms)) {
        return false;
    }
    f();
    bsp_display_unlock();
    return true;
}

// Home steps take the hub's names for their targets (a room's id, a device's
// target) when Home can tell, so a near name ("bedroom") keeps working after
// another room comes along. False: Home knows and there is no such thing.
bool resolve_home_steps(Shortcut &sc, std::string &error)
{
    bool ok = true;
    for (size_t i = 0; i < sc.steps.size() && ok; ++i) {
        Step &s = sc.steps[i];
        if (s.kind != Do::Home) {
            continue;
        }
        on_ui([&] {
            if (!home_app_ready()) {
                return;  // checked when it runs
            }
            std::string what, resolved;
            if (!home_app_resolve(s.verb.c_str(), s.target.c_str(), s.value.c_str(), what, resolved))
            {
                error = "step " + std::to_string(i + 1) + ": " + what;
                ok = false;
            } else if (s.verb == "scene" && !resolved.empty())
            {
                s.value = resolved;  // the scene's id
                s.target.clear();
            } else if (!resolved.empty())
            {
                s.target = resolved;
            }
        });
    }
    return ok;
}

// What a step does, in words (home steps as Home puts them, when it can).
std::string describe(const Step &s, bool ask_home)
{
    char t[96];
    switch (s.kind) {
    case Do::Home: {
        std::string what, resolved;
        if (ask_home) {
            on_ui([&] {
                if (home_app_ready()) {
                    home_app_resolve(s.verb.c_str(), s.target.c_str(), s.value.c_str(), what, resolved);
                }
            });
        }
        if (!what.empty()) {
            return what;
        }
        return s.verb + (s.target.empty() ? "" : " " + s.target) + (s.value.empty() ? "" : " " + s.value);
    }
    case Do::Wait:
        snprintf(t, sizeof(t), "Wait %g s", s.n / 1000.0);
        return t;
    case Do::Ir:
        return "TV remote: " + s.value;
    case Do::BoardLights:
        return "Interaction board lights: " + s.verb + (s.value.empty() ? "" : " " + s.value);
    case Do::Brightness:
        snprintf(t, sizeof(t), "Screen brightness %d%%", s.n);
        return t;
    case Do::Open:
        return "Open " + s.verb + (s.value.empty() ? "" : " " + s.value);
    case Do::Flashlight:
        return s.n ? "Flashlight on" : "Flashlight off";
    }
    return "";
}

// ---- storage (LVGL task) -----------------------------------------------------------------
void load()
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = 0;
    std::string text;
    if (nvs_get_blob(h, "data", nullptr, &n) == ESP_OK && n > 1) {
        text.resize(n);
        nvs_get_blob(h, "data", text.data(), &n);
    }
    nvs_close(h);
    JsonDocument doc;
    if (text.empty() || deserializeJson(doc, text)) {
        return;
    }
    std::lock_guard<std::mutex> g(lock);
    for (JsonObjectConst o : doc["list"].as<JsonArrayConst>()) {
        Shortcut sc;
        std::string error;
        if (parse_shortcut(o, sc, error) && sc.id) {
            list.push_back(std::move(sc));
        } else {
            ESP_LOGW(TAG, "dropped a stored shortcut: %s", error.c_str());
        }
    }
    next_id = std::max<uint32_t>(doc["next"] | 1u, 1);
    for (const Shortcut &sc : list) {
        next_id = std::max(next_id, sc.id + 1);
    }
    seeded = doc["seeded"] | false;
    ESP_LOGI(TAG, "%u shortcuts", unsigned(list.size()));
}

std::string stored_json_locked()
{
    JsonDocument doc;
    doc["next"] = next_id;
    doc["seeded"] = seeded;
    JsonArray arr = doc["list"].to<JsonArray>();
    for (const Shortcut &sc : list) {
        shortcut_json(sc, arr.add<JsonObject>());
    }
    std::string text;
    serializeJson(doc, text);
    return text;
}

void save()
{
    std::string text;
    {
        std::lock_guard<std::mutex> g(lock);
        if (!dirty) {
            return;
        }
        dirty = false;
        text = stored_json_locked();
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "data", text.data(), text.size()) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the shortcuts");
    }
    nvs_close(h);
}

void changed_locked()
{
    dirty = true;
    ++revision;
}

// ---- changes (any task) ------------------------------------------------------------------
// Adds sc (id 0) or replaces the one with its id. False: why not, in error.
// resolve: ask Home for its names (not on a small stack: see rpc()).
bool store(Shortcut sc, std::string &error, uint32_t *id_out, bool resolve = true)
{
    if (resolve && !resolve_home_steps(sc, error)) {
        return false;
    }
    std::lock_guard<std::mutex> g(lock);
    if (sc.id) {
        auto it = std::find_if(list.begin(), list.end(), [&](const Shortcut & s) {
            return s.id == sc.id;
        });
        if (it == list.end()) {
            error = "there is no shortcut " + std::to_string(sc.id);
            return false;
        }
        *it = std::move(sc);
        *id_out = it->id;
    } else {
        if (list.size() >= MAX_COUNT) {
            error = "there are already " + std::to_string(MAX_COUNT) + " shortcuts: delete one first";
            return false;
        }
        sc.id = next_id++;
        *id_out = sc.id;
        list.push_back(std::move(sc));
    }
    if (stored_json_locked().size() > MAX_STORED) {
        error = "too much to keep: make it shorter";
        list.erase(std::find_if(list.begin(), list.end(), [&](const Shortcut & s) {
            return s.id == *id_out;
        }));
        return false;
    }
    changed_locked();
    return true;
}

bool remove(uint32_t id, std::string &name)
{
    std::lock_guard<std::mutex> g(lock);
    auto it = std::find_if(list.begin(), list.end(), [id](const Shortcut & s) {
        return s.id == id;
    });
    if (it == list.end()) {
        return false;
    }
    name = it->name;
    list.erase(it);
    changed_locked();
    return true;
}

bool find_copy(uint32_t id, Shortcut &out)
{
    std::lock_guard<std::mutex> g(lock);
    for (const Shortcut &sc : list)
        if (sc.id == id) {
            out = sc;
            return true;
        }
    return false;
}

// A shortcut named like this (exactly, then nearly), for Swifty. 0: none.
uint32_t find_by_name(const std::string &want)
{
    std::lock_guard<std::mutex> g(lock);
    std::string w = want;
    for (char &c : w) {
        c = char(tolower(uint8_t(c)));
    }
    for (int exact = 1; exact >= 0; --exact)
        for (const Shortcut &sc : list) {
            std::string n = sc.name;
            for (char &c : n) {
                c = char(tolower(uint8_t(c)));
            }
            if (exact ? n == w : (!w.empty() && n.find(w) != std::string::npos)) {
                return sc.id;
            }
        }
    return 0;
}

// ---- starters ----------------------------------------------------------------------------
Shortcut lights_off(const std::string &name, const std::string &target)
{
    Shortcut sc;
    sc.name = trimmed(name, MAX_NAME);
    sc.icon = "power";
    sc.accent = 0;
    Step s;
    s.kind = Do::Home;
    s.verb = "lights_off";
    s.target = target;
    sc.steps.push_back(s);
    return sc;
}

// "All lights off" and one per room with lights (the living room and a
// bedroom first), from what Home knows. Call with the display lock held.
std::vector<Shortcut> starters_from_home()
{
    std::vector<Shortcut> out;
    if (!home_app_ready()) {
        return out;
    }
    JsonDocument doc;
    deserializeJson(doc, home_app_choices_json());
    struct Room {
        std::string id, name;
        int rank;
    };
    std::vector<Room> rooms;
    for (JsonObjectConst r : doc["rooms"].as<JsonArrayConst>()) {
        if ((r["lights"] | 0) == 0) {
            continue;
        }
        std::string name = r["name"] | "", low = name;
        for (char &c : low) {
            c = char(tolower(uint8_t(c)));
        }
        const int rank = low.find("living") != std::string::npos ? 0 : low.find("bed") != std::string::npos ? 1 : 2;
        rooms.push_back({r["id"] | "", name, rank});
    }
    std::stable_sort(rooms.begin(), rooms.end(), [](const Room & a, const Room & b) {
        return a.rank < b.rank;
    });
    if (!doc["lights"].as<JsonArrayConst>().size()) {
        return out;
    }
    out.push_back(lights_off("All lights off", "all"));
    for (size_t i = 0; i < rooms.size() && i < 3; ++i) {
        out.push_back(lights_off(rooms[i].name + " lights off", rooms[i].id));
        out.back().icon = "bulb";
    }
    return out;
}

// Adds the starters not there yet (by name). Returns how many.
int add_starters(const std::vector<Shortcut> &starters)
{
    std::lock_guard<std::mutex> g(lock);
    int added = 0;
    for (const Shortcut &s : starters) {
        const bool have = std::any_of(list.begin(), list.end(), [&](const Shortcut & x) {
            return !strcasecmp(x.name.c_str(), s.name.c_str());
        });
        if (have || list.size() >= MAX_COUNT) {
            continue;
        }
        Shortcut sc = s;
        sc.id = next_id++;
        list.push_back(std::move(sc));
        ++added;
    }
    seeded = true;
    changed_locked();
    return added;
}

// ---- feedback (LVGL task) ----------------------------------------------------------------
enum class Feedback : uint8_t { Running, Done, Failed };

struct {
    lv_obj_t *pill = nullptr;
    lv_timer_t *timer = nullptr;
    lv_timer_t *buzz = nullptr;
} T;

void close_toast()
{
    if (T.timer) {
        lv_timer_delete(T.timer);
        T.timer = nullptr;
    }
    if (T.pill) {
        lv_obj_delete(T.pill);
        T.pill = nullptr;
    }
}

// A pill at the foot of the panel, over any app: the shortcut's icon (a tick
// or a cross once it ends), its name and how it went.
void toast(const std::string &name, const std::string &glyph_name, uint8_t accent, Feedback state, const std::string &detail)
{
    close_toast();
    always_on_wake();
    const Accent &a = *ACCENT_STYLES[accent % 4];
    const bool two_lines = !detail.empty();
    const int h = two_lines ? 78 : 60, w = 400, x = (PANEL - w) / 2, y = PANEL - h - 44;
    lv_obj_t *pill = glass(lv_layer_top(), x, y, w, h, h / 2, a);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    T.pill = pill;
    lv_obj_t *badge = box(pill, 10, (h - 44) / 2, 44, 44, 22);
    const uint32_t ring = state == Feedback::Failed ? RED : state == Feedback::Done ? OK : a.glow;
    lv_obj_set_style_bg_color(badge, lv_color_hex(ring), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    const char *glyph = state == Feedback::Failed ? "x" : state == Feedback::Done ? "check" : glyph_name.c_str();
    icon(badge, glyph, 10, 10, 24, state == Feedback::Running ? a.ink : 0x06200F, 2.2f);
    const int tx = 66, tw = w - tx - 22;
    std::string title = name;
    if (state == Feedback::Running) {
        title += "\xE2\x80\xA6";
    }
    lv_obj_t *t = label(pill, title.c_str(), tx, two_lines ? 12 : (h - 24) / 2, tw, TEXT, 18, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (two_lines) {
        lv_obj_t *d = label(pill, detail.c_str(), tx, 40, tw, state == Feedback::Failed ? RED : TEXT2, 14);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_set_style_opa(pill, LV_OPA_TRANSP, 0);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, pill);
    lv_anim_set_values(&an, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&an, 160);
    lv_anim_set_exec_cb(&an, [](void *o, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(o), lv_opa_t(v), 0);
    });
    lv_anim_start(&an);
    if (state != Feedback::Running) {
        T.timer = lv_timer_create([](lv_timer_t *) {
            T.timer = nullptr;  // deleted after this run (repeat count 1)
            if (T.pill) {
                lv_obj_delete(T.pill);
                T.pill = nullptr;
            }
        }, state == Feedback::Failed ? 5000 : 2500, nullptr);
        lv_timer_set_repeat_count(T.timer, 1);
        lv_timer_set_auto_delete(T.timer, true);
    }
}

void toast(const Shortcut &sc, Feedback state, const std::string &detail)
{
    toast(sc.name, sc.icon, sc.accent, state, detail);
}

void buzz(bool failed)
{
    haptics_pulse(HapticPulse::Tap);
    if (failed && !T.buzz) {
        T.buzz = lv_timer_create([](lv_timer_t *) {
            T.buzz = nullptr;
            haptics_pulse(HapticPulse::Tap);
        }, 220, nullptr);
        lv_timer_set_repeat_count(T.buzz, 1);
        lv_timer_set_auto_delete(T.buzz, true);
    }
}

// The home screen's tiles follow the list (LVGL task). Their keys are
// "sc:<id>"; the launcher keeps where each one is (and if it is hidden).
std::string tile_key(uint32_t id)
{
    return "sc:" + std::to_string(id);
}

std::vector<uint32_t> tiles;  // the ids the launcher has (LVGL task)

void tile_tapped(const char *, void *user)
{
    shortcuts_run(uint32_t(uintptr_t(user)));
}

void tiles_publish()
{
    std::vector<Shortcut> copy;
    {
        std::lock_guard<std::mutex> g(lock);
        copy = list;
    }
    for (uint32_t id : tiles) {
        const bool kept = std::any_of(copy.begin(), copy.end(), [id](const Shortcut & s) {
            return s.id == id;
        });
        if (!kept) {
            launcher_shortcut_remove(tile_key(id).c_str());  // deleted: its place goes too
        }
    }
    tiles.clear();
    for (const Shortcut &sc : copy) {
        const std::string key = tile_key(sc.id);
        const LauncherShortcut tile = {key.c_str(), sc.name.c_str(), sc.icon.c_str(), ACCENT_STYLES[sc.accent % 4],
                                       tile_tapped, reinterpret_cast<void *>(uintptr_t(sc.id))
                                      };
        if (launcher_shortcut_set(tile)) {
            tiles.push_back(sc.id);
        }
    }
}

void tile_feedback(uint32_t id, Feedback state)
{
    launcher_shortcut_feedback(tile_key(id).c_str(), state == Feedback::Running ? -1 : state == Feedback::Done ? 1 : 0);
}

// ---- runs (their own task) ---------------------------------------------------------------
struct Outcome {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string message;
};
struct Job {
    uint32_t id;
    bool dry;
    std::shared_ptr<Outcome> outcome;
};
std::deque<Job> jobs;  // under lock
TaskHandle_t runner = nullptr;

struct HomeResult {
    std::atomic<int> state{-1};  // -1 waiting, 0 failed, 1 done
    std::string message;
};

bool run_home(const Step &s, bool dry, std::string &what, std::string &error)
{
    // Home starts a few seconds after a restart and needs the hub's lists.
    bool ready = false, paired = true;
    for (int i = 0; i < 60 && !ready; ++i) {
        on_ui([&] {
            if (H.home_start) {
                H.home_start();
            }
            ready = home_app_ready();
            home_pairing_info_t info;
            home_app_pairing_info(&info);
            paired = info.paired;
        });
        if (!paired) {
            error = "Swifty Home is not paired (open Home to pair).";
            return false;
        }
        if (!ready) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    if (!ready) {
        error = "No answer from Swifty Home.";
        return false;
    }
    auto result = std::make_shared<HomeResult>();
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool started = false;
    const bool locked = on_ui([&] {
        started = home_app_run(s.verb.c_str(), s.target.c_str(), s.value.c_str(), dry,
        [result, me](bool ok, const std::string & message) {
            result->message = message;
            result->state.store(ok ? 1 : 0);
            xTaskNotifyGive(me);
        }, what);
    });
    if (!locked) {
        error = "The screen is busy.";
        return false;
    }
    if (!started) {
        error = what;
        what.clear();
        return false;
    }
    for (int waited = 0; result->state.load() < 0 && waited < 20000; waited += 250) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    }
    const int state = result->state.load();
    if (state != 1) {
        error = state < 0 ? "No answer from Swifty Home." : result->message;
        return false;
    }
    if (dry) {
        ESP_LOGI(TAG, "  %s", result->message.c_str());
    }
    return true;
}

bool run_step(const Step &s, bool dry, std::string &what, std::string &error)
{
    char out[200] = "";
    what = describe(s, false);
    switch (s.kind) {
    case Do::Home:
        return run_home(s, dry, what, error);
    case Do::Wait:
        vTaskDelay(pdMS_TO_TICKS(s.n));
        return true;
    case Do::Ir:
        if (dry) {
            return true;
        }
        if (!tools_board_ir(s.value.c_str(), -1, -1, out, sizeof(out))) {
            error = out;
            return false;
        }
        return true;
    case Do::BoardLights:
        if (dry) {
            return true;
        }
        if (!tools_board_lights(s.verb.c_str(), s.value.c_str(), out, sizeof(out))) {
            error = out;
            return false;
        }
        return true;
    case Do::Brightness:
        on_ui([&] {
            if (H.brightness) {
                H.brightness(s.n);
            }
        });
        return true;
    case Do::Open: {
        bool ok = false;
        on_ui([&] {
            ok = H.open_app && H.open_app(s.verb.c_str(), s.value.c_str());
        });
        if (!ok) {
            error = "could not open " + s.verb;
        }
        return ok;
    }
    case Do::Flashlight: {
        if (dry) {
            return true;
        }
        bool ok = false;
        on_ui([&] {
            ok = H.flashlight && H.flashlight(s.n != 0);
        });
        if (!ok) {
            error = "no camera module";
        }
        return ok;
    }
    }
    return false;
}

void run_one(const Job &job)
{
    Shortcut sc;
    if (!find_copy(job.id, sc)) {
        if (job.outcome) {
            job.outcome->message = "That shortcut is gone.";
            job.outcome->done.store(true);
        }
        return;
    }
    const bool dry = job.dry || dry_all.load();
    ESP_LOGI(TAG, "running %u \"%s\"%s", unsigned(sc.id), sc.name.c_str(), dry ? " (dry run)" : "");
    on_ui([&] {
        tile_feedback(sc.id, Feedback::Running);
        toast(sc, Feedback::Running, dry ? "Dry run: nothing is sent" : "");
    });
    bool ok = true;
    std::string message;
    for (size_t i = 0; i < sc.steps.size(); ++i) {
        std::string what, error;
        const bool step_ok = run_step(sc.steps[i], dry, what, error);
        ESP_LOGI(TAG, "  step %u: %s: %s%s", unsigned(i + 1), what.c_str(), step_ok ? "ok" : "failed: ", error.c_str());
        if (!step_ok) {
            ok = false;
            message = (sc.steps.size() > 1 ? "Step " + std::to_string(i + 1) + ": " : std::string()) + error;
            break;
        }
    }
    if (ok) {
        message = dry ? "Dry run: nothing was sent" : sc.steps.size() > 1 ? "Done, " + std::to_string(sc.steps.size()) + " steps" : "Done";
    }
    on_ui([&] {
        tile_feedback(sc.id, ok ? Feedback::Done : Feedback::Failed);
        toast(sc, ok ? Feedback::Done : Feedback::Failed, ok && sc.steps.size() == 1 && !dry ? "" : message);
        buzz(!ok);
    });
    {
        std::lock_guard<std::mutex> g(lock);
        last_result = sc.name + ": " + (ok ? "ok" : "failed") + " - " + message;
    }
    if (job.outcome) {
        job.outcome->ok = ok;
        job.outcome->message = message;
        job.outcome->done.store(true);
    }
}

void runner_task(void *)
{
    for (;;) {
        Job job;
        for (;;) {
            {
                std::lock_guard<std::mutex> g(lock);
                if (!jobs.empty()) {
                    job = jobs.front();
                    jobs.pop_front();
                    break;
                }
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        run_one(job);
    }
}

bool queue_run(uint32_t id, bool dry, std::shared_ptr<Outcome> outcome)
{
    {
        std::lock_guard<std::mutex> g(lock);
        const bool known = std::any_of(list.begin(), list.end(), [id](const Shortcut & s) {
            return s.id == id;
        });
        if (!known || !runner || jobs.size() >= MAX_QUEUE) {
            return false;
        }
        jobs.push_back({id, dry, std::move(outcome)});
    }
    xTaskNotifyGive(runner);
    return true;
}

// ---- the LVGL timer: save, tiles, starters -----------------------------------------------
uint32_t published = 0;

void check(lv_timer_t *)
{
    save();
    uint32_t rev;
    bool offer;
    {
        std::lock_guard<std::mutex> g(lock);
        rev = revision;
        offer = !seeded;
    }
    // The first time Home has its lists: the starters, unless there are
    // shortcuts already (made before Home answered).
    if (starters_wanted.exchange(false)) {
        const int added = add_starters(starters_from_home());
        ESP_LOGI(TAG, "starters asked for: %d added", added);
    }
    if (offer && home_app_ready()) {
        bool empty;
        {
            std::lock_guard<std::mutex> g(lock);
            empty = list.empty();
        }
        const int added = add_starters(empty ? starters_from_home() : std::vector<Shortcut>());
        ESP_LOGI(TAG, "starters: %d added", added);
        save();
    }
    if (rev != published) {
        published = rev;
        tiles_publish();
    }
}

// ---- text for Swifty and RPC 55 ----------------------------------------------------------
std::string list_text(bool ask_home)
{
    std::vector<Shortcut> copy;
    {
        std::lock_guard<std::mutex> g(lock);
        copy = list;
    }
    if (copy.empty()) {
        return "No shortcuts.";
    }
    std::string text;
    for (const Shortcut &sc : copy) {
        text += "Shortcut " + std::to_string(sc.id) + ": " + sc.name + " (" + sc.icon + ", " + ACCENTS[sc.accent % 4] + "): ";
        for (size_t i = 0; i < sc.steps.size(); ++i) {
            text += (i ? ", then " : "") + describe(sc.steps[i], ask_home);
        }
        text += "\n";
    }
    return text;
}

// ---- Swifty's tools (its agent task) -----------------------------------------------------
void add_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    deserializeJson(a, args);
    Shortcut sc;
    std::string error;
    if (!parse_shortcut(a.as<JsonObjectConst>(), sc, error)) {
        snprintf(out, cap, "Not made: %s.", error.c_str());
        return;
    }
    const bool replace = sc.id != 0;
    uint32_t id = 0;
    if (!store(sc, error, &id)) {
        snprintf(out, cap, "Not made: %s.", error.c_str());
        return;
    }
    Shortcut saved;
    find_copy(id, saved);
    std::string steps;
    for (size_t i = 0; i < saved.steps.size(); ++i) {
        steps += (i ? ", then " : "") + describe(saved.steps[i], true);
    }
    snprintf(out, cap, "Shortcut %u \"%s\" %s: %s. It is a tile on the home screen.", unsigned(id), saved.name.c_str(),
             replace ? "changed" : "made", steps.c_str());
}

void list_tool(const char *, char *out, size_t cap)
{
    strlcpy(out, list_text(true).c_str(), cap);
}

void run_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    deserializeJson(a, args);
    uint32_t id = a["id"] | 0u;
    if (!id && a["name"].is<const char *>()) {
        id = find_by_name(a["name"].as<const char *>());
    }
    Shortcut sc;
    if (!id || !find_copy(id, sc)) {
        snprintf(out, cap, "No such shortcut (list_shortcuts shows them).");
        return;
    }
    auto outcome = std::make_shared<Outcome>();
    if (!queue_run(id, false, outcome)) {
        snprintf(out, cap, "Too many shortcuts are running; try again in a moment.");
        return;
    }
    // Waits for the end (steps can wait up to a minute each).
    for (int waited = 0; !outcome->done.load() && waited < 90000; waited += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!outcome->done.load()) {
        snprintf(out, cap, "\"%s\" is still running.", sc.name.c_str());
    } else {
        snprintf(out, cap, "\"%s\" %s: %s.", sc.name.c_str(), outcome->ok ? "ran" : "failed", outcome->message.c_str());
    }
}

void delete_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    deserializeJson(a, args);
    const uint32_t id = a["id"] | 0u;
    std::string name;
    if (!remove(id, name)) {
        snprintf(out, cap, "There is no shortcut %u (list_shortcuts shows them).", unsigned(id));
        return;
    }
    snprintf(out, cap, "Deleted shortcut %u, \"%s\", and its tile.", unsigned(id), name.c_str());
}

void register_tools()
{
    static const std::string add_schema =
        std::string(R"({"type":"object","properties":{)"
                    R"("id":{"type":"integer","description":"Change this shortcut instead of making a new one."},)"
                    R"("name":{"type":"string","description":"Short, on the tile: e.g. Bedroom lights off."},)"
                    R"("icon":{"type":"string","enum":[)") +
    [] {
        std::string s;
        for (const char *i : ICONS) {
            s += (s.empty() ? "\"" : ",\"") + std::string(i) + "\"";
        }
        return s;
    }() +
    R"(]},"accent":{"type":"string","enum":["violet","cyan","amber","green"]},)"
    R"("steps":{"type":"array","description":"Run in order, 1 to 10.","items":{"type":"object","properties":{)"
    R"("do":{"type":"string","enum":["home","wait","ir","board_lights","brightness","open","flashlight"]},)"
    R"("verb":{"type":"string","description":"home: lights_on, lights_off, lights_toggle, brightness, color, scene, plug_on, plug_off, plug_toggle, tv_on, tv_off, tv_toggle, volume, play, pause, mute, unmute"},)"
    R"("target":{"type":"string","description":"home: a room, a light or a plug by name, or all"},)"
    R"("value":{"type":"string","description":"home: brightness or volume 0-100, a colour, a scene's name"},)"
    R"("seconds":{"type":"number","description":"wait: 0.1-60"},)"
    R"("key":{"type":"string","description":"ir: an LG TV key: power, volume_up, volume_down, mute, input, ok, up, down, left, right, back, home, channel_up or channel_down"},)"
    R"("mode":{"type":"string","description":"board_lights: off, react, rainbow, night or colour"},)"
    R"("colour":{"type":"string","description":"board_lights: a colour name or #RRGGBB"},)"
    R"("percent":{"type":"integer","description":"brightness: the screen, 5-100"},)"
    R"("app":{"type":"string","description":"open: launcher, glimdock, home, camera, swifty, files, tools, gallery, webcam, interaction or calculator"},)"
    R"("tool":{"type":"string","description":"open tools: torch, level or compass"},)"
    R"("on":{"type":"boolean","description":"flashlight"}},"required":["do"]}}},"required":["name","steps"]})";
    swifty_app_add_tool("add_shortcut",
                        "Make a one-tap shortcut: a tile on the board's home screen (like Shortcuts on a phone) that runs "
                        "steps in order: Swifty Home commands (home: home_control's actions, plus lights_toggle, "
                        "plug_toggle and tv_toggle), waits, TV remote keys (ir, from the Interaction board), the board's "
                        "lights, the screen's brightness, opening an app, the flashlight. Use home_state for the rooms' "
                        "names. With id it changes that shortcut.",
                        add_schema.c_str(), add_tool);
    swifty_app_add_tool("list_shortcuts", "The home screen's shortcuts: their numbers (ids, kept when others are deleted), names and steps.",
                        R"({"type":"object","properties":{},"required":[]})", list_tool);
    swifty_app_add_tool("run_shortcut",
                        "Run a shortcut now, as a tap on its tile does, and say how it went. It switches real devices.",
                        R"({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string","description":"Instead of id"}},"required":[]})",
                        run_tool);
    swifty_app_add_tool("delete_shortcut", "Delete a shortcut and its tile, by its number (from list_shortcuts). Ask first.",
                        R"({"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]})", delete_tool);
}

// ---- web settings and RPC 55 (their tasks) -----------------------------------------------
template <size_t N> void names_json(JsonArray a, const char *const(&names)[N])
{
    for (const char *n : names) {
        a.add(n);
    }
}

// light: from RPC 55, whose task has a small stack: nothing that asks Home
// (names are then resolved when the shortcut runs; the starters wait for the
// LVGL timer).

std::string apply(const char *json, size_t length, bool *ok_out, bool light)
{
    JsonDocument in, out;
    *ok_out = false;
    auto fail = [&](const std::string & why) {
        JsonDocument e;
        e["error"] = why;
        std::string text;
        serializeJson(e, text);
        return text;
    };
    if (deserializeJson(in, json, length) || !in.is<JsonObject>()) {
        return fail("expected a JSON object");
    }
    const std::string op = in["op"] | "";
    std::string error;
    if (op == "save") {
        Shortcut sc;
        uint32_t id = 0;
        if (!parse_shortcut(in["shortcut"].as<JsonObjectConst>(), sc, error) || !store(sc, error, &id, !light)) {
            return fail(error);
        }
        out["id"] = id;
    } else if (op == "delete") {
        std::string name;
        if (!remove(in["id"] | 0u, name)) {
            return fail("no such shortcut");
        }
    } else if (op == "run") {
        if (!queue_run(in["id"] | 0u, in["dry_run"] | false, nullptr)) {
            return fail("no such shortcut, or too many running");
        }
    } else if (op == "move") {
        std::lock_guard<std::mutex> g(lock);
        const uint32_t id = in["id"] | 0u;
        auto it = std::find_if(list.begin(), list.end(), [id](const Shortcut & s) {
            return s.id == id;
        });
        if (it == list.end()) {
            return fail("no such shortcut");
        }
        const size_t to = std::min<size_t>(size_t(std::max(0, in["to"] | 0)), list.size() - 1);
        Shortcut sc = std::move(*it);
        list.erase(it);
        list.insert(list.begin() + long(to), std::move(sc));
        changed_locked();
    } else if (op == "starters" && light) {
        starters_wanted.store(true);
        out["added"] = "on the board's next check";
    } else if (op == "starters") {
        std::vector<Shortcut> starters;
        on_ui([&] {
            starters = starters_from_home();
        });
        if (starters.empty()) {
            return fail("Swifty Home has not answered yet (or has no lights)");
        }
        out["added"] = add_starters(starters);
    } else if (op == "editor") {
        // Opens the board's editor (a test hook, as RPC 30 opens apps).
        if (!on_ui([] { lv_async_call([](void *)
    {
        shortcuts_editor_open();
        }, nullptr);
                      }, 1000)) {
            return fail("the screen is busy");
        }
    } else if (op == "dry_run") {
        dry_all.store(in["on"] | true);
        out["dry_run"] = dry_all.load();
    } else {
        return fail("op: save, delete, run, move, starters, editor or dry_run");
    }
    out["reply"] = "ok";
    *ok_out = true;
    std::string text;
    serializeJson(out, text);
    return text;
}

// Payload: a JSON change as Web settings sends (see shortcuts.h), or nothing
// (or {"op":"list"}) for the list. The reply is text.
esp_err_t rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t cap, size_t *size, void *)
{
    std::string reply;
    JsonDocument op;
    const bool listing = request->payload_size == 0 ||
                         (!deserializeJson(op, reinterpret_cast<const char *>(request->payload), request->payload_size) &&
                          op["op"] == "list");
    if (listing) {
        reply = list_text(false);
        std::lock_guard<std::mutex> g(lock);
        if (!last_result.empty()) {
            reply += "Last run: " + last_result + "\n";
        }
        if (dry_all.load()) {
            reply += "Every run is a dry run.\n";
        }
    } else {
        bool ok;
        reply = apply(reinterpret_cast<const char *>(request->payload), request->payload_size, &ok, true);
    }
    *size = std::min(cap, reply.size());
    memcpy(out, reply.data(), *size);
    return ESP_OK;
}

// ---- the board's own editor (LVGL task) --------------------------------------------------
// A full-panel sheet over any app (the top layer): the shortcuts, each one's
// steps with Run, Test (a dry run) and Delete, and New: lights off, on or
// toggled for all lights or a room, or a scene, in two taps without a
// keyboard. More steps are made in Web settings or by Swifty.
enum class EditorPage : uint8_t { List, Detail, New };
struct {
    lv_obj_t *root = nullptr, *page = nullptr, *targets = nullptr;
    EditorPage go_page = EditorPage::List;
    uint32_t go_id = 0, delete_armed = 0;
    int kind = 0;  // New: 0 lights off, 1 on, 2 toggle, 3 a scene
} E;

void editor_show(EditorPage page, uint32_t id);

// Pages change after the tap that asked (the tapped object is on the old page).
void editor_go(EditorPage page, uint32_t id = 0)
{
    E.go_page = page;
    E.go_id = id;
    lv_async_call([](void *) {
        if (E.root) {
            editor_show(E.go_page, E.go_id);
        }
    }, nullptr);
}

// What a step does, in words, from here (the LVGL task may ask Home itself).
std::string describe_here(const Step &s)
{
    if (s.kind == Do::Home && home_app_ready()) {
        std::string what, resolved;
        home_app_resolve(s.verb.c_str(), s.target.c_str(), s.value.c_str(), what, resolved);
        if (!what.empty()) {
            return what;
        }
    }
    return describe(s, false);
}

std::string steps_text(const Shortcut &sc, const char *joint)
{
    std::string text;
    for (size_t i = 0; i < sc.steps.size(); ++i) {
        text += (i ? joint : "") + describe_here(sc.steps[i]);
    }
    return text;
}

lv_obj_t *round_button(lv_obj_t *parent, int x, int y, const char *glyph, lv_event_cb_t cb, void *user = nullptr)
{
    lv_obj_t *b = box(parent, x, y, 48, 48, 24);
    lv_obj_set_style_bg_color(b, lv_color_hex(FIELD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    press_fx(b);
    icon(b, glyph, 12, 12, 24, TEXT, 2.2f);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

lv_obj_t *scroller(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *s = box(parent, 40, y, PANEL - 80, h);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
    return s;
}

// A row in a list: a badge with a glyph, a title and a line under it.
lv_obj_t *row(lv_obj_t *list, int y, const char *glyph, const Accent &a, const std::string &title, const std::string &sub,
              lv_event_cb_t cb, void *user)
{
    const int w = PANEL - 80, h = sub.empty() ? 56 : 66;
    lv_obj_t *r = glass(list, 0, y, w, h, 20, a);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    press_fx(r);
    lv_obj_t *badge = box(r, 10, (h - 40) / 2, 40, 40, 20);
    lv_obj_set_style_bg_color(badge, lv_color_hex(a.glow), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    icon(badge, glyph, 9, 9, 22, a.ink, 2.2f);
    lv_obj_t *t = label(r, title.c_str(), 62, sub.empty() ? (h - 22) / 2 : 11, w - 76, TEXT, 17, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (!sub.empty()) {
        lv_obj_t *d = label(r, sub.c_str(), 62, 37, w - 76, TEXT3, 13);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
    return r;
}

lv_obj_t *page_frame(const char *title, bool back)
{
    if (E.page) {
        lv_obj_delete(E.page);
    }
    E.targets = nullptr;
    E.page = box(E.root, 0, 0, PANEL, PANEL);
    lv_obj_t *t = label(E.page, title, 48, 44, back ? 300 : 320, TEXT, 26, true);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    round_button(E.page, PANEL - 96, 36, back ? "back" : "x", [](lv_event_t *e) {
        const bool back = lv_event_get_user_data(e) != nullptr;
        if (back) {
            editor_go(EditorPage::List);
        } else {
            lv_async_call([](void *) {
                if (E.root) {
                    lv_obj_delete(E.root);
                    E.root = E.page = E.targets = nullptr;
                }
            }, nullptr);
        }
    }, back ? reinterpret_cast<void *>(1) : nullptr);
    return E.page;
}

void list_page()
{
    lv_obj_t *p = page_frame("Shortcuts", false);
    std::vector<Shortcut> copy;
    {
        std::lock_guard<std::mutex> g(lock);
        copy = list;
    }
    lv_obj_t *s = scroller(p, 100, 286);
    int y = 0;
    for (const Shortcut &sc : copy) {
        row(s, y, sc.icon.c_str(), *ACCENT_STYLES[sc.accent % 4], sc.name, steps_text(sc, ", then "), [](lv_event_t *e) {
            editor_go(EditorPage::Detail, uint32_t(uintptr_t(lv_event_get_user_data(e))));
        }, reinterpret_cast<void *>(uintptr_t(sc.id)));
        y += 76;
    }
    if (copy.empty()) {
        lv_obj_t *n = label(s, "No shortcuts yet. New makes one; Web settings and Swifty can make longer ones.", 8, 8,
                            PANEL - 96, TEXT2, 16);
        wrap_label(n);
    }
    lv_obj_t *add = fill_button(p, 96, 400, PANEL - 192, 50, "New shortcut", VIOLET, 17);
    lv_obj_add_event_cb(add, [](lv_event_t *) {
        editor_go(EditorPage::New);
    }, LV_EVENT_CLICKED, nullptr);
    if (copy.size() >= MAX_COUNT) {
        lv_obj_add_state(add, LV_STATE_DISABLED);
    }
    stagger(s, 0, 24);
}

void detail_page(uint32_t id)
{
    Shortcut sc;
    if (!find_copy(id, sc)) {
        return list_page();
    }
    lv_obj_t *p = page_frame(sc.name.c_str(), true);
    lv_obj_t *steps = label(p, steps_text(sc, "\n").c_str(), 48, 104, PANEL - 96, TEXT2, 17);
    wrap_label(steps);
    lv_obj_set_height(steps, 150);
    void *user = reinterpret_cast<void *>(uintptr_t(id));
    lv_obj_t *run = fill_button(p, 48, 280, 184, 52, "Run", *ACCENT_STYLES[sc.accent % 4], 17);
    lv_obj_add_event_cb(run, [](lv_event_t *e) {
        shortcuts_run(uint32_t(uintptr_t(lv_event_get_user_data(e))));
    }, LV_EVENT_CLICKED, user);
    lv_obj_t *test = field_button(p, 248, 280, 184, 52, "Test (dry run)", 16);
    lv_obj_add_event_cb(test, [](lv_event_t *e) {
        shortcuts_run(uint32_t(uintptr_t(lv_event_get_user_data(e))), true);
    }, LV_EVENT_CLICKED, user);
    E.delete_armed = 0;
    lv_obj_t *del = field_button(p, 48, 348, PANEL - 96, 52, "Delete", 16, RED);
    lv_obj_add_event_cb(del, [](lv_event_t *e) {
        const uint32_t id = uint32_t(uintptr_t(lv_event_get_user_data(e)));
        lv_obj_t *button = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
        if (E.delete_armed != id) {
            E.delete_armed = id;  // a second tap deletes
            lv_obj_t *text = lv_obj_get_child(button, 0);
            if (text) {
                set_text(text, "Tap again to delete it and its tile");
            }
            return;
        }
        std::string name;
        if (remove(id, name)) {
            toast(name, "x", 0, Feedback::Done, "Deleted, with its tile");
        }
        editor_go(EditorPage::List);
    }, LV_EVENT_CLICKED, user);
    label(p, "Edit its steps in Web settings, or ask Swifty.", 48, 416, PANEL - 96, TEXT3, 13, false, LV_TEXT_ALIGN_CENTER);
}

// New: what (lights off, on, toggled, a scene), then which.
const char *const KIND_LABELS[] = {"Off", "On", "Toggle", "Scene"};

void fill_targets();

void target_tapped(lv_event_t *e)
{
    // The row's user data: "<target>\x1f<title>" (target_keys holds it).
    const char *packed = static_cast<const char *>(lv_event_get_user_data(e));
    const char *sep = strchr(packed, '\x1f');
    if (!sep) {
        return;
    }
    const std::string target(packed, sep), title(sep + 1);
    Shortcut sc;
    Step s;
    s.kind = Do::Home;
    if (E.kind == 3) {
        s.verb = "scene";
        s.value = target;
        sc.name = title;
        sc.icon = "house", sc.accent = 1;
    } else {
        s.verb = E.kind == 0 ? "lights_off" : E.kind == 1 ? "lights_on" : "lights_toggle";
        s.target = target;
        sc.name = title + (E.kind == 0 ? " lights off" : E.kind == 1 ? " lights on" : " lights");
        if (target == "all") {
            sc.name = E.kind == 0 ? "All lights off" : E.kind == 1 ? "All lights on" : "All lights";
        }
        sc.icon = E.kind == 0 ? "power" : "bulb";
        sc.accent = E.kind == 0 ? 0 : 2;
    }
    sc.name = trimmed(sc.name, MAX_NAME);
    sc.steps.push_back(s);
    {
        std::lock_guard<std::mutex> g(lock);
        for (const Shortcut &x : list)
            if (!strcasecmp(x.name.c_str(), sc.name.c_str())) {
                toast(sc, Feedback::Failed, "There is one with that name");
                return;
            }
    }
    std::string error;
    uint32_t id = 0;
    if (!store(sc, error, &id, false)) {  // the targets came from Home: nothing to resolve
        toast(sc, Feedback::Failed, error);
        return;
    }
    toast(sc, Feedback::Done, "Added to the home screen");
    editor_go(EditorPage::List);
}

std::vector<std::string> target_keys;  // the rows' user data (kept while they are shown)

void fill_targets()
{
    if (!E.targets) {
        return;
    }
    lv_obj_clean(E.targets);
    target_keys.clear();
    std::vector<std::pair<std::string, std::string>> choices;  // target, title
    std::string note;
    if (!home_app_ready()) {
        note = "Swifty Home has not answered yet (pair it in Home).";
    } else {
        JsonDocument doc;
        deserializeJson(doc, home_app_choices_json());
        if (E.kind == 3) {
            for (JsonObjectConst sc : doc["scenes"].as<JsonArrayConst>()) {
                choices.push_back({sc["id"] | "", sc["name"] | (sc["id"] | "")});
            }
            if (choices.empty()) {
                note = "Swifty Home has no scenes.";
            }
        } else {
            if (doc["lights"].as<JsonArrayConst>().size()) {
                choices.push_back({"all", "All lights"});
            }
            for (JsonObjectConst r : doc["rooms"].as<JsonArrayConst>())
                if ((r["lights"] | 0) > 0) {
                    choices.push_back({r["id"] | "", r["name"] | ""});
                }
            if (choices.empty()) {
                note = "Swifty Home has no lights.";
            }
        }
    }
    target_keys.reserve(choices.size());
    int y = 0;
    const Accent &a = E.kind == 3 ? CYAN : E.kind == 0 ? VIOLET : AMBER;
    const char *glyph = E.kind == 3 ? "house" : E.kind == 0 ? "power" : "bulb";
    for (const auto &[target, title] : choices) {
        target_keys.push_back(target + '\x1f' + title);
        row(E.targets, y, glyph, a, title, "", target_tapped, const_cast<char *>(target_keys.back().c_str()));
        y += 64;
    }
    if (!note.empty()) {
        lv_obj_t *n = label(E.targets, note.c_str(), 8, 8, PANEL - 96, TEXT2, 16);
        wrap_label(n);
    }
}

void new_page()
{
    lv_obj_t *p = page_frame("New shortcut", true);
    lv_obj_t *c = capsule(p, 40, 102, PANEL - 80, 46, KIND_LABELS, 4, E.kind, VIOLET, 16);
    lv_obj_add_event_cb(c, [](lv_event_t *e) {
        E.kind = capsule_selected(static_cast<lv_obj_t *>(lv_event_get_current_target(e)));
        fill_targets();
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    E.targets = scroller(p, 164, 238);
    fill_targets();
    label(p, "Longer ones: Web settings, or ask Swifty.", 48, 416, PANEL - 96, TEXT3, 13, false, LV_TEXT_ALIGN_CENTER);
}

void editor_show(EditorPage page, uint32_t id)
{
    if (page == EditorPage::Detail) {
        detail_page(id);
    } else if (page == EditorPage::New) {
        new_page();
    } else {
        list_page();
    }
    if (T.pill) {
        lv_obj_move_foreground(T.pill);  // a toast stays over the sheet
    }
}
}  // namespace

void shortcuts_start(const ShortcutHooks &hooks)
{
    H = hooks;
    load();
    register_tools();
    esp_iris_rpc_register(0x4744, 55, rpc, nullptr);
    // Runs wait and block (the Interaction board), never on the LVGL task.
    // Below LVGL's drawing (3); HTTP is Home's own worker, and nothing here
    // writes flash, so the stack can be in PSRAM.
    xTaskCreatePinnedToCoreWithCaps(runner_task, "shortcuts", 10240, nullptr, 2, &runner, tskNO_AFFINITY,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_timer_create(check, 1000, nullptr);
}

void shortcuts_editor_open()
{
    if (E.root) {
        return;
    }
    E.root = box(lv_layer_top(), 0, 0, PANEL, PANEL);
    lv_obj_set_style_bg_color(E.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(E.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(E.root, LV_OBJ_FLAG_CLICKABLE);  // takes the taps meant for the app under it
    E.kind = 0;
    editor_show(EditorPage::List, 0);
}

bool shortcuts_run(uint32_t id, bool dry_run)
{
    return queue_run(id, dry_run, nullptr);
}

std::string shortcuts_web_list()
{
    JsonDocument doc;
    {
        std::lock_guard<std::mutex> g(lock);
        JsonArray arr = doc["shortcuts"].to<JsonArray>();
        for (const Shortcut &sc : list) {
            shortcut_json(sc, arr.add<JsonObject>());
        }
        doc["last"] = last_result;
    }
    doc["dry_run"] = dry_all.load();
    std::string choices = "{}";
    on_ui([&] {
        choices = home_app_choices_json();
    });
    JsonDocument home;
    deserializeJson(home, choices);
    doc["home"] = home;
    names_json(doc["verbs"].to<JsonArray>(), HOME_VERBS);
    names_json(doc["ir_keys"].to<JsonArray>(), IR_KEYS);
    names_json(doc["board_modes"].to<JsonArray>(), BOARD_MODES);
    names_json(doc["apps"].to<JsonArray>(), APPS);
    names_json(doc["tools"].to<JsonArray>(), TOOLS);
    names_json(doc["icons"].to<JsonArray>(), ICONS);
    names_json(doc["accents"].to<JsonArray>(), ACCENTS);
    doc["max"] = MAX_COUNT;
    std::string text;
    serializeJson(doc, text);
    return text;
}

std::string shortcuts_web_apply(const char *json, size_t length, bool *ok)
{
    return apply(json, length, ok, false);
}
