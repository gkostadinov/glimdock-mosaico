// SPDX-License-Identifier: Apache-2.0
// Shortcuts' engine (see shortcuts.h). The list lives in memory behind a
// mutex, each shortcut an unchanging copy that runs share: Web settings,
// Swifty's agent task and RPC 55 replace them; the LVGL task saves the list
// (a flash write) from its timer, as reminders do, hands the home screen its
// tiles and watches the triggers. Runs happen on three tasks of their own
// (stacks in PSRAM, below LVGL's drawing, no flash writes): Swifty Home
// commands start on the LVGL task (with the display lock) and are waited for,
// waits sleep, and the Interaction board's steps, which can take seconds,
// block there and not the screen. The programs (their JSON, checks and
// interpreter) are shortcuts_program.cpp; the screens shortcuts_ui.cpp.
#include "shortcuts.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "ArduinoJson.h"
#include "always_on.h"
#include "board_wifi.h"
#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "glimdock_app.h"
#include "haptics.h"
#include "home_app.h"
#include "launcher_ui.h"
#include "mui.h"
#include "nvs.h"
#include "shortcuts_internal.h"
#include "swifty_app.h"
#include "tools_app.h"
#include "vpn_app.h"

namespace shortcuts {
namespace {
const char *TAG = "shortcuts";
constexpr const char *NS = "shortcuts";
constexpr size_t MAX_STORED = 12 * 1024;  // the list as saved (NVS is shared by every app)
constexpr size_t MAX_QUEUE = 6;
constexpr int RUNNERS = 3;                 // runs at once
constexpr time_t CLOCK_SET = 1735689600;   // 2025-01-01: before this the clock is not set yet
constexpr int ASK_S = 60;                  // a card that asks waits this long
const mui::Accent *const ACCENT_STYLES[] = {&mui::VIOLET, &mui::CYAN, &mui::AMBER, &mui::GREEN};

using Ptr = std::shared_ptr<const Shortcut>;

std::mutex lock;
std::vector<Ptr> list;
uint32_t next_id = 1;
bool seeded = false;          // the starters were offered once (deleting them is final)
bool dirty = false;           // changed since saved (the timer saves it)
uint32_t list_rev = 1;        // bumped by every change to the list: the tiles follow it
std::atomic<uint32_t> rev{1};  // list_rev and every run's start and end: the editor follows it
Location location;
std::map<uint32_t, RunStatus> statuses;  // 0: the last test of a shortcut not saved
std::vector<uint32_t> boot_ids;          // there at start-up, with a boot trigger
std::atomic<bool> dry_all{false};        // RPC 55: every run is a dry run until a restart
std::atomic<bool> starters_wanted{false};  // RPC 55 asked for them: the LVGL timer adds them
ShortcutHooks H;

DeserializationError read_json(JsonDocument &doc, const char *text, size_t length)
{
    // A shortcut's blocks nest deeper than ArduinoJson's default of 10.
    return deserializeJson(doc, text, length, DeserializationOption::NestingLimit(24));
}

DeserializationError read_json(JsonDocument &doc, const std::string &text)
{
    return read_json(doc, text.data(), text.size());
}

std::string json_text(const JsonDocument &doc)
{
    std::string text;
    serializeJson(doc, text);
    return text;
}

std::string error_json(const std::string &why)
{
    JsonDocument e;
    e["error"] = why;
    return json_text(e);
}

std::string when_text(time_t t)
{
    if (!t) {
        return "";
    }
    tm local;
    localtime_r(&t, &local);
    char text[24];
    strftime(text, sizeof(text), "%a %H:%M", &local);
    return text;
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

// A card that asks, from a run or Swifty's task (never the LVGL task): true
// on Allow, false on Not now or when nobody answers within a minute.
bool ask_screen(const std::string &title, const std::string &body, const std::string &allow)
{
    auto answer = std::make_shared<std::atomic<int>>(-1);
    if (!on_ui([&] {
    ui_ask(title, body, allow, ASK_S, [answer](bool yes) {
            answer->store(yes ? 1 : 0);
        });
    })) {
        return false;
    }
    for (int waited = 0; answer->load() < 0 && waited < (ASK_S + 5) * 1000; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return answer->load() == 1;
}

// ---- the list (under lock) ---------------------------------------------------------------
Ptr find_locked(uint32_t id)
{
    for (const Ptr &p : list)
        if (p->id == id) {
            return p;
        }
    return nullptr;
}

// A shortcut named like this (exactly, then nearly). nullptr: none.
Ptr by_name_locked(const std::string &want)
{
    const std::string w = lowered(want);
    for (int exact = 1; exact >= 0; --exact)
        for (const Ptr &p : list) {
            const std::string n = lowered(p->name);
            if (exact ? n == w : (!w.empty() && n.find(w) != std::string::npos)) {
                return p;
            }
        }
    return nullptr;
}

std::string stored_json_locked()
{
    JsonDocument doc;
    doc["v"] = FORMAT;
    doc["next"] = next_id;
    doc["seeded"] = seeded;
    if (location.set) {
        doc["loc"]["lat"] = location.lat;
        doc["loc"]["lon"] = location.lon;
    }
    JsonArray arr = doc["list"].to<JsonArray>();
    for (const Ptr &p : list) {
        shortcut_json(*p, arr.add<JsonObject>());
    }
    return json_text(doc);
}

void changed_locked()
{
    dirty = true;
    ++list_rev;
    ++rev;
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
    bool migrated = false;
    if (text.empty() || read_json(doc, text) || !migrate(doc, &migrated)) {
        return;
    }
    // A format 1 list is read through the migration every start until
    // something changes; only then is format 2 written over it.
    std::lock_guard<std::mutex> g(lock);
    for (JsonObjectConst o : doc["list"].as<JsonArrayConst>()) {
        auto sc = std::make_shared<Shortcut>();
        std::string error;
        if (parse_shortcut(o, *sc, error) && sc->id) {
            list.push_back(sc);
        } else {
            ESP_LOGW(TAG, "dropped a stored shortcut: %s", error.c_str());
        }
    }
    next_id = std::max<uint32_t>(doc["next"] | 1u, 1);
    for (const Ptr &p : list) {
        next_id = std::max(next_id, p->id + 1);
        if (p->enabled && std::any_of(p->triggers.begin(), p->triggers.end(), [](const Trigger & t) {
        return t.kind == On::Boot;
    })) {
            boot_ids.push_back(p->id);
        }
    }
    seeded = doc["seeded"] | false;
    if (doc["loc"]["lat"].is<float>()) {
        location.set = true;
        location.lat = doc["loc"]["lat"];
        location.lon = doc["loc"]["lon"] | 0.0f;
    }
    ESP_LOGI(TAG, "%u shortcuts%s", unsigned(list.size()), migrated ? " (format 1, read as format 2)" : "");
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

// ---- Home --------------------------------------------------------------------------------
// Home started, paired and with the hub's lists, waiting up to 15 s for them
// after a restart (a run's or Swifty's task, never the LVGL task).
bool wait_home(std::string &error)
{
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
    }
    return ready;
}

struct Reading {
    std::string name;
    bool has_on = false, on = false, has_temp = false;
    float temp = 0;
};

// What a name means among Home's devices (home_app_states_json): a device by
// its target or name, else a room (on when anything in it is on; its first
// temperature), else a device whose name holds it.
bool reading_of(JsonArrayConst devices, const std::string &want, Reading &out)
{
    out = Reading();
    const std::string w = lowered(want);
    auto take = [&](JsonObjectConst d) {
        if (d["on"].is<bool>()) {
            out.has_on = true;
            out.on = out.on || d["on"].as<bool>();
        }
        if (!out.has_temp && d["temp"].is<float>()) {
            out.has_temp = true;
            out.temp = d["temp"].as<float>();
        }
    };
    for (JsonObjectConst d : devices)
        if (w == lowered(d["target"] | "") || w == lowered(d["name"] | "")) {
            out.name = d["name"] | "";
            take(d);
            return true;
        }
    for (JsonObjectConst d : devices) {
        const std::string room = lowered(d["room"] | "");
        if (!room.empty() && (w == room || w == room + " lights")) {
            out.name = d["room"] | "";
            take(d);
        }
    }
    if (!out.name.empty()) {
        return true;
    }
    for (JsonObjectConst d : devices)
        if (!w.empty() && lowered(d["name"] | "").find(w) != std::string::npos) {
            out.name = d["name"] | "";
            take(d);
            return true;
        }
    return false;
}

// The device and room names Home has, for an error that says what to use.
std::string home_names(JsonArrayConst devices)
{
    std::vector<std::string> names;
    for (JsonObjectConst d : devices) {
        for (const char *key : {"name", "room"}) {
            const std::string n = d[key] | "";
            if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end() && names.size() < 12) {
                names.push_back(n);
            }
        }
    }
    std::string out;
    for (const std::string &n : names) {
        out += (out.empty() ? "" : ", ") + n;
    }
    return out;
}

bool run_home(const Action &a, bool dry, bool ask, const std::string &shortcut, std::string &what, std::string &error)
{
    if (!wait_home(error)) {
        return false;
    }
    // Nobody tapped: what Home asks before (the TV's power, Movie) is asked on the screen.
    if (ask && !dry) {
        bool asks = false;
        std::string words, resolved;
        on_ui([&] {
            asks = home_app_asks_first(a.verb.c_str(), a.target.c_str(), a.value.c_str());
            home_app_resolve(a.verb.c_str(), a.target.c_str(), a.value.c_str(), words, resolved);
        });
        if (asks && !ask_screen(shortcut, "wants to: " + words + ". Nobody tapped it: allow this now?", "Allow")) {
            what = words;
            error = "Not allowed on the screen (Swifty Home asks first for this).";
            return false;
        }
    }
    struct HomeResult {
        std::atomic<int> state{-1};  // -1 waiting, 0 failed, 1 done
        std::string message;
    };
    auto result = std::make_shared<HomeResult>();
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool started = false;
    const bool locked = on_ui([&] {
        started = home_app_run(a.verb.c_str(), a.target.c_str(), a.value.c_str(), dry,
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

// ---- actions: the one way out (a run's task) ---------------------------------------------
bool perform(const Action &a, bool dry, Source source, const std::string &shortcut, std::string &what, std::string &error)
{
    what = action_text(a);
    // Profiles' choke point: every action, just before it runs.
    const ShortcutActionInfo info = {kind_name(a.kind), a.verb.c_str(), a.target.c_str(), a.value.c_str(), a.n,
                                     source_name(source), shortcut.c_str(), dry
                                    };
    std::string why;
    if (!shortcuts_action_allowed(info, &why)) {
        error = why.empty() ? "Not allowed." : why;
        return false;
    }
    char out[200] = "";
    switch (a.kind) {
    case Do::Home:
        return run_home(a, dry, source_automatic(source), shortcut, what, error);
    case Do::Ir:
        if (!dry && !tools_board_ir(a.value.c_str(), -1, -1, out, sizeof(out))) {
            error = out;
            return false;
        }
        return true;
    case Do::BoardLights:
        if (!dry && !tools_board_lights(a.verb.c_str(), a.value.c_str(), out, sizeof(out))) {
            error = out;
            return false;
        }
        return true;
    case Do::Brightness:
        if (!dry) {
            on_ui([&] {
                if (H.brightness) {
                    H.brightness(a.n);
                }
            });
        }
        return true;
    case Do::Open: {
        if (dry) {
            return true;
        }
        bool ok = false;
        on_ui([&] {
            ok = H.open_app && H.open_app(a.verb.c_str(), a.value.c_str());
        });
        if (!ok) {
            error = "could not open " + a.verb;
        }
        return ok;
    }
    case Do::Flashlight: {
        if (dry) {
            return true;
        }
        bool ok = false;
        on_ui([&] {
            ok = H.flashlight && H.flashlight(a.n != 0);
        });
        if (!ok) {
            error = "no camera module";
        }
        return ok;
    }
    default:
        return false;
    }
}

// ---- the board, for the interpreter (a run's task) ---------------------------------------
class BoardEnv : public Env {
public:
    BoardEnv(const Shortcut &sc, Source source, std::atomic<bool> &stop) : sc_(sc), source_(source), stop_(stop) {}

    bool act(const Action &a, bool dry, std::string &what, std::string &error) override
    {
        return perform(a, dry, source_, sc_.name, what, error);
    }

    bool read(const Cond &c, bool &value, std::string &error) override
    {
        switch (c.kind) {
        case Check::Home: {
            if (!wait_home(error)) {
                return false;
            }
            std::string states;
            on_ui([&] {
                states = home_app_states_json();
            });
            JsonDocument doc;
            deserializeJson(doc, states);
            Reading r;
            if (!reading_of(doc.as<JsonArrayConst>(), c.name, r)) {
                error = "Swifty Home has no \"" + c.name + "\"";
                return false;
            }
            if (c.cmp == Cmp::IsOn || c.cmp == Cmp::IsOff) {
                if (!r.has_on) {
                    error = r.name + " does not say whether it is on";
                    return false;
                }
                value = r.on == (c.cmp == Cmp::IsOn);
            } else {
                if (!r.has_temp) {
                    error = r.name + " reports no temperature";
                    return false;
                }
                value = c.cmp == Cmp::Above ? r.temp > c.num : r.temp < c.num;
            }
            return true;
        }
        case Check::Motion:
        case Check::Button:
        case Check::Light: {
            tools_board_inputs_t in;
            if (!tools_board_inputs(&in, 3000)) {
                error = "no Interaction board";
                return false;
            }
            if (c.kind == Check::Motion) {
                value = in.motion || (c.a > 0 && in.motion_age_s >= 0 && in.motion_age_s <= c.a);
            } else if (c.kind == Check::Button) {
                value = c.name == "left" ? in.left : c.name == "right" ? in.right : in.left || in.right;
            } else {
                if (in.light < 0) {
                    error = "the Interaction board's light sensor has not answered";
                    return false;
                }
                value = c.cmp == Cmp::Above ? in.light > c.num : in.light < c.num;
            }
            return true;
        }
        case Check::Wifi:
            on_ui([&] {
                value = board_wifi_status().connected;
            });
            return true;
        case Check::Vpn: {
            vpn_summary_t s = {};
            on_ui([&] {
                vpn_app_summary(&s);
            });
            value = s.state == VPN_CONNECTED;
            return true;
        }
        case Check::Battery:
        case Check::Charging: {
            int pct = -1;
            bool charging = false;
            on_ui([&] {
                if (H.battery) {
                    pct = H.battery(&charging);
                }
            });
            if (pct < 0) {
                error = "the battery cannot be read";
                return false;
            }
            value = c.kind == Check::Charging ? charging : c.cmp == Cmp::Above ? pct > c.num : pct < c.num;
            return true;
        }
        default:
            error = "nothing reads this";
            return false;
        }
    }

    bool sleep(int ms) override
    {
        for (int left = ms; left > 0; left -= 100) {
            if (stop_.load()) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(std::min(left, 100)));
        }
        return !stop_.load();
    }

    int64_t now_ms() override
    {
        return esp_timer_get_time() / 1000;
    }

    bool local_time(struct tm &out) override
    {
        const time_t now = time(nullptr);
        if (now < CLOCK_SET) {
            return false;
        }
        localtime_r(&now, &out);
        return true;
    }

    void message(const std::string &text, bool buzz, bool chime) override
    {
        on_ui([&] {
            ui_message(sc_.name, text, sc_.accent, buzz, chime);
        });
    }

    std::shared_ptr<const Shortcut> find(uint32_t id) override
    {
        return shortcuts::find(id);
    }

    bool stopped() override
    {
        return stop_.load();
    }

private:
    const Shortcut &sc_;
    Source source_;
    std::atomic<bool> &stop_;
};

// ---- the home screen's tiles (LVGL task) -------------------------------------------------
// Their keys are "sc:<id>"; the launcher keeps where each one is (and if it is hidden).
std::string tile_key(uint32_t id)
{
    return "sc:" + std::to_string(id);
}

std::vector<uint32_t> tiles;  // the ids the launcher has

void tile_tapped(const char *, void *user)
{
    const uint32_t id = uint32_t(uintptr_t(user));
    if (!start(id, false, Source::Tile)) {
        Ptr p = find(id);
        if (p && status_of(id).running) {
            ui_toast(p->name, p->icon, p->accent, Feedback::Failed, "Running already: stop it in the shortcuts' editor");
        }
    }
}

void tiles_publish()
{
    std::vector<Ptr> copy = list_copy();
    for (uint32_t id : tiles) {
        const bool kept = std::any_of(copy.begin(), copy.end(), [id](const Ptr & p) {
            return p->id == id && p->tile();
        });
        if (!kept) {
            launcher_shortcut_remove(tile_key(id).c_str());  // deleted (or its tile is): its place goes too
        }
    }
    tiles.clear();
    for (const Ptr &p : copy) {
        if (!p->tile()) {
            continue;
        }
        const std::string key = tile_key(p->id);
        const LauncherShortcut tile = {key.c_str(), p->name.c_str(), p->icon.c_str(), ACCENT_STYLES[p->accent % 4],
                                       tile_tapped, reinterpret_cast<void *>(uintptr_t(p->id))
                                      };
        if (launcher_shortcut_set(tile)) {
            tiles.push_back(p->id);
        }
    }
}

void tile_feedback(uint32_t id, Feedback state)
{
    if (std::find(tiles.begin(), tiles.end(), id) != tiles.end()) {
        launcher_shortcut_feedback(tile_key(id).c_str(), state == Feedback::Running ? -1 : state == Feedback::Done ? 1 : 0);
    }
}

// ---- runs (their own tasks) --------------------------------------------------------------
struct Outcome {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string message;
    std::vector<std::string> log;
};
struct Job {
    Ptr sc;
    uint32_t key;  // the shortcut's id; 0 for a test of one not saved
    bool dry;
    Source source;
    std::shared_ptr<Outcome> outcome;
};
struct Active {
    uint32_t key;
    std::shared_ptr<std::atomic<bool>> stop;
};
std::deque<Job> jobs;        // under lock
std::vector<Active> active;  // under lock
TaskHandle_t runners[RUNNERS] = {};

bool busy_locked(uint32_t key)
{
    return key && (std::any_of(jobs.begin(), jobs.end(), [key](const Job & j) {
        return j.key == key;
    }) || std::any_of(active.begin(), active.end(), [key](const Active & a) {
        return a.key == key;
    }));
}

// Queues a run. False: why not (it is running already, or too many wait).
bool enqueue(Ptr sc, uint32_t key, bool dry, Source source, std::shared_ptr<Outcome> outcome, std::string *why)
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (!runners[0]) {
            *why = "Shortcuts have not started yet.";
            return false;
        }
        if (busy_locked(key)) {
            *why = "\"" + sc->name + "\" is running already.";
            return false;
        }
        if (jobs.size() >= MAX_QUEUE) {
            *why = "Too many shortcuts are running; try again in a moment.";
            return false;
        }
        jobs.push_back({std::move(sc), key, dry, source, std::move(outcome)});
        statuses[key].running = true;
        ++rev;
    }
    for (TaskHandle_t t : runners) {
        xTaskNotifyGive(t);  // whichever is free takes it
    }
    return true;
}

void run_job(const Job &job, std::atomic<bool> &stop)
{
    const Shortcut &sc = *job.sc;
    const bool dry = job.dry || dry_all.load();
    // A run nobody tapped stays quiet unless it fails (its messages show anyway).
    const bool quiet = source_automatic(job.source);
    ESP_LOGI(TAG, "running %u \"%s\" (%s)%s", unsigned(sc.id), sc.name.c_str(), source_name(job.source), dry ? " (dry run)" : "");
    on_ui([&] {
        if (job.key) {
            tile_feedback(job.key, Feedback::Running);
        }
        if (!quiet) {
            ui_toast(sc.name, sc.icon, sc.accent, Feedback::Running, dry ? "Test: nothing is sent" : "");
        }
    });
    BoardEnv env(sc, job.source, stop);
    const RunResult r = run(sc, env, dry);
    for (const std::string &line : r.log) {
        ESP_LOGI(TAG, "  %s", line.c_str());
    }
    ESP_LOGI(TAG, "  %s: %s (%lld ms)", r.ok ? "ok" : "failed", r.message.c_str(), (long long)r.ms);
    on_ui([&] {
        if (job.key) {
            tile_feedback(job.key, r.ok ? Feedback::Done : Feedback::Failed);
        }
        if (!quiet || !r.ok) {
            ui_toast(sc.name, sc.icon, sc.accent, r.ok ? Feedback::Done : Feedback::Failed,
                     r.ok && r.steps == 1 && !dry ? "" : r.message);
            ui_buzz(!r.ok);
        }
    });
    {
        std::lock_guard<std::mutex> g(lock);
        RunStatus &st = statuses[job.key];
        st.running = false;
        st.ran = true;
        st.ok = r.ok;
        st.dry = dry;
        st.source = job.source;
        const time_t now = time(nullptr);
        st.at = now >= CLOCK_SET ? now : 0;
        st.message = r.message;
        st.log = r.log;
        ++rev;
    }
    if (job.outcome) {
        job.outcome->ok = r.ok;
        job.outcome->message = r.message;
        job.outcome->log = r.log;
        job.outcome->done.store(true);
    }
}

void runner_task(void *)
{
    for (;;) {
        Job job;
        auto stop = std::make_shared<std::atomic<bool>>(false);
        for (;;) {
            {
                std::lock_guard<std::mutex> g(lock);
                if (!jobs.empty()) {
                    job = jobs.front();
                    jobs.pop_front();
                    active.push_back({job.key, stop});
                    break;
                }
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        run_job(job, *stop);
        std::lock_guard<std::mutex> g(lock);
        active.erase(std::find_if(active.begin(), active.end(), [&](const Active & a) {
            return a.stop == stop;
        }));
    }
}

// Waits for a run that reports back (Swifty's and Web settings' tasks).
bool wait_for(const std::shared_ptr<Outcome> &outcome, int timeout_ms)
{
    for (int waited = 0; !outcome->done.load() && waited < timeout_ms; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return outcome->done.load();
}

// ---- checks before a shortcut is kept (any task but the LVGL task) -----------------------
// Home steps take the hub's names for their targets (a room's id, a device's
// target) when Home can tell, so a near name ("bedroom") keeps working after
// another room comes along. False: Home knows and there is no such thing.
bool resolve_home_steps(Shortcut &sc, std::string &error)
{
    bool ok = true;
    int index = 0;
    each_node(sc.steps, [&](Node & n) {
        ++index;
        if (!ok || n.kind != Do::Home) {
            return;
        }
        Action &a = n.act;
        on_ui([&] {
            if (!home_app_ready()) {
                return;  // checked when it runs
            }
            std::string what, resolved;
            if (!home_app_resolve(a.verb.c_str(), a.target.c_str(), a.value.c_str(), what, resolved))
            {
                error = "a home step (" + action_text(a) + "): " + what;
                ok = false;
            } else if (a.verb == "scene" && !resolved.empty())
            {
                a.value = resolved;  // the scene's id
                a.target.clear();
            } else if (!resolved.empty())
            {
                a.target = resolved;
            }
        });
    });
    return ok;
}

// The devices that conditions and triggers name are Home's (when it answers).
bool check_home_names(const Shortcut &sc, std::string &error)
{
    std::vector<std::string> names;
    auto add_cond = [&](const Cond & c) {
        each_cond(c, [&](const Cond & k) {
            if (k.kind == Check::Home) {
                names.push_back(k.name);
            }
        });
    };
    each_node(sc.steps, [&](const Node & n) {
        if (n.kind == Do::If) {
            add_cond(n.cond);
            for (const Branch &b : n.elifs) {
                add_cond(b.cond);
            }
        } else if (n.kind == Do::Repeat && !n.n) {
            add_cond(n.cond);
        }
    });
    for (const Trigger &t : sc.triggers)
        if (t.kind == On::Home) {
            names.push_back(t.name);
        }
    if (names.empty()) {
        return true;
    }
    bool ready = false;
    std::string states;
    on_ui([&] {
        ready = home_app_ready();
        if (ready) {
            states = home_app_states_json();
        }
    });
    if (!ready) {
        return true;  // checked when it runs
    }
    JsonDocument doc;
    deserializeJson(doc, states);
    for (const std::string &name : names) {
        Reading r;
        if (!reading_of(doc.as<JsonArrayConst>(), name, r)) {
            error = "Swifty Home has no device or room \"" + name + "\" (it has: " + home_names(doc.as<JsonArrayConst>()) + ")";
            return false;
        }
    }
    return true;
}

// Run steps name another shortcut: kept by its id (and its name, for reading).
bool resolve_runs_locked(Shortcut &sc, std::string &error)
{
    bool ok = true;
    each_node(sc.steps, [&](Node & n) {
        if (!ok || n.kind != Do::Run) {
            return;
        }
        Ptr target = n.n ? find_locked(uint32_t(n.n)) : nullptr;
        if (!target && !n.text.empty()) {
            target = by_name_locked(n.text);
        }
        if (!target) {
            error = "run: there is no shortcut " + (n.text.empty() ? std::to_string(n.n) : "\"" + n.text + "\"");
            ok = false;
        } else if (sc.id && target->id == sc.id) {
            error = "run: a shortcut cannot run itself";
            ok = false;
        } else {
            n.n = int(target->id);
            n.text = target->name;
        }
    });
    return ok;
}

// Adds sc (id 0) or replaces the one with its id. resolve: ask Home for its
// names (not on a small stack: see rpc()). approve: asked last, once
// everything else is right (a card on the screen). False: why not, in error.
bool store(Shortcut sc, std::string &error, uint32_t *id_out, bool resolve, const std::function<bool(const Shortcut &)> &approve = nullptr)
{
    if (resolve && (!resolve_home_steps(sc, error) || !check_home_names(sc, error))) {
        return false;
    }
    auto check_locked = [&]() {
        if (!resolve_runs_locked(sc, error)) {
            return false;
        }
        if (sc.id && !find_locked(sc.id)) {
            error = "there is no shortcut " + std::to_string(sc.id);
            return false;
        }
        for (const Ptr &p : list)
            if (p->id != sc.id && !strcasecmp(p->name.c_str(), sc.name.c_str())) {
                error = "there is a shortcut named \"" + p->name + "\" already (id " + std::to_string(p->id) +
                        "): give its id to change it, or another name";
                return false;
            }
        if (!sc.id && list.size() >= MAX_COUNT) {
            error = "there are " + std::to_string(MAX_COUNT) + " shortcuts already: delete one first";
            return false;
        }
        return true;
    };
    {
        std::lock_guard<std::mutex> g(lock);
        if (!check_locked()) {
            return false;
        }
    }
    if (approve && !approve(sc)) {
        error = "not allowed on the board's screen";
        return false;
    }
    std::lock_guard<std::mutex> g(lock);
    if (!check_locked()) {  // again: the list may have changed while the card waited
        return false;
    }
    const bool fresh = sc.id == 0;
    if (fresh) {
        sc.id = next_id++;
    }
    const uint32_t id = sc.id;
    Ptr ptr = std::make_shared<const Shortcut>(std::move(sc));
    Ptr old;
    if (fresh) {
        list.push_back(ptr);
    } else {
        for (Ptr &p : list)
            if (p->id == id) {
                old = p;
                p = ptr;
            }
    }
    if (stored_json_locked().size() > MAX_STORED) {
        error = "too much to keep on the board: make it shorter, or delete a shortcut";
        if (fresh) {
            list.pop_back();
        } else {
            for (Ptr &p : list)
                if (p->id == id) {
                    p = old;
                }
        }
        return false;
    }
    *id_out = id;
    changed_locked();
    return true;
}

bool remove_one(uint32_t id, std::string &name)
{
    std::lock_guard<std::mutex> g(lock);
    auto it = std::find_if(list.begin(), list.end(), [id](const Ptr & p) {
        return p->id == id;
    });
    if (it == list.end()) {
        return false;
    }
    name = (*it)->name;
    list.erase(it);
    statuses.erase(id);
    for (const Active &a : active)
        if (a.key == id) {
            a.stop->store(true);  // a run of it ends at its next step
        }
    changed_locked();
    return true;
}

// A changed copy in place of one (enabled, its tile). No checks: nothing
// that needs them changes.
template <typename F> bool modify(uint32_t id, std::string &error, F change)
{
    std::lock_guard<std::mutex> g(lock);
    for (Ptr &p : list)
        if (p->id == id) {
            auto copy = std::make_shared<Shortcut>(*p);
            change(*copy);
            p = copy;
            changed_locked();
            return true;
        }
    error = "there is no shortcut " + std::to_string(id);
    return false;
}

// ---- starters ----------------------------------------------------------------------------
Shortcut lights_off(const std::string &name, const std::string &target)
{
    Shortcut sc;
    sc.name = trimmed(name, MAX_NAME);
    sc.icon = "power";
    sc.accent = 0;
    sc.triggers.push_back(Trigger());
    Node n;
    n.kind = Do::Home;
    n.act.kind = Do::Home;
    n.act.verb = "lights_off";
    n.act.target = target;
    sc.steps.push_back(n);
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
        std::string name = r["name"] | "";
        const std::string low = lowered(name);
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
        const bool have = std::any_of(list.begin(), list.end(), [&](const Ptr & x) {
            return !strcasecmp(x->name.c_str(), s.name.c_str());
        });
        if (have || list.size() >= MAX_COUNT) {
            continue;
        }
        auto sc = std::make_shared<Shortcut>(s);
        sc->id = next_id++;
        list.push_back(sc);
        ++added;
    }
    seeded = true;
    changed_locked();
    return added;
}

// ---- triggers (LVGL task) ----------------------------------------------------------------
struct {
    int64_t minute = -1;  // the last local minute looked at (minutes since 1970)
    bool board = false;   // watching the Interaction board
    bool inputs_valid = false;
    tools_board_inputs_t inputs = {};
    std::map<uint32_t, int64_t> motion_ran;  // when each shortcut last ran on motion (ms since start-up)
    int64_t home_next = 0;
    std::map<std::string, float> home_seen;  // "<id>/<trigger>": last on (0, 1) or temperature
    uint32_t list_seen = 0;
} W;

void fire(const Ptr &p, Source source)
{
    std::string why;
    if (enqueue(p, p->id, false, source, nullptr, &why)) {
        ESP_LOGI(TAG, "\"%s\" started by %s", p->name.c_str(), source_name(source));
    } else {
        ESP_LOGI(TAG, "\"%s\" not started by %s: %s", p->name.c_str(), source_name(source), why.c_str());
    }
}

void watch_triggers()
{
    std::vector<Ptr> autos;
    Location loc;
    uint32_t seen;
    {
        std::lock_guard<std::mutex> g(lock);
        for (const Ptr &p : list)
            if (p->enabled && p->automatic()) {
                autos.push_back(p);
            }
        loc = location;
        seen = list_rev;
    }
    if (seen != W.list_seen) {
        W.list_seen = seen;
        W.home_seen.clear();  // triggers may name other devices now
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    auto has = [&](const Ptr & p, On kind) {
        return std::any_of(p->triggers.begin(), p->triggers.end(), [kind](const Trigger & t) {
            return t.kind == kind;
        });
    };

    // Schedules, minute by minute. Minutes passed while the board was off,
    // asleep or before its clock was set are not made up.
    const time_t now = time(nullptr);
    if (now >= CLOCK_SET) {
        const int64_t minute = int64_t(now) / 60;
        if (W.minute < 0 || minute < W.minute || minute - W.minute > 3) {
            W.minute = minute;
        }
        while (W.minute < minute) {
            ++W.minute;
            const time_t start = time_t(W.minute * 60);
            tm local;
            localtime_r(&start, &local);
            for (const Ptr &p : autos)
                for (const Trigger &t : p->triggers)
                    if (fires_at(t, local, start, loc)) {
                        fire(p, t.kind == On::Time ? Source::Time : t.kind == On::Every ? Source::Every : Source::Sun);
                        break;
                    }
        }
    }

    // Start-up: each shortcut that was there with a boot trigger, once.
    for (auto it = boot_ids.begin(); it != boot_ids.end();) {
        Ptr p;
        for (const Ptr &x : autos)
            if (x->id == *it && has(x, On::Boot)) {
                p = x;
            }
        const auto t = p ? std::find_if(p->triggers.begin(), p->triggers.end(), [](const Trigger & x) {
            return x.kind == On::Boot;
        }) : std::vector<Trigger>::const_iterator();
        if (!p || now_ms > 3700 * 1000LL) {
            it = boot_ids.erase(it);  // gone, off, or start-up is long past
        } else if (now_ms >= t->n * 1000LL) {
            fire(p, Source::Boot);
            it = boot_ids.erase(it);
        } else {
            ++it;
        }
    }

    // The Interaction board: motion and its buttons, by their counts.
    const bool want_board = std::any_of(autos.begin(), autos.end(), [&](const Ptr & p) {
        return has(p, On::Motion) || has(p, On::Button);
    });
    if (want_board != W.board) {
        W.board = want_board;
        W.inputs_valid = false;
        tools_board_watch(want_board);
    }
    tools_board_inputs_t in;
    if (want_board && tools_board_inputs(&in, 0)) {
        const tools_board_inputs_t &was = W.inputs;
        const bool reset = in.motions < was.motions || in.left_presses < was.left_presses || in.right_presses < was.right_presses;
        if (W.inputs_valid && !reset) {
            const bool motion = in.motions > was.motions, left = in.left_presses > was.left_presses,
                       right = in.right_presses > was.right_presses;
            for (const Ptr &p : autos)
                for (const Trigger &t : p->triggers) {
                    if (t.kind == On::Motion && motion) {
                        auto ran = W.motion_ran.find(p->id);
                        if (ran == W.motion_ran.end() || now_ms - ran->second >= t.n * 1000LL) {
                            W.motion_ran[p->id] = now_ms;
                            fire(p, Source::Motion);
                            break;
                        }
                    } else if (t.kind == On::Button && ((t.name != "right" && left) || (t.name != "left" && right))) {
                        fire(p, Source::Button);
                        break;
                    }
                }
        }
        W.inputs = in;
        W.inputs_valid = true;
    } else {
        W.inputs_valid = false;
    }

    // Home: a device turning on or off, or crossing a temperature, every 3 s
    // from what Home already polls.
    const bool want_home = std::any_of(autos.begin(), autos.end(), [&](const Ptr & p) {
        return has(p, On::Home);
    });
    if (want_home && now_ms >= W.home_next) {
        W.home_next = now_ms + 3000;
        if (H.home_start) {
            H.home_start();
        }
        if (home_app_ready()) {
            JsonDocument doc;
            deserializeJson(doc, home_app_states_json());
            for (const Ptr &p : autos) {
                bool fired = false;
                for (size_t i = 0; i < p->triggers.size(); ++i) {
                    const Trigger &t = p->triggers[i];
                    Reading r;
                    if (t.kind != On::Home || !reading_of(doc.as<JsonArrayConst>(), t.name, r)) {
                        continue;
                    }
                    const bool temp = t.cmp == Cmp::Above || t.cmp == Cmp::Below;
                    if (temp ? !r.has_temp : !r.has_on) {
                        continue;
                    }
                    const float v = temp ? r.temp : r.on ? 1.0f : 0.0f;
                    const std::string key = std::to_string(p->id) + "/" + std::to_string(i);
                    auto it = W.home_seen.find(key);
                    if (it != W.home_seen.end() && !fired) {
                        const float x = it->second;
                        const bool go = t.cmp == Cmp::IsOn ? x < 0.5f && v >= 0.5f : t.cmp == Cmp::IsOff ? x >= 0.5f && v < 0.5f
                                        : t.cmp == Cmp::Above ? x <= t.num && v > t.num : t.cmp == Cmp::Below ? x >= t.num && v < t.num
                                        : x != v;
                        if (go) {
                            fire(p, Source::Home);
                            fired = true;
                        }
                    }
                    W.home_seen[key] = v;
                }
            }
        }
    }
}

// ---- the LVGL timer: save, tiles, starters, triggers -------------------------------------
uint32_t published = 0;

void check(lv_timer_t *)
{
    save();
    uint32_t seen;
    bool offer;
    {
        std::lock_guard<std::mutex> g(lock);
        seen = list_rev;
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
    if (seen != published) {
        published = seen;
        tiles_publish();
    }
    watch_triggers();
    ui_tick();
}

// ---- text for Swifty and RPC 55 ----------------------------------------------------------
std::string triggers_text(const Shortcut &sc)
{
    if (sc.triggers.empty()) {
        return "run by hand (Swifty, the editor or another shortcut)";
    }
    std::string out;
    for (const Trigger &t : sc.triggers) {
        out += (out.empty() ? "" : "; ") + trigger_text(t);
    }
    return out;
}

std::string status_text(uint32_t id)
{
    const RunStatus st = status_of(id);
    if (st.running) {
        return "running now";
    }
    if (!st.ran) {
        return "";
    }
    return std::string(st.ok ? "ok" : "failed") + (st.dry ? " (test)" : "") + (st.at ? " " + when_text(st.at) : "") +
           " (" + source_name(st.source) + "): " + st.message;
}

// ask_home: Home's words for its steps (takes the display lock: not RPC 55).
std::string steps_text(const Shortcut &sc, bool ask_home, const char *joint)
{
    std::vector<std::string> lines;
    if (ask_home) {
        steps_lines(sc.steps, lines, [](const Action & a) {
            std::string words;
            on_ui([&] {
                words = action_words(a);
            });
            return words;
        });
    } else {
        steps_lines(sc.steps, lines);
    }
    std::string out;
    for (const std::string &l : lines) {
        out += (out.empty() ? "" : joint) + (ask_home ? trimmed(l, 200) : l);
    }
    return out;
}

std::string list_text(bool ask_home)
{
    std::vector<Ptr> copy = list_copy();
    if (copy.empty()) {
        return "No shortcuts.";
    }
    std::string text;
    for (const Ptr &p : copy) {
        const std::string last = status_text(p->id), next = next_text(*p);
        text += "Shortcut " + std::to_string(p->id) + ": " + p->name + (p->enabled ? "" : " (off)") + ". Starts: " +
                triggers_text(*p) + ". Does: " + steps_text(*p, ask_home, "; ") + "." + (next.empty() ? "" : " Next: " + next + ".") +
                (last.empty() ? "" : " Last run: " + last) + "\n";
    }
    return text;
}

std::string log_text(uint32_t id)
{
    const RunStatus st = status_of(id);
    std::string out = status_text(id);
    if (out.empty()) {
        return id ? "It has not run since the board started." : "No test yet.";
    }
    for (const std::string &l : st.log) {
        out += "\n" + l;
    }
    return out;
}

// ---- Swifty's tools (its agent task) -----------------------------------------------------
Ptr tool_target(JsonDocument &a)
{
    uint32_t id = a["id"] | 0u;
    std::lock_guard<std::mutex> g(lock);
    Ptr p = id ? find_locked(id) : nullptr;
    if (!p && a["name"].is<const char *>()) {
        p = by_name_locked(a["name"].as<const char *>());
    }
    return p;
}

void put(char *out, size_t cap, const std::string &text)
{
    strlcpy(out, text.c_str(), cap);
}

// Swifty asks; the person allows it on the board when it starts by itself.
bool allowed_on_screen(const Shortcut &sc, bool change)
{
    std::vector<std::string> lines;
    steps_lines(sc.steps, lines);
    std::string what;
    for (size_t i = 0; i < lines.size() && i < 4; ++i) {
        what += (i ? "\n" : "") + trimmed(lines[i], 60);
    }
    if (lines.size() > 4) {
        what += "\n\xE2\x80\xA6";
    }
    return ask_screen(sc.name, std::string("Swifty wants this to run by itself: ") + triggers_text(sc) + ".\n" + what,
                      change ? "Allow change" : "Allow");
}

std::string saved_text(uint32_t id, bool changed)
{
    Ptr p = find(id);
    if (!p) {
        return "Saved.";
    }
    const std::string next = next_text(*p);
    return "Shortcut " + std::to_string(id) + " \"" + p->name + "\" " + (changed ? "changed" : "made") +
           (p->enabled ? "" : " (off)") + ". Starts: " + triggers_text(*p) + ". Does: " + steps_text(*p, true, "; ") + "." +
           (p->tile() ? " Its tile is on the home screen." : "") + (next.empty() || !p->enabled ? "" : " Next: " + next + ".");
}

void add_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    if (read_json(a, args, strlen(args))) {
        put(out, cap, "Not saved: the arguments are not JSON.");
        return;
    }
    JsonObjectConst o = a["shortcut"].is<JsonObjectConst>() ? a["shortcut"].as<JsonObjectConst>() : a.as<JsonObjectConst>();
    Shortcut sc;
    std::string error;
    if (!parse_shortcut(o, sc, error)) {
        put(out, cap, "Not saved: " + error + ". Fix it and call add_shortcut again.");
        return;
    }
    const bool change = sc.id != 0;
    uint32_t id = 0;
    std::function<bool(const Shortcut &)> approve;
    if (sc.enabled && sc.automatic()) {
        approve = [change](const Shortcut & s) {
            return allowed_on_screen(s, change);
        };
    }
    if (!store(sc, error, &id, true, approve)) {
        put(out, cap, error == "not allowed on the board's screen"
            ? "Not saved: it starts by itself, and nobody allowed it on the board's screen within a minute. "
            "Tell the person; it is asked again when you retry."
            : "Not saved: " + error + ".");
        return;
    }
    put(out, cap, saved_text(id, change));
}

void list_tool(const char *, char *out, size_t cap)
{
    std::string text = list_text(true);
    if (text.size() >= cap) {
        text = list_text(false);  // Home's words are longer
    }
    put(out, cap, text);
}

void get_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    read_json(a, args, strlen(args));
    Ptr p = tool_target(a);
    if (!p) {
        put(out, cap, "No such shortcut (list_shortcuts shows them).");
        return;
    }
    JsonDocument doc;
    shortcut_json(*p, doc.to<JsonObject>());
    put(out, cap, json_text(doc) + "\nLast run: " + log_text(p->id));
}

void run_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    read_json(a, args, strlen(args));
    Ptr p = tool_target(a);
    if (!p) {
        put(out, cap, "No such shortcut (list_shortcuts shows them).");
        return;
    }
    const bool dry = a["dry_run"] | false;
    auto outcome = std::make_shared<Outcome>();
    std::string why;
    if (!enqueue(p, p->id, dry, Source::Swifty, outcome, &why)) {
        put(out, cap, why);
        return;
    }
    if (!wait_for(outcome, dry ? 40000 : 120000)) {
        put(out, cap, "\"" + p->name + "\" is still running.");
        return;
    }
    std::string text = "\"" + p->name + "\" " + (outcome->ok ? (dry ? "tested" : "ran") : "failed") + ": " + outcome->message + ".";
    for (size_t i = 0; i < outcome->log.size() && i < 16; ++i) {
        text += "\n" + outcome->log[i];
    }
    put(out, cap, text);
}

void enable_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    read_json(a, args, strlen(args));
    Ptr p = tool_target(a);
    if (!p) {
        put(out, cap, "No such shortcut (list_shortcuts shows them).");
        return;
    }
    const bool on = a["on"] | true;
    if (on && !p->enabled && p->automatic() && !allowed_on_screen(*p, true)) {
        put(out, cap, "Not switched on: nobody allowed it on the board's screen within a minute.");
        return;
    }
    std::string error;
    if (!set_enabled(p->id, on, error)) {
        put(out, cap, error);
        return;
    }
    put(out, cap, "\"" + p->name + "\" is " + (on ? "on" : "off") + (p->automatic() ? (on ? ": it starts by itself (" + triggers_text(*p) + ")." :
            ": only a tap or run_shortcut starts it.") : "."));
}

void delete_tool(const char *args, char *out, size_t cap)
{
    JsonDocument a;
    read_json(a, args, strlen(args));
    Ptr p = tool_target(a);
    std::string name;
    if (!p || !remove_one(p->id, name)) {
        put(out, cap, "There is no such shortcut (list_shortcuts shows them).");
        return;
    }
    put(out, cap, "Deleted shortcut " + std::to_string(p->id) + ", \"" + name + "\"" + (p->tile() ? ", and its tile." : "."));
}

void register_tools()
{
    static const std::string add_schema =
        std::string(R"({"type":"object","properties":{)"
                    R"("id":{"type":"integer","description":"Change this shortcut instead of making one: send all of it (get_shortcut gives it)."},)"
                    R"("name":{"type":"string","description":"Short, on its tile: e.g. Bedroom lights off."},)"
                    R"("icon":{"type":"string","enum":[)") +
    [] {
        std::string s;
        for (const char *i : ICONS) {
            s += (s.empty() ? "\"" : ",\"") + std::string(i) + "\"";
        }
        return s;
    }() +
    R"(]},"accent":{"type":"string","enum":["violet","cyan","amber","green"]},)"
    R"("enabled":{"type":"boolean","description":"Default true. Off: only a tap or run_shortcut starts it."},)"
    R"("triggers":{"type":"array","description":"What starts it; default [{\"on\":\"tile\"}] (a tile to tap). )"
    R"(tile; time {at:\"07:30\",days}; every {minutes,from,to,days}; boot {delay s}; motion {cooldown s}; )"
    R"(button {button:left|right|any}; home {device,to:on|off|any} or {device,above|below:degrees}; )"
    R"(sun {event:sunrise|sunset,offset:min,days}. days: [\"mon\",\"sat\"], \"weekdays\" or \"weekend\"; absent: every day.",)"
    R"("items":{"type":"object","properties":{"on":{"type":"string","enum":["tile","time","every","boot","motion","button","home","sun"]}},"required":["on"]}},)"
    R"("steps":{"type":"array","description":"Run in order, 48 in all at most. Actions: home {verb,target,value}; ir {key}; )"
    R"(board_lights {mode,colour}; brightness {percent}; open {app,tool}; flashlight {on}. )"
    R"(wait {seconds|minutes, up to 60 min}; message {text,haptic,sound} ({n} in text shows variable n); )"
    R"(if {if:COND,then:[...],elif:[{if:COND,then:[...]}],else:[...]}; repeat {times:1-100,steps:[...]} or )"
    R"({while:COND,max:1-100,steps:[...]}; set {var,value}; add {var,value} (whole numbers); )"
    R"(run {shortcut: another's name}; stop. Ifs and repeats nest 4 deep. )"
    R"(COND: {check:time,from:\"22:00\",to:\"06:00\"} | {check:day,days} | {check:home,device,is:on|off} or above|below )"
    R"(| {check:motion,within:s} | {check:button,button} | {check:light,below:%} | {check:wifi} | {check:vpn} )"
    R"(| {check:battery,below:%} | {check:charging} | {check:var,var,equals|above|below} | {and:[...]} | {or:[...]} | {not:COND}.",)"
    R"("items":{"type":"object","properties":{"do":{"type":"string","enum":["home","ir","board_lights","brightness","open",)"
    R"("flashlight","wait","if","repeat","set","add","message","run","stop"]},)"
    R"("verb":{"type":"string","description":"home: lights_on, lights_off, lights_toggle, brightness, color, scene, plug_on, plug_off, plug_toggle, tv_on, tv_off, tv_toggle, volume, play, pause, mute, unmute, or another of Swifty Home's commands"},)"
    R"("target":{"type":"string","description":"home: a room, light or plug by name, or all"},)"
    R"("value":{"description":"home: brightness or volume 0-100, a colour, a scene; set/add: a number"},)"
    R"("key":{"type":"string","description":"ir: power, volume_up, volume_down, mute, input, ok, up, down, left, right, back, home, channel_up, channel_down"},)"
    R"("app":{"type":"string","description":"open: launcher, glimdock, home, camera, swifty, files, tools, gallery, webcam, interaction, calculator"}},)"
    R"("required":["do"]}}},"required":["name","steps"]})";
    swifty_app_add_tool("add_shortcut",
                        "Make or change a shortcut: a small program the board runs, like Siri Shortcuts: on a tile tap, at "
                        "times, every N minutes, at start-up, sunrise or sunset, on the Interaction board's motion or "
                        "buttons, or when a Swifty Home device changes. Steps: Home commands (home_status names the rooms "
                        "and devices), waits, TV remote keys, the board's lights, brightness, apps, the flashlight, "
                        "messages, ifs, repeats, variables and other shortcuts. The board checks it and says what to fix. "
                        "One that starts by itself needs the person to tap Allow on the board's screen; say so. "
                        "Example: {\"name\":\"Night\",\"triggers\":[{\"on\":\"time\",\"at\":\"23:00\",\"days\":\"weekdays\"}],"
                        "\"steps\":[{\"do\":\"if\",\"if\":{\"check\":\"home\",\"device\":\"TV\",\"is\":\"on\"},"
                        "\"then\":[{\"do\":\"message\",\"text\":\"TV still on\",\"sound\":true}],"
                        "\"else\":[{\"do\":\"home\",\"verb\":\"lights_off\",\"target\":\"all\"}]}]}",
                        add_schema.c_str(), add_tool);
    swifty_app_add_tool("list_shortcuts",
                        "The shortcuts: numbers (ids, kept when others are deleted), names, on or off, what starts them, "
                        "their steps, the next scheduled run and how the last run went.",
                        R"({"type":"object","properties":{},"required":[]})", list_tool);
    swifty_app_add_tool("get_shortcut", "One shortcut as JSON (to change it with add_shortcut) and its last run's log.",
                        R"({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string","description":"Instead of id"}},"required":[]})",
                        get_tool);
    swifty_app_add_tool("run_shortcut",
                        "Run a shortcut now, as a tap on its tile does, and say how it went, step by step. It switches real "
                        "devices; dry_run true checks every step and sends nothing (waits are skipped).",
                        R"({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string","description":"Instead of id"},"dry_run":{"type":"boolean"}},"required":[]})",
                        run_tool);
    swifty_app_add_tool("enable_shortcut",
                        "Switch a shortcut on or off. Off, its schedules and sensors no longer start it (a tap still does).",
                        R"({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string","description":"Instead of id"},"on":{"type":"boolean"}},"required":["on"]})",
                        enable_tool);
    swifty_app_add_tool("delete_shortcut", "Delete a shortcut (and its tile), by its number from list_shortcuts. Ask first.",
                        R"({"type":"object","properties":{"id":{"type":"integer"},"name":{"type":"string","description":"Instead of id"}},"required":[]})",
                        delete_tool);
}

