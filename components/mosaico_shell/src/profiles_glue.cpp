// SPDX-License-Identifier: Apache-2.0
// See profiles_glue.h. ESP-Iris holds 32 handlers and the board uses them all:
// the RPCs are sub-commands of RPC 54 (iris_ext.h).
//   54 · 60  the profiles as JSON, a page at a time; or a change (JSON) with
//            the rights of a trusted computer (USB, or a paired one)
//   54 · 61  recovery, over USB only: a new PIN, wrong tries forgotten, or
//            every profile gone (as before profiles)
#include "profiles_glue.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>

#include "ArduinoJson.h"
#include "bsp/esp_mosaico.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "face_id.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iris_ext.h"
#include "launcher_apps.h"
#include "lvgl.h"
#include "mbedtls/platform_util.h"
#include "profiles.h"
#include "profiles_ui.h"
#include "shell_internal.h"

namespace {
const char *TAG = "profiles";
constexpr int SLOTS = PROFILES_MAX + 1;
constexpr uint32_t WORKER_STACK = 7 * 1024;  // PBKDF2, ArduinoJson and an NVS write: internal (flash)

ProfilesUiApp apps[PROFILE_APPS];  // the launcher's apps (fewer than the slots)
void (*apply_hook)() = nullptr;

int slot_of(const LauncherApp &a)
{
    return a.open >= 0 ? a.open : PROFILE_APP_SETTINGS;
}

// ---- work off the LVGL task ------------------------------------------------------------------
// A short-lived task with an internal stack (the hash runs ~0.5 s; NVS writes
// need internal RAM), ended when done: no stack kept in between.
void run_task(std::function<void()> *fn)
{
    if (xTaskCreatePinnedToCore([](void *arg) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(arg));
        (*f)();
        f.reset();
        vTaskDelete(nullptr);
    }, "profiles", WORKER_STACK, fn, 2, nullptr, tskNO_AFFINITY) != pdPASS) {
        ESP_LOGW(TAG, "no worker task; running inline");
        (*fn)();
        delete fn;
    }
}

void ui_background(void (*work)(void *), void (*done)(void *), void *arg)
{
    run_task(new std::function<void()>([work, done, arg] {
        work(arg);
        // Back on the LVGL task for the result.
        while (!bsp_display_lock(1000)) {
        }
        struct Back {
            void (*done)(void *);
            void *arg;
        };
        lv_async_call([](void *b)
        {
            std::unique_ptr<Back> back(static_cast<Back *>(b));
            back->done(back->arg);
        }, new Back{done, arg});
        bsp_display_unlock();
    }));
}

// ---- the shell's response to a change, after it ----------------------------------------------
std::atomic<bool> apply_pending{false};
void changed(void *)
{
    if (apply_pending.exchange(true)) {
        return;
    }
    lv_async_call([](void *) {
        apply_pending = false;
        if (apply_hook) {
            apply_hook();
        }
    }, nullptr);
}

// ---- JSON ----------------------------------------------------------------------------------
// Apps as one digit per slot (0 hidden, 1 read-only, 2 full): slot = ShellApp
// value, 31 Settings. Compact enough for an ESP-Iris reply.
std::string apps_text(const profile_t &p)
{
    std::string s(PROFILE_APPS, '0');
    for (int i = 0; i < PROFILE_APPS; ++i) {
        s[size_t(i)] = char('0' + p.apps[i]);
    }
    return s;
}

void profile_json(const profile_t &p, JsonObject o)
{
    o["id"] = p.id;
    o["name"] = p.name;
    o["color"] = p.color;
    o["signin"] = profiles_signin_key(profile_signin_t(p.signin));
    o["elevated"] = p.elevated;
    o["pin"] = p.has_pin;
    o["face"] = profiles_face_enrolled(p.id);
    o["caps"] = p.caps;
    o["apps"] = apps_text(p);
    o["wait_s"] = profiles_wait_s(p.id);
    o["tries_left"] = profiles_tries_left(p.id);
}

void header_json(JsonDocument &doc)
{
    profiles_settings_t s{};
    profiles_get_settings(&s);
    doc["configured"] = profiles_configured();
    doc["current"] = profiles_current();
    doc["elevated"] = profiles_elevated();
    doc["face_available"] = face_id_available();
    doc["default"] = s.default_id;
    doc["lock_s"] = s.lock_s;
    doc["elevated_lock_s"] = s.elevated_lock_s;
}

