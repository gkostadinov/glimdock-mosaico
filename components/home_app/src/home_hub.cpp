// SPDX-License-Identifier: Apache-2.0
#include "home_hub.h"

#include <stdio.h>
#include <string.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "home_prefs.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#define HOME_LOG(...) ESP_LOGI("home", __VA_ARGS__)
#else
#define HOME_LOG(...) (printf(__VA_ARGS__), printf("\n"))
#endif

namespace home {
Hub hub;

namespace {
constexpr uint32_t POLL_MS = 3000, IDLE_POLL_MS = 15000, LISTS_MS = 60000;

struct Job { uint32_t id; HttpRequest request; };
struct Done { uint32_t id; HttpResponse response; };

// One worker serializes HTTP, as the Lua worker did.
std::mutex mutex;
std::condition_variable wake;
std::deque<Job> jobs;
std::deque<Done> finished;
bool worker_started = false;

void worker()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [] { return !jobs.empty(); });
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        HttpResponse response = http_perform(job.request);
        // Requests carry the bearer token; clear it before freeing.
        for (auto &header : job.request.headers) {
            std::fill(header.second.begin(), header.second.end(), '\0');
        }
        std::lock_guard<std::mutex> lock(mutex);
        finished.push_back({job.id, std::move(response)});
    }
}

std::string trim_slashes(std::string url)
{
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}
}  // namespace

void Hub::start()
{
    prefs_load(cfg.hub, cfg.token, cfg.name, prefs.theme, prefs.room);
    if (!worker_started) {
        worker_started = true;
#ifdef ESP_PLATFORM
        esp_pthread_cfg_t thread_cfg = esp_pthread_get_default_config();
        thread_cfg.stack_size = 10 * 1024;  // TLS handshakes
        thread_cfg.thread_name = "home-http";
        // Only HTTP runs here (no flash writes: replies are handled on the UI
        // task), so the stack can live in PSRAM and leave internal DMA memory
        // to the display's SPI transfers.
        thread_cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        esp_pthread_set_cfg(&thread_cfg);
#endif
        std::thread(worker).detach();
    }
    bump(paired() ? "Connecting to your home" : "Not paired", 0);
}

void Hub::save()
{
    // NVS writes in place: the token survives a sudden reset.
    prefs_save(cfg.hub, cfg.token, cfg.name, prefs.theme, prefs.room);
}

void Hub::bump(const std::string &message, int connected)
{
    if (!message.empty() && message != status.message) {
        status.message = message;
        HOME_LOG("%s", message.c_str());
    }
    if (connected >= 0) {
        status.connected = connected != 0;
    }
    status.revision++;
}

bool Hub::fresh(const std::string &what)
{
    size_t &seen = seen_[what];
    if (seen == body_hash_) return false;
    seen = body_hash_;
    return true;
}

uint32_t Hub::request(const char *method, const std::string &path, const JsonDocument *body, Handler done,
                      bool anonymous, const std::string &hub_override, int timeout_ms)
{
    HttpRequest req;
    req.method = method;
    req.url = trim_slashes(hub_override.empty() ? cfg.hub : hub_override) + "/api/v1" + path;
    req.timeout_ms = timeout_ms;
    req.headers.emplace_back("Accept", "application/json");
    if (!anonymous) {
        req.headers.emplace_back("Authorization", "Bearer " + cfg.token);
    }
    if (body) {
        serializeJson(*body, req.body);
        req.headers.emplace_back("Content-Type", "application/json");
    }
    const uint32_t id = ++next_id_;
    pending_[id] = std::move(done);
    {
        std::lock_guard<std::mutex> lock(mutex);
        jobs.push_back({id, std::move(req)});
    }
    wake.notify_one();
    return id;
}

std::string Hub::error_message(int code, const DocPtr &doc, const std::string &error)
{
    if (!error.empty()) {
        return "Hub unreachable";
    }
    JsonObjectConst e = doc ? (*doc)["error"].as<JsonObjectConst>() : JsonObjectConst();
    const char *detail = e["code"] | "";
    if (code == 401 && !strcmp(detail, "display_revoked")) {
        return "This display was removed. Pair it again.";
    }
    if (code == 401) {
        return "The hub no longer accepts this display. Pair it again.";
    }
    if (e["message"].is<const char *>()) {
        return e["message"].as<const char *>();
    }
    return "Hub answered " + std::to_string(code);
}