// ---- Web settings and RPC 55 (their tasks) -----------------------------------------------
template <size_t N> void names_json(JsonArray a, const char *const(&names)[N])
{
    for (const char *n : names) {
        a.add(n);
    }
}

void status_json(uint32_t id, JsonObject o)
{
    const RunStatus st = status_of(id);
    o["running"] = st.running;
    if (!st.ran) {
        return;
    }
    o["ok"] = st.ok;
    o["dry"] = st.dry;
    o["source"] = source_name(st.source);
    if (st.at) {
        o["at"] = when_text(st.at);
    }
    o["message"] = st.message;
    JsonArray log = o["log"].to<JsonArray>();
    for (const std::string &l : st.log) {
        log.add(l);
    }
}

// A test: a dry run of an unsaved shortcut (or a saved one), waited for.
std::string test_reply(Ptr sc, uint32_t key, bool *ok_out, bool wait)
{
    auto outcome = std::make_shared<Outcome>();
    std::string why;
    if (!enqueue(sc, key, true, Source::Test, outcome, &why)) {
        return error_json(why);
    }
    JsonDocument out;
    out["reply"] = "ok";
    *ok_out = true;
    if (!wait) {
        out["queued"] = true;
        return json_text(out);
    }
    if (!wait_for(outcome, 25000)) {
        out["message"] = "Still running: its log comes with the list.";
        return json_text(out);
    }
    out["ok"] = outcome->ok;
    out["message"] = outcome->message;
    JsonArray log = out["log"].to<JsonArray>();
    for (const std::string &l : outcome->log) {
        log.add(l);
    }
    return json_text(out);
}