bool parse_signin(const char *key, uint8_t *out)
{
    for (int i = 0; i <= PROFILE_SIGNIN_FACE_PIN; ++i)
        if (key && !strcmp(key, profiles_signin_key(profile_signin_t(i)))) {
            *out = uint8_t(i);
            return true;
        }
    return false;
}

// A profile from JSON over the stored one (or a new one's defaults): only
// the fields given change.
const char *parse_profile(JsonObjectConst o, profile_t &p)
{
    const int id = o["id"] | int(PROFILE_NEW);
    if (id == PROFILE_NEW || !profiles_get(uint8_t(id), &p)) {
        if (id != PROFILE_NEW) {
            return "no such profile";
        }
        profiles_defaults(PROFILE_NEW, &p);
        p.id = PROFILE_NEW;
    }
    if (o["name"].is<const char *>()) {
        strlcpy(p.name, o["name"].as<const char *>(), sizeof(p.name));
    }
    if (o["color"].is<int>()) {
        p.color = uint8_t(o["color"].as<int>());
    }
    if (o["signin"].is<const char *>() && !parse_signin(o["signin"].as<const char *>(), &p.signin)) {
        return "signin is none, pin, face or face_pin";
    }
    if (o["elevated"].is<bool>()) {
        p.elevated = o["elevated"].as<bool>();
    }
    if (o["caps"].is<uint32_t>()) {
        p.caps = o["caps"].as<uint32_t>();
    } else if (o["caps"].is<JsonArrayConst>()) {
        p.caps = 0;
        for (JsonVariantConst v : o["caps"].as<JsonArrayConst>()) {
            bool known = false;
            for (int c = 0; c < PROFILE_CAP_COUNT; ++c)
                if (v.is<const char *>() && !strcmp(v.as<const char *>(), profiles_cap_key(profile_cap_t(c)))) {
                    p.caps |= PROFILE_CAP(c);
                    known = true;
                }
            if (!known) {
                return "unknown capability";
            }
        }
    }
    if (o["apps"].is<const char *>()) {
        const char *digits = o["apps"].as<const char *>();
        for (int i = 0; i < PROFILE_APPS && digits[i]; ++i) {
            if (digits[i] < '0' || digits[i] > '2') {
                return "apps: one digit per app, 0 hidden, 1 read-only, 2 full";
            }
            p.apps[i] = uint8_t(digits[i] - '0');
        }
    } else if (o["apps"].is<JsonObjectConst>()) {
        for (JsonPairConst kv : o["apps"].as<JsonObjectConst>()) {
            const LauncherApp *a = launcher_app(kv.key().c_str());
            const char *v = kv.value() | "";
            const int level = !strcmp(v, "full") ? 2 : !strcmp(v, "read") ? 1 : !strcmp(v, "hidden") ? 0 : -1;
            if (!a || level < 0) {
                return "apps: {\"home\": \"hidden\" | \"read\" | \"full\", ...}";
            }
            p.apps[slot_of(*a)] = uint8_t(level);
        }
    }
    return nullptr;
}

