// SPDX-License-Identifier: Apache-2.0
// Swifty Home hub client for a paired wall display (bearer token, polling),
// as the Lua app's hub.lua: same endpoints, intervals, messages and flows.
// HTTP runs on a worker thread; every other member is used on the UI thread.
#pragma once

#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ArduinoJson.h"

namespace home {

struct HttpRequest {
    std::string method, url, body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeout_ms = 8000;
};
struct HttpResponse {
    int code = 0;
    std::string body, error;  // error set when no HTTP answer arrived
};
// Implemented per platform (home_transport_idf.cpp, or the host preview).
HttpResponse http_perform(const HttpRequest &request);
// Monotonic milliseconds and wall-clock seconds (0 until synchronized).
uint32_t now_ms();

using DocPtr = std::shared_ptr<JsonDocument>;
// code 0 with error set: transport failure. doc may be empty (not JSON).
using Handler = std::function<void(int code, const DocPtr &doc, const std::string &error)>;
// run is null on failure; message explains it.
using RunHandler = std::function<void(const DocPtr &run, const std::string &message)>;

class Hub {
public:
    struct Config {
        std::string hub, token, name;
    } cfg;
    struct Prefs {
        int theme = 1;
        std::string room;
    } prefs;  // theme 0 light, 1 dark (AMOLED), 2 auto
    struct Status {
        bool connected = false;
        std::string message = "Not paired";
        uint32_t revision = 0;
        bool live = true;
        uint32_t last_ok = 0;
    } status;
    // Latest good responses. Replaced (never mutated) on the UI thread.
    DocPtr devices, rooms, scenes, me, state;
    std::map<std::string, std::string> catalog;  // action id -> flow

    void start();
    bool paired() const
    {
        return !cfg.hub.empty() && !cfg.token.empty();
    }
    void save();
    void set_active(bool active)
    {
        active_ = active;
    }
    // Delivers finished requests and schedules polling. True if anything changed.
    bool poll();
    void tick_timers();
    void after(uint32_t ms, std::function<void()> fn);
    void refresh()
    {
        next_state_ = next_lists_ = 0;
    }

    void pair(const std::string &hub_url, const std::string &code, std::function<void(bool, const std::string &)> done);
    void unpair();
    void execute(const std::string &action, const std::string &target, JsonDocument params, RunHandler done);
    void prepare(const std::string &action, const std::string &target, JsonDocument params, RunHandler done);
    void confirm(const DocPtr &prepared, RunHandler done);
    bool can(const std::string &action) const
    {
        return catalog.count(action) != 0;
    }
    std::string flow(const std::string &action) const;
    static std::string error_message(int code, const DocPtr &doc, const std::string &error);

private:
    uint32_t request(const char *method, const std::string &path, const JsonDocument *body, Handler done,
                     bool anonymous = false, const std::string &hub_override = std::string(), int timeout_ms = 8000);
    void follow(const std::string &run_id, RunHandler done, int tries);
    void load_lists();
    void bump(const std::string &message = std::string(), int connected = -1);
    std::string request_id();

    bool active_ = true, state_inflight_ = false;
    uint32_t next_state_ = 0, next_lists_ = 0, next_id_ = 0, rid_ = 0;
    // The body of the response being handled, hashed: lists and state only
    // bump the revision (and so rebuild the page) when they changed.
    size_t body_hash_ = 0;
    std::map<std::string, size_t> seen_;
    bool fresh(const std::string &what);
    std::map<uint32_t, Handler> pending_;
    struct Timer {
        uint32_t at;
        std::function<void()> fn;
    };
    std::vector<Timer> timers_;
};

extern Hub hub;

}  // namespace home