// light: from RPC 55, whose reply waits: nothing that asks Home (names are
// then resolved when the shortcut runs; the starters wait for the LVGL timer)
// and nothing that waits for a run.
std::string apply(const char *json, size_t length, bool *ok_out, bool light)
{
    JsonDocument in, out;
    *ok_out = false;
    if (read_json(in, json, length) || !in.is<JsonObject>()) {
        return error_json("expected a JSON object");
    }
    const std::string op = in["op"] | "";
    std::string error;
    if (op == "save") {
        Shortcut sc;
        uint32_t id = 0;
        if (!parse_shortcut(in["shortcut"].as<JsonObjectConst>(), sc, error) || !store(sc, error, &id, !light)) {
            return error_json(error);
        }
        out["id"] = id;
    } else if (op == "test") {
        Ptr sc;
        uint32_t key = 0;
        if (in["shortcut"].is<JsonObjectConst>()) {
            auto made = std::make_shared<Shortcut>();
            if (!parse_shortcut(in["shortcut"].as<JsonObjectConst>(), *made, error) ||
                    (!light && (!resolve_home_steps(*made, error) || !check_home_names(*made, error)))) {
                return error_json(error);
            }
            {
                std::lock_guard<std::mutex> g(lock);
                if (!resolve_runs_locked(*made, error)) {
                    return error_json(error);
                }
            }
            sc = made;
        } else {
            key = in["id"] | 0u;
            sc = find(key);
            if (!sc) {
                return error_json("no such shortcut");
            }
        }
        return test_reply(sc, key, ok_out, !light);
    } else if (op == "delete") {
        std::string name;
        if (!remove_one(in["id"] | 0u, name)) {
            return error_json("no such shortcut");
        }
    } else if (op == "run" || op == "fire") {
        // fire: as a trigger would start it (quiet, asking first), to try automations.
        static const char *const FIRES[] = {"time", "every", "boot", "motion", "button", "home", "sun"};
        Source source = op == "run" ? (light ? Source::Rpc : Source::Web) : Source::Time;
        if (op == "fire") {
            const std::string as = in["as"] | "time";
            for (int i = 0; i < 7; ++i)
                if (as == FIRES[i]) {
                    source = Source(int(Source::Time) + i);
                }
        }
        Ptr p = find(in["id"] | 0u);
        std::string why = "no such shortcut";
        if (!p || !enqueue(p, p->id, in["dry_run"] | false, source, nullptr, &why)) {
            return error_json(why);
        }
    } else if (op == "stop") {
        stop(in["id"] | 0u);
    } else if (op == "enable") {
        Ptr p = find(in["id"] | 0u);
        if (!p || !set_enabled(p->id, in["on"] | true, error)) {
            return error_json(p ? error : "no such shortcut");
        }
    } else if (op == "move") {
        std::lock_guard<std::mutex> g(lock);
        const uint32_t id = in["id"] | 0u;
        auto it = std::find_if(list.begin(), list.end(), [id](const Ptr & p) {
            return p->id == id;
        });
        if (it == list.end()) {
            return error_json("no such shortcut");
        }
        const size_t to = std::min<size_t>(size_t(std::max(0, in["to"] | 0)), list.size() - 1);
        Ptr p = *it;
        list.erase(it);
        list.insert(list.begin() + long(to), p);
        changed_locked();
    } else if (op == "location") {
        std::lock_guard<std::mutex> g(lock);
        if (in["lat"].isNull()) {
            location = Location();
        } else {
            const float lat = in["lat"] | 1000.0f, lon = in["lon"] | 1000.0f;
            if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
                return error_json("latitude -90 to 90, longitude -180 to 180 (decimal degrees)");
            }
            location.set = true;
            location.lat = lat;
            location.lon = lon;
        }
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
            return error_json("Swifty Home has not answered yet (or has no lights)");
        }
        out["added"] = add_starters(starters);
    } else if (op == "editor") {
        // Opens the board's editor (a test hook, as RPC 30 opens apps).
        if (!on_ui([] { lv_async_call([](void *)
    {
        ui_editor_open();
        }, nullptr);
                      }, 1000)) {
            return error_json("the screen is busy");
        }
    } else if (op == "dry_run") {
        dry_all.store(in["on"] | true);
        out["dry_run"] = dry_all.load();
    } else {
        return error_json("op: save, test, delete, run, fire, stop, enable, move, location, starters, editor or dry_run");
    }
    out["reply"] = "ok";
    *ok_out = true;
    return json_text(out);
}