void Hub::load_lists()
{
    request("GET", "/catalog", nullptr, [this](int code, const DocPtr &doc, const std::string &) {
        if (code != 200 || !doc) return;
        catalog.clear();
        for (JsonObjectConst action : (*doc)["actions"].as<JsonArrayConst>()) {
            catalog[action["id"] | ""] = action["flow"] | "";
        }
        if (fresh("catalog")) bump();
    });
    request("GET", "/devices", nullptr, [this](int code, const DocPtr &doc, const std::string &) {
        if (code == 200 && doc && fresh("devices")) { devices = doc; bump(); }
    });
    request("GET", "/rooms", nullptr, [this](int code, const DocPtr &doc, const std::string &) {
        if (code == 200 && doc && fresh("rooms")) { rooms = doc; bump(); }
    });
    request("GET", "/scenes", nullptr, [this](int code, const DocPtr &doc, const std::string &) {
        if (code == 200 && doc && fresh("scenes")) { scenes = doc; bump(); }
    });
    request("GET", "/me", nullptr, [this](int code, const DocPtr &doc, const std::string &) {
        if (code == 200 && doc && fresh("me")) { me = doc; bump(); }
    });
}

bool Hub::poll()
{
    std::deque<Done> ready;
    {
        std::lock_guard<std::mutex> lock(mutex);
        ready.swap(finished);
    }
    const uint32_t revision = status.revision;
    for (auto &done : ready) {
        auto it = pending_.find(done.id);
        if (it == pending_.end()) {
            continue;
        }
        Handler handler = std::move(it->second);
        pending_.erase(it);
        DocPtr doc;
        if (!done.response.body.empty()) {
            doc = std::make_shared<JsonDocument>();
            if (deserializeJson(*doc, done.response.body, DeserializationOption::NestingLimit(24))) {
                doc.reset();
            }
        }
        if (handler) {
            body_hash_ = std::hash<std::string>()(done.response.body);
            handler(done.response.code, doc, done.response.error);
        }
    }
    const bool changed = status.revision != revision;
    const uint32_t now = now_ms();
    if (paired()) {
        if (int32_t(now - next_lists_) >= 0) {
            next_lists_ = now + LISTS_MS;
            load_lists();
        }
        if (!state_inflight_ && int32_t(now - next_state_) >= 0) {
            next_state_ = now + (active_ ? POLL_MS : IDLE_POLL_MS);
            state_inflight_ = true;
            request("GET", "/state", nullptr, [this](int code, const DocPtr &doc, const std::string &error) {
                state_inflight_ = false;
                if (code == 200 && doc) {
                    status.last_ok = now_ms();
                    if (fresh("state") || !status.connected) {
                        state = doc;
                        status.live = (*doc)["live"] != false;
                        bump("Connected", 1);
                    }
                } else {
                    seen_.erase("state");
                    bump(error_message(code, doc, error), 0);
                }
            });
        }
    }
    return changed;
}

void Hub::after(uint32_t ms, std::function<void()> fn)
{
    timers_.push_back({now_ms() + ms, std::move(fn)});
}

void Hub::tick_timers()
{
    const uint32_t now = now_ms();
    for (size_t i = timers_.size(); i-- > 0;) {
        if (int32_t(now - timers_[i].at) >= 0) {
            auto fn = std::move(timers_[i].fn);
            timers_.erase(timers_.begin() + long(i));
            if (fn) fn();
        }
    }
}

std::string Hub::flow(const std::string &action) const
{
    auto it = catalog.find(action);
    return it == catalog.end() ? std::string() : it->second;
}

std::string Hub::request_id()
{
    char id[40];
    snprintf(id, sizeof(id), "mosaico-%u-%u", unsigned(now_ms()), unsigned(++rid_));
    return id;
}