std::string reply(profile_result_t r, int id = -1)
{
    JsonDocument doc;
    if (r == PROFILE_OK) {
        doc["reply"] = "ok";
        if (id >= 0) {
            doc["id"] = id;
        }
    } else {
        doc["error"] = profiles_result_text(r);
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string error_reply(const char *text)
{
    JsonDocument doc;
    doc["error"] = text;
    std::string out;
    serializeJson(doc, out);
    return out;
}

// Changes run with the display lock held: listeners update the screens.
template <typename F> profile_result_t locked(F f)
{
    if (!bsp_display_lock(3000)) {
        return PROFILE_ERR_STORAGE;
    }
    const profile_result_t r = f();
    bsp_display_unlock();
    return r;
}

// One change. by: whose rights. recovery: RPC 61's operations allowed.
std::string apply(const char *json, size_t length, profile_editor_t by, bool recovery, bool *ok)
{
    *ok = false;
    JsonDocument doc;
    if (deserializeJson(doc, json, length) || !doc.is<JsonObject>()) {
        return error_reply("expected a JSON object");
    }
    const std::string op = doc["op"] | "";
    const int id = doc["id"] | -1;
    std::string pin = doc["pin"] | "";
    profile_result_t r = PROFILE_ERR_INVALID;
    int result_id = -1;
    profile_pin_t hashed{};
    // The hash first, without the display lock: it takes about half a second.
    const bool wants_hash = op == "setup" || op == "set_pin";
    if (wants_hash && !profiles_hash_pin(pin.c_str(), &hashed)) {
        mbedtls_platform_zeroize(&pin[0], pin.size());
        return error_reply("a PIN is 4 to 6 digits");
    }
    if (op == "setup") {
        const std::string name = doc["name"] | "Owner";
        uint8_t owner = 0;
        r = locked([&] {
            return profiles_create_owner(name.c_str(), &hashed, &owner);
        });
        result_id = owner;
    } else if (op == "save" && doc["profile"].is<JsonObject>()) {
        profile_t p{};
        if (const char *bad = parse_profile(doc["profile"].as<JsonObjectConst>(), p)) {
            return error_reply(bad);
        }
        r = locked([&] {
            return profiles_save(&p, by);
        });
        result_id = p.id;
    } else if (op == "delete") {
        r = locked([&] {
            return profiles_delete(uint8_t(id), by);
        });
    } else if (op == "lock") {
        r = locked([&] {
            profiles_lock();
            return PROFILE_OK;
        });
    } else if (op == "switch") {
        // A profile without sign-in, or with a PIN only: full mode needs the
        // face at the board.
        profile_t p{};
        if (!profiles_get(uint8_t(id), &p)) {
            r = PROFILE_ERR_NO_PROFILE;
        } else {
            if (!pin.empty()) {
                r = profiles_check_pin(uint8_t(id), pin.c_str());
            }
            if (pin.empty() || r == PROFILE_OK) {
                r = locked([&] {
                    return profiles_sign_in(uint8_t(id));
                });
            }
        }
    } else if (op == "settings") {
        profiles_settings_t s{};
        profiles_get_settings(&s);
        s.default_id = uint8_t(doc["default"] | int(s.default_id));
        s.lock_s = uint16_t(doc["lock_s"] | int(s.lock_s));
        s.elevated_lock_s = uint16_t(doc["elevated_lock_s"] | int(s.elevated_lock_s));
        r = locked([&] {
            return profiles_set_settings(&s, by);
        });
    } else if (op == "set_pin" && (recovery || by == PROFILE_BY_SESSION)) {
        r = locked([&] {
            return profiles_set_pin(uint8_t(id), &hashed, by);
        });
    } else if (op == "unblock" && (recovery || by == PROFILE_BY_SESSION)) {
        r = profiles_unblock(uint8_t(id), by);
    } else if (op == "off" && (recovery || by == PROFILE_BY_SESSION)) {
        r = locked([&] {
            return profiles_turn_off(by);
        });
    } else {
        mbedtls_platform_zeroize(&pin[0], pin.size());
        return error_reply(recovery ? "op: set_pin, unblock or off" : "op: list, setup, save, delete, lock, switch or settings");
    }
    mbedtls_platform_zeroize(&pin[0], pin.size());
    mbedtls_platform_zeroize(&hashed, sizeof(hashed));
    ESP_LOGI(TAG, "%s%s: %s", recovery ? "recovery " : "", op.c_str(), profiles_result_text(r));
    *ok = r == PROFILE_OK;
    return reply(r, result_id);
}

// ---- RPC 54 · 60 and 54 · 61 -----------------------------------------------------------------
// A page of the list: as many profiles from `from` as fit, and where the next page starts.
size_t list_page(int from, char *out, size_t cap)
{
    profile_t all[SLOTS];
    const int n = profiles_list(all, SLOTS);
    JsonDocument doc;
    header_json(doc);
    JsonArray arr = doc["profiles"].to<JsonArray>();
    int i = std::max(0, from);
    for (; i < n; ++i) {
        profile_json(all[i], arr.add<JsonObject>());
        if (measureJson(doc) + 16 > cap) {  // 16: room for "next"
            arr.remove(arr.size() - 1);
            break;
        }
    }
    if (i < n) {
        doc["next"] = i;
    }
    return serializeJson(doc, out, cap);
}

// Changes run in their own task: ESP-Iris's service task has a 4 KB stack,
// too little for the hash and an NVS write. The reply waits for it.
struct Change {
    std::string json, result;
    bool recovery = false, ok = false;
    SemaphoreHandle_t done = nullptr;
    ~Change()
    {
        mbedtls_platform_zeroize(json.data(), json.size());
        if (done) {
            vSemaphoreDelete(done);
        }
    }
};

esp_err_t change_rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t cap, size_t *size, bool recovery)
{
    auto change = std::make_shared<Change>();
    change->json.assign(reinterpret_cast<const char *>(request->payload), request->payload_size);
    change->recovery = recovery;
    change->done = xSemaphoreCreateBinary();
    if (!change->done) {
        return ESP_ERR_NO_MEM;
    }
    run_task(new std::function<void()>([change] {
        change->result = apply(change->json.data(), change->json.size(), PROFILE_BY_TRUSTED, change->recovery, &change->ok);
        xSemaphoreGive(change->done);
    }));
    if (xSemaphoreTake(change->done, pdMS_TO_TICKS(8000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *size = std::min(cap, change->result.size());
    memcpy(out, change->result.data(), *size);
    return ESP_OK;
}

esp_err_t profiles_rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t cap, size_t *size, void *)
{
    *size = 0;
    // None, or {"op":"list","from":N}: a page of the list. Anything else: a change.
    JsonDocument doc;
    const bool list = !request->payload_size ||
                      (!deserializeJson(doc, reinterpret_cast<const char *>(request->payload), request->payload_size) && (doc["op"] | "list") == std::string("list"));
    if (list) {
        *size = list_page(doc["from"] | 0, reinterpret_cast<char *>(out), cap);
        return ESP_OK;
    }
    return change_rpc(request, out, cap, size, false);
}

// Recovery needs the board in hand: not over the network, even from a paired computer.
esp_err_t recovery_rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t cap, size_t *size, void *)
{
    *size = 0;
    esp_iris_status_t status{};
    if (esp_iris_get_status(&status) != ESP_OK || status.transport == ESP_IRIS_TRANSPORT_KIND_TCP ||
            status.transport == ESP_IRIS_TRANSPORT_KIND_NONE) {
        static const char NOT_USB[] = "{\"error\":\"recovery works over USB only\"}";
        *size = std::min(cap, sizeof(NOT_USB) - 1);
        memcpy(out, NOT_USB, *size);
        return ESP_OK;
    }
    return change_rpc(request, out, cap, size, true);
}

// ---- Swifty's tools ----------------------------------------------------------------------------
// What each tool needs: an app at least so far open (-1: none), and a
// capability (-1: none). Tools not listed need Swifty in full; a profile with
// Swifty read-only gets the READS only.
struct ToolRule {
    const char *name;
    int app;
    uint8_t access;
    int cap;
};
constexpr uint8_t R = PROFILE_ACCESS_READ, F = PROFILE_ACCESS_FULL;
const ToolRule RULES[] = {
    {"open_app", -1, 0, -1},  // the app it opens is checked as it opens
    {"device_status", -1, 0, -1},
    {"set_brightness", -1, 0, -1},
    {"get_time", -1, 0, -1},
    {"level", -1, 0, -1},
    {"glimdock", int(ShellApp::Glimdock), R, -1},
    {"home_state", int(ShellApp::Home), R, -1},
    {"home_status", int(ShellApp::Home), R, -1},
    {"home_control", int(ShellApp::Home), F, PROFILE_CAP_HOME_CONTROL},
    {"vpn", int(ShellApp::Vpn), R, -1},  // switching it checks Wi-Fi and VPN settings
    {"flashlight", -1, 0, PROFILE_CAP_BOARD_CONTROL},
    {"list_files", int(ShellApp::Files), R, -1},
    {"read_file", int(ShellApp::Files), R, -1},
    {"write_file", int(ShellApp::Files), F, -1},
    {"delete_file", int(ShellApp::Files), F, -1},
    {"interaction_board", int(ShellApp::Interaction), R, -1},
    {"board_lights", -1, 0, PROFILE_CAP_BOARD_CONTROL},
    {"tv_remote", -1, 0, PROFILE_CAP_BOARD_CONTROL},
    {"take_photo", int(ShellApp::Camera), F, -1},
    {"show_photo", int(ShellApp::Gallery), R, -1},
    {"web_search", -1, 0, PROFILE_CAP_WEB_SEARCH},
    {"list_shortcuts", -1, 0, PROFILE_CAP_SHORTCUTS_RUN},
    {"get_shortcut", -1, 0, PROFILE_CAP_SHORTCUTS_RUN},
    {"run_shortcut", -1, 0, PROFILE_CAP_SHORTCUTS_RUN},  // each action is checked as it runs (shortcuts_policy.cpp)
    {"add_shortcut", -1, 0, PROFILE_CAP_SHORTCUTS_EDIT},
    {"enable_shortcut", -1, 0, PROFILE_CAP_SHORTCUTS_EDIT},
    {"delete_shortcut", -1, 0, PROFILE_CAP_SHORTCUTS_EDIT},
};
// Tools that change nothing: all a read-only Swifty offers.
const char *const READS[] = {"open_app", "device_status", "get_time", "level", "glimdock", "home_state", "home_status",
                             "vpn", "list_files", "read_file", "interaction_board", "show_photo", "web_search",
                             "list_shortcuts", "get_shortcut", "list_reminders", "paseo_list_agents", "paseo_read", "paseo_workspaces",
                             "paseo_schedules"
                            };
// Families by prefix: Paseo's agents; machines once the collector can act.
const struct {
    const char *prefix;
    int cap;
} FAMILIES[] = {{"paseo_", PROFILE_CAP_PASEO}, {"machine_", PROFILE_CAP_MACHINE_ACTIONS}, {"restart_", PROFILE_CAP_MACHINE_ACTIONS}};
}  // namespace

bool profiles_tool_allowed(const char *tool)
{
    if (!profiles_configured()) {
        return true;  // as before profiles
    }
    const profile_access_t swifty = profiles_app_access(ShellApp::Swifty);
    if (!tool || swifty == PROFILE_ACCESS_HIDDEN) {
        return false;
    }
    if (swifty == PROFILE_ACCESS_READ) {
        bool read = false;
        for (const char *r : READS) {
            read |= !strcmp(r, tool);
        }
        if (!read) {
            return false;
        }
    }
    for (const auto &f : FAMILIES)
        if (!strncmp(tool, f.prefix, strlen(f.prefix)) && !profiles_can(profile_cap_t(f.cap))) {
            return false;
        }
    for (const ToolRule &r : RULES)
        if (!strcmp(r.name, tool)) {
            return (r.app < 0 || profiles_app_access(r.app) >= r.access) && (r.cap < 0 || profiles_can(profile_cap_t(r.cap)));
        }
    return true;  // others: Swifty in full (checked above), a family's capability
}

void profiles_describe(char *out, size_t cap)
{
    out[0] = 0;
    if (!profiles_configured() || !cap) {
        return;  // one owner, as before profiles: nothing to say
    }
    profile_t p{};
    profiles_get(profiles_current(), &p);
    std::string s = std::string("The person at the board is signed in as ") + p.name +
                    (p.id == PROFILE_GUEST ? " (a guest)" : "") + (profiles_elevated() ? ", in full mode." : ".");
    std::string hidden, read;
    for (int i = 0; i < LAUNCHER_APP_COUNT && i < PROFILE_APPS; ++i) {
        const LauncherApp &a = LAUNCHER_APPS[i];
        if (a.keep) {
            continue;  // Settings shows for everyone
        }
        const profile_access_t acc = profiles_app_access(slot_of(a));
        std::string &list = acc == PROFILE_ACCESS_HIDDEN ? hidden : read;
        if (acc != PROFILE_ACCESS_FULL) {
            list += (list.empty() ? "" : ", ") + std::string(a.name);
        }
    }
    if (!hidden.empty()) {
        s += " Not theirs: " + hidden + ".";
    }
    if (!read.empty()) {
        s += " Read-only for them: " + read + ".";
    }
    std::string cannot;
    for (int c = 0; c < PROFILE_CAP_COUNT; ++c)
        if (!profiles_can(profile_cap_t(c)) && c != PROFILE_CAP_SETTINGS_PROFILES && c != PROFILE_CAP_SWIFTY_ADVANCED) {
            std::string t = profiles_cap_title(profile_cap_t(c));
            t[0] = char(tolower(t[0]));
            cannot += (cannot.empty() ? "" : ", ") + t;
        }
    if (!cannot.empty()) {
        s += " They may not: " + cannot + ".";
    }
    s += " Tools they may not use are not offered; if they ask for more, say briefly that their profile does not allow it.";
    strlcpy(out, s.c_str(), cap);
}

bool profiles_key_visible(const char *key)
{
    if (!profiles_configured() || !key) {
        return true;
    }
    if (!strncmp(key, "sc:", 3)) {
        return profiles_can(PROFILE_CAP_SHORTCUTS_RUN);  // shortcut tiles
    }
    const LauncherApp *a = launcher_app(key);
    return !a || profiles_app_access(slot_of(*a)) != PROFILE_ACCESS_HIDDEN;
}

std::string profiles_web_list()
{
    profile_t all[SLOTS];
    const int n = profiles_list(all, SLOTS);
    JsonDocument doc;
    header_json(doc);
    doc["manage"] = profiles_can(PROFILE_CAP_SETTINGS_PROFILES);
    JsonArray keys = doc["app_keys"].to<JsonArray>();  // by slot
    for (int i = 0; i < PROFILE_APPS; ++i) {
        const char *key = i == 0 ? "launcher" : i == PROFILE_APP_SETTINGS ? "settings" : launcher_app_key(ShellApp(i));
        keys.add(key ? key : "");
    }
    JsonArray names = doc["app_names"].to<JsonArray>();
    for (int i = 0; i < LAUNCHER_APP_COUNT && i < PROFILE_APPS; ++i) {
        const LauncherApp &a = LAUNCHER_APPS[i];
        JsonObject o = names.add<JsonObject>();
        o["slot"] = slot_of(a);
        o["key"] = a.key;
        o["name"] = a.name;
    }
    JsonArray caps = doc["cap_keys"].to<JsonArray>();  // by bit
    for (int c = 0; c < PROFILE_CAP_COUNT; ++c) {
        JsonObject o = caps.add<JsonObject>();
        o["key"] = profiles_cap_key(profile_cap_t(c));
        o["title"] = profiles_cap_title(profile_cap_t(c));
        o["elevated"] = bool(PROFILE_CAP(c) & PROFILE_CAPS_ELEVATED);
    }
    JsonArray arr = doc["profiles"].to<JsonArray>();
    for (int i = 0; i < n; ++i) {
        profile_json(all[i], arr.add<JsonObject>());
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string profiles_web_apply(const char *json, size_t length, bool *ok)
{
    return apply(json, length, PROFILE_BY_SESSION, false, ok);
}

void profiles_glue_start(void (*apply_fn)())
{
    apply_hook = apply_fn;
    profiles_set_face_hooks(face_id_enrolled, face_id_forget);
    int n = 0;
    for (int i = 0; i < LAUNCHER_APP_COUNT && i < PROFILE_APPS; ++i) {
        const LauncherApp &a = LAUNCHER_APPS[i];
        if (a.keep) {
            continue;  // Settings: always on the home screen (the way back)
        }
        const int slot = slot_of(a);
        // Read-only means something in Glimdock, Home and Swifty (its tools).
        const bool read_only = slot == int(ShellApp::Glimdock) || slot == int(ShellApp::Home) || slot == int(ShellApp::Swifty);
        apps[n++] = {slot, a.name, a.glyph, read_only};
    }
    ProfilesUiHooks hooks;
    hooks.apps = apps;
    hooks.app_count = n;
    hooks.background = ui_background;
    profiles_ui_init(hooks);
    profiles_on_change(changed, nullptr);
    // The auto-lock: unused for the profile's time, back to the default one.
    lv_timer_create([](lv_timer_t *) {
        if (profiles_idle(lv_display_get_inactive_time(nullptr))) {
            ESP_LOGI(TAG, "locked after the board went unused");
        }
    }, 1000, nullptr);
    iris_ext_register(60, profiles_rpc, nullptr);
    iris_ext_register(61, recovery_rpc, nullptr);
    ESP_LOGI(TAG, "%s; signed in: %u", profiles_configured() ? "profiles on" : "profiles off (one owner, as before)",
             unsigned(profiles_current()));
}