// A change from RPC 55 runs on a helper task: the RPC task's 4 KB stack is
// too small for a shortcut's nested JSON and checks. The reply waits for it.
struct RpcChange {
    std::string json, reply;
    TaskHandle_t caller;
};

void rpc_change_task(void *arg)
{
    auto *held = static_cast<std::shared_ptr<RpcChange> *>(arg);
    std::shared_ptr<RpcChange> job = *held;
    delete held;
    bool ok;
    job->reply = apply(job->json.data(), job->json.size(), &ok, true);
    xTaskNotifyGive(job->caller);
    vTaskDeleteWithCaps(nullptr);  // its stack is in PSRAM
}

std::string rpc_change(const char *json, size_t length)
{
    auto job = std::make_shared<RpcChange>();
    job->json.assign(json, length);
    job->caller = xTaskGetCurrentTaskHandle();
    auto *held = new std::shared_ptr<RpcChange>(job);
    ulTaskNotifyTake(pdTRUE, 0);
    if (xTaskCreatePinnedToCoreWithCaps(rpc_change_task, "shortcuts-rpc", 12288, held, 2, nullptr, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete held;
        return error_json("out of memory");
    }
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))) {
        return error_json("no answer in time");
    }
    return job->reply;
}

// Payload: a JSON change as Web settings sends (see shortcuts.h), a question
// ({"op":"list"}, {"op":"get","id":N}, {"op":"log","id":N}, {"op":"next"};
// "from": a later page of a long answer), or nothing for the list. Replies
// are text, cut to the reply's size.
esp_err_t rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t cap, size_t *size, void *)
{
    std::string reply;
    JsonDocument op;
    // Questions are flat; anything nested deeper is a change, read on the helper task.
    const bool parsed = request->payload_size &&
                        !deserializeJson(op, reinterpret_cast<const char *>(request->payload), request->payload_size,
                                         DeserializationOption::NestingLimit(2));
    const std::string what = parsed ? std::string(op["op"] | "") : std::string(request->payload_size ? "" : "list");
    if (what == "list") {
        reply = list_text(false);
        if (dry_all.load()) {
            reply += "Every run is a dry run.\n";
        }
    } else if (what == "get") {
        Ptr p = find(op["id"] | 0u);
        if (p) {
            JsonDocument doc;
            shortcut_json(*p, doc.to<JsonObject>());
            reply = json_text(doc);
        } else {
            reply = "No such shortcut.";
        }
    } else if (what == "log") {
        reply = log_text(op["id"] | 0u);
    } else if (what == "next") {
        for (const Ptr &p : list_copy()) {
            const std::string next = next_text(*p);
            if (p->enabled && !next.empty()) {
                reply += std::to_string(p->id) + " " + p->name + ": " + next + "\n";
            }
        }
        std::lock_guard<std::mutex> g(lock);
        if (reply.empty()) {
            reply = "Nothing scheduled.";
        }
        if (location.set) {
            char t[64];
            snprintf(t, sizeof(t), "Location %.2f, %.2f\n", double(location.lat), double(location.lon));
            reply += t;
        }
    } else {
        reply = rpc_change(reinterpret_cast<const char *>(request->payload), request->payload_size);
    }
    const size_t from = std::min<size_t>(parsed ? size_t(op["from"] | 0) : 0, reply.size());
    *size = std::min(cap, reply.size() - from);
    memcpy(out, reply.data() + from, *size);
    return ESP_OK;
}
}  // namespace