void Hub::pair(const std::string &hub_url, const std::string &code, std::function<void(bool, const std::string &)> done)
{
    const std::string base = trim_slashes(hub_url);
    JsonDocument body;
    body["code"] = code;
    body["name"] = "Mosaico";
    request("POST", "/auth/display/exchange", &body,
            [this, base, done](int status_code, const DocPtr &doc, const std::string &error) {
                if (status_code == 200 && doc && (*doc)["token"].is<const char *>()) {
                    cfg.hub = base;
                    cfg.token = (*doc)["token"].as<const char *>();
                    cfg.name = (*doc)["display"]["name"] | "Wall display";
                    save();
                    refresh();
                    bump("Paired as " + cfg.name, 0);
                    done(true, std::string());
                    return;
                }
                JsonObjectConst e = doc ? (*doc)["error"].as<JsonObjectConst>() : JsonObjectConst();
                std::string message = !error.empty()           ? "Could not reach the hub"
                                      : status_code == 403      ? "That code is wrong, used or expired. Make a new one."
                                      : status_code == 429      ? "Too many tries. Wait a minute."
                                      : e["message"].is<const char *>() ? std::string(e["message"].as<const char *>())
                                                                        : "Hub answered " + std::to_string(status_code);
                done(false, message);
            },
            true, base);
}

void Hub::unpair()
{
    std::fill(cfg.token.begin(), cfg.token.end(), '\0');
    cfg.token.clear();
    cfg.name.clear();
    state.reset();
    me.reset();
    devices.reset();
    rooms.reset();
    scenes.reset();
    catalog.clear();
    seen_.clear();
    save();
    bump("Not paired", 0);
}

void Hub::follow(const std::string &run_id, RunHandler done, int tries)
{
    if (tries <= 0) {
        return;
    }
    after(600, [this, run_id, done, tries] {
        request("GET", "/actions/runs/" + run_id, nullptr, [this, run_id, done, tries](int code, const DocPtr &doc,
                                                                                         const std::string &) {
            if (code != 200 || !doc) return;
            if (strcmp((*doc)["phase"] | "", "commanded") == 0) {
                follow(run_id, done, tries - 1);
            } else {
                if (done) done(doc, std::string());
                next_state_ = 0;
            }
        });
    });
}

void Hub::execute(const std::string &action, const std::string &target, JsonDocument params, RunHandler done)
{
    JsonDocument body;
    body["action"] = action;
    body["target"] = target;
    body["params"] = params.is<JsonObject>() ? params.as<JsonObject>() : body["params"].to<JsonObject>();
    body["channel"] = "panel";
    body["request_id"] = request_id();
    request("POST", "/actions/execute", &body, [this, done](int code, const DocPtr &doc, const std::string &error) {
        if ((code == 200 || code == 202) && doc) {
            if (done) done(doc, std::string());
            if (code == 202 && (*doc)["run_id"].is<const char *>()) follow((*doc)["run_id"].as<const char *>(), done, 100);
            next_state_ = 0;
        } else if (done) {
            done(nullptr, error_message(code, doc, error));
        }
    });
}

void Hub::prepare(const std::string &action, const std::string &target, JsonDocument params, RunHandler done)
{
    JsonDocument body;
    body["action"] = action;
    body["target"] = target;
    body["params"] = params.is<JsonObject>() ? params.as<JsonObject>() : body["params"].to<JsonObject>();
    body["channel"] = "panel";
    request("POST", "/actions/prepare", &body, [done](int code, const DocPtr &doc, const std::string &error) {
        if (code == 200 && doc && (*doc)["prepared_id"].is<const char *>()) {
            done(doc, std::string());
        } else {
            done(nullptr, error_message(code, doc, error));
        }
    });
}

void Hub::confirm(const DocPtr &prep, RunHandler done)
{
    JsonDocument body;
    body["prepared_id"] = (*prep)["prepared_id"];
    body["action"] = (*prep)["action"];
    body["target"] = (*prep)["target"];
    if ((*prep)["params"].is<JsonObjectConst>()) {
        body["params"] = (*prep)["params"];
    } else {
        body["params"].to<JsonObject>();
    }
    body["confirmation"] = (*prep)["confirmation"];
    body["channel"] = "panel";
    body["request_id"] = request_id();
    request("POST", "/actions/confirm", &body, [this, done](int code, const DocPtr &doc, const std::string &error) {
        if ((code == 200 || code == 202) && doc) {
            done(doc, std::string());
            if (code == 202 && (*doc)["run_id"].is<const char *>()) follow((*doc)["run_id"].as<const char *>(), done, 100);
            next_state_ = 0;
        } else {
            done(nullptr, error_message(code, doc, error));
        }
    });
}

}  // namespace home