// ---- the engine, for the screens (shortcuts_internal.h) ----------------------------------
std::vector<std::shared_ptr<const Shortcut>> list_copy()
{
    std::lock_guard<std::mutex> g(lock);
    return list;
}

std::shared_ptr<const Shortcut> find(uint32_t id)
{
    std::lock_guard<std::mutex> g(lock);
    return find_locked(id);
}

RunStatus status_of(uint32_t id)
{
    std::lock_guard<std::mutex> g(lock);
    auto it = statuses.find(id);
    return it == statuses.end() ? RunStatus() : it->second;
}

bool start(uint32_t id, bool dry, Source source)
{
    Ptr p = find(id);
    std::string why;
    return p && enqueue(p, id, dry, source, nullptr, &why);
}

void stop(uint32_t id)
{
    std::lock_guard<std::mutex> g(lock);
    for (const Active &a : active)
        if (a.key == id) {
            a.stop->store(true);
        }
    // One still waiting goes without running.
    for (auto it = jobs.begin(); it != jobs.end();) {
        if (it->key == id) {
            if (it->outcome) {
                it->outcome->message = "Stopped.";
                it->outcome->done.store(true);
            }
            statuses[id].running = false;
            it = jobs.erase(it);
            ++rev;
        } else {
            ++it;
        }
    }
}

bool set_enabled(uint32_t id, bool on, std::string &error)
{
    return modify(id, error, [on](Shortcut & sc) {
        sc.enabled = on;
    });
}

bool set_tile(uint32_t id, bool on, std::string &error)
{
    return modify(id, error, [on](Shortcut & sc) {
        auto &ts = sc.triggers;
        ts.erase(std::remove_if(ts.begin(), ts.end(), [](const Trigger & t) {
            return t.kind == On::Tile;
        }), ts.end());
        if (on) {
            ts.insert(ts.begin(), Trigger());
        }
    });
}

bool erase(uint32_t id, std::string &name)
{
    return remove_one(id, name);
}

bool add_made(Shortcut sc, std::string &error)
{
    uint32_t id = 0;
    return store(std::move(sc), error, &id, false);
}

bool name_taken(const std::string &name)
{
    std::lock_guard<std::mutex> g(lock);
    return std::any_of(list.begin(), list.end(), [&](const Ptr & p) {
        return !strcasecmp(p->name.c_str(), name.c_str());
    });
}

std::string action_words(const Action &a)
{
    if (a.kind == Do::Home && home_app_ready()) {
        std::string what, resolved;
        home_app_resolve(a.verb.c_str(), a.target.c_str(), a.value.c_str(), what, resolved);
        if (!what.empty()) {
            return what;
        }
    }
    return action_text(a);
}

std::string home_choices()
{
    return home_app_choices_json();
}

std::string next_text(const Shortcut &sc)
{
    const time_t now = time(nullptr);
    if (now < CLOCK_SET) {
        return "";
    }
    Location loc;
    {
        std::lock_guard<std::mutex> g(lock);
        loc = location;
    }
    time_t best = 0;
    for (const Trigger &t : sc.triggers) {
        const time_t at = next_fire(t, now, loc);
        if (at && (!best || at < best)) {
            best = at;
        }
    }
    return when_text(best);
}

uint32_t revision()
{
    return rev.load();
}

void board_wake()
{
    always_on_wake();
}

void board_buzz()
{
    haptics_pulse(HapticPulse::Tap);
}

void board_chime()
{
    glimdock_app_play_reminder();
}
}  // namespace shortcuts

using namespace shortcuts;

void shortcuts_start(const ShortcutHooks &hooks)
{
    H = hooks;
    load();
    register_tools();
    esp_iris_rpc_register(0x4744, 55, rpc, nullptr);
    // Runs wait and block (the Interaction board), never on the LVGL task.
    // Below LVGL's drawing (3); HTTP is Home's own worker, and nothing here
    // writes flash, so the stacks can be in PSRAM. Three, so that a long
    // automation (a wait of minutes) leaves taps free.
    static const char *const NAMES[RUNNERS] = {"shortcuts", "shortcuts2", "shortcuts3"};
    for (int i = 0; i < RUNNERS; ++i) {
        xTaskCreatePinnedToCoreWithCaps(runner_task, NAMES[i], 16384, nullptr, 2, &runners[i], tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    lv_timer_create(check, 1000, nullptr);
}

void shortcuts_editor_open()
{
    ui_editor_open();
}

bool shortcuts_editor_back()
{
    return ui_editor_back();
}

bool shortcuts_run(uint32_t id, bool dry_run)
{
    return start(id, dry_run, Source::Tile);
}

std::string shortcuts_web_list()
{
    JsonDocument doc;
    JsonArray arr = doc["shortcuts"].to<JsonArray>();
    for (const auto &p : list_copy()) {
        JsonObject o = arr.add<JsonObject>();
        shortcut_json(*p, o);
        status_json(p->id, o["last"].to<JsonObject>());
        const std::string next = next_text(*p);
        if (!next.empty() && p->enabled) {
            o["next"] = next;
        }
    }
    {
        std::lock_guard<std::mutex> g(lock);
        if (location.set) {
            doc["location"]["lat"] = location.lat;
            doc["location"]["lon"] = location.lon;
        }
    }
    doc["dry_run"] = dry_all.load();
    std::string choices = "{}", states = "[]";
    on_ui([&] {
        choices = home_app_choices_json();
        if (home_app_ready()) {
            states = home_app_states_json();
        }
    });
    JsonDocument home, devices;
    deserializeJson(home, choices);
    deserializeJson(devices, states);
    // Home's commands: these, and any more Home lists itself.
    JsonArray verbs = doc["verbs"].to<JsonArray>();
    names_json(verbs, HOME_VERBS);
    for (JsonVariantConst v : home["verbs"].as<JsonArrayConst>())
        if (v.is<const char *>() && index_of(HOME_VERBS, v.as<const char *>()) < 0) {
            verbs.add(v.as<const char *>());
        }
    doc["home"] = home;
    // Names for conditions and triggers: devices that switch or report a temperature, and rooms.
    JsonArray names = doc["devices"].to<JsonArray>();
    for (JsonObjectConst d : devices.as<JsonArrayConst>()) {
        if (d["on"].is<bool>() || d["temp"].is<float>()) {
            JsonObject n = names.add<JsonObject>();
            n["name"] = d["name"] | "";
            n["room"] = d["room"] | "";
            n["switch"] = d["on"].is<bool>();
            n["temp"] = d["temp"].is<float>();
        }
    }
    names_json(doc["ir_keys"].to<JsonArray>(), IR_KEYS);
    names_json(doc["board_modes"].to<JsonArray>(), BOARD_MODES);
    names_json(doc["apps"].to<JsonArray>(), APPS);
    names_json(doc["tools"].to<JsonArray>(), TOOLS);
    names_json(doc["icons"].to<JsonArray>(), ICONS);
    names_json(doc["accents"].to<JsonArray>(), ACCENTS);
    doc["max"] = MAX_COUNT;
    doc["max_steps"] = MAX_NODES;
    doc["max_depth"] = MAX_DEPTH;
    return json_text(doc);
}

std::string shortcuts_web_apply(const char *json, size_t length, bool *ok)
{
    return apply(json, length, ok, false);
}
