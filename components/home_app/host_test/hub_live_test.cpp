// SPDX-License-Identifier: Apache-2.0
// Sends every body Home builds (home_actions.h) through Home's own hub client
// (home_hub.cpp) to a running Swifty Home hub, and reports how each one ended.
//
// For a local demo hub only (`swifty init --demo --live`): it switches the
// pretend devices, feeds the pretend cat and spends a pairing code. Never point
// it at a real home.
//
//   cmake -S components/home_app/host_test -B build-hub-test && cmake --build build-hub-test
//   build-hub-test/hub-live-test http://localhost:8790 PAIRING-CODE [OWNER-SESSION]
//
// The pairing code is one the demo hub's owner made (display.pair). With a
// demo owner's session as well, the actions the hub keeps for the owner
// (switches, the air conditioner, a plugin's own action) are also sent, once
// as the display (the hub must refuse them) and once as the owner. Exits 1 if
// any result differs from what the hub should answer.
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "home_actions.h"
#include "home_hub.h"
#include "home_prefs.h"

namespace {
std::string owner_session, hub_origin;
bool as_owner = false;  // read on the HTTP worker; only flipped while it is idle

size_t collect(char *data, size_t size, size_t count, void *out)
{
    static_cast<std::string *>(out)->append(data, size * count);
    return size * count;
}
}  // namespace

namespace home {
uint32_t now_ms()
{
    using namespace std::chrono;
    return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

HttpResponse http_perform(const HttpRequest &request)
{
    HttpResponse out;
    CURL *curl = curl_easy_init();
    curl_slist *headers = nullptr;
    for (const auto &h : request.headers) {
        if (as_owner && h.first == "Authorization") {
            // The owner signs in with a passkey and a session cookie, and the
            // hub checks a cookie's Origin; a display sends its bearer token.
            headers = curl_slist_append(headers, ("Cookie: __Host-swifty-session=" + owner_session).c_str());
            headers = curl_slist_append(headers, ("Origin: " + hub_origin).c_str());
            continue;
        }
        headers = curl_slist_append(headers, (h.first + ": " + h.second).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, long(request.timeout_ms));
    if (!request.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(request.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    const CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        out.error = curl_easy_strerror(result);
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        out.code = int(code);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return out;
}

void prefs_load(std::string &hub_url, std::string &token, std::string &name, int &theme, std::string &room)
{
    hub_url.clear();
    token.clear();
    name.clear();
    theme = 1;
    room.clear();
}
void prefs_save(const std::string &, const std::string &, const std::string &, int, const std::string &) {}
int prefs_clock_zone()
{
    return 0;
}
}  // namespace home

using namespace home;

namespace {
// Runs the hub client until done() or the time is up.
bool pump(const std::function<bool()> &done, int ms)
{
    for (int waited = 0; waited < ms; waited += 20) {
        hub.poll();
        hub.tick_timers();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::string body_of(const Command &c)
{
    std::string params;
    serializeJson(c.params, params);
    return c.action + " " + c.target + " " + params;
}

int failures = 0;

// Sends one command as Home does (its catalog flow: direct, or prepare then
// confirm, which the person's Go stands for here), waits for the run to end,
// and compares its phase, or the hub's refusal, with what is expected.
void check(Command c, const std::string &expect, const char *flow_override = nullptr)
{
    const std::string flow = flow_override ? flow_override : hub.flow(c.action);
    const std::string what = body_of(c);
    bool finished = false;
    std::string outcome;
    RunHandler done = [&](const DocPtr &run, const std::string &error) {
        if (finished) {
            return;
        }
        if (!run) {
            outcome = "refused: " + error;
            finished = true;
            return;
        }
        const std::string phase = (*run)["phase"] | "";
        if (phase != "commanded") {
            outcome = phase;
            finished = true;
        }
    };
    if (flow == "direct") {
        hub.execute(c.action, c.target, std::move(c.params), done);
    } else if (flow == "prepare_confirm") {
        hub.prepare(c.action, c.target, std::move(c.params), [&](const DocPtr &prep, const std::string &error) {
            if (!prep) {
                outcome = "refused at prepare: " + error;
                finished = true;
                return;
            }
            hub.confirm(prep, done);
        });
    } else {
        outcome = "not in this display's catalog";
        finished = true;
    }
    if (!pump([&] { return finished; }, 60000)) {
        outcome = "no answer";
    }
    const bool ok = expect == "ok" ? (outcome == "confirmed" || outcome == "sent" || outcome == "simulated")
                                   : outcome.find(expect) != std::string::npos;
    if (!ok) {
        ++failures;
    }
    printf("%-4s %-62s %s\n", ok ? "ok" : "FAIL", what.c_str(), outcome.c_str());
}

const char *first_room_id()
{
    if (!hub.rooms) {
        return nullptr;
    }
    for (JsonObjectConst r : (*hub.rooms)["rooms"].as<JsonArrayConst>()) {
        return r["id"] | (const char *)nullptr;
    }
    return nullptr;
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s HUB-URL PAIRING-CODE [OWNER-SESSION]\n", argv[0]);
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const std::string url = argv[1];
    hub_origin = url;
    while (!hub_origin.empty() && hub_origin.back() == '/') {
        hub_origin.pop_back();
    }
    owner_session = argc > 3 ? argv[3] : "";

    hub.start();
    bool paired = false, answered = false;
    std::string why;
    hub.pair(url, argv[2], [&](bool ok, const std::string &message) {
        paired = ok;
        why = message;
        answered = true;
    });
    pump([&] { return answered; }, 15000);
    if (!paired) {
        fprintf(stderr, "pairing failed: %s\n", why.c_str());
        return 1;
    }
    printf("paired as %s\n", hub.cfg.name.c_str());
    hub.refresh();
    if (!pump([] { return hub.state && hub.devices && hub.rooms && hub.scenes && !hub.catalog.empty(); }, 15000)) {
        fprintf(stderr, "the hub's lists did not arrive\n");
        return 1;
    }
    printf("catalog: %zu actions, devices: %zu\n", hub.catalog.size(), (*hub.devices)["devices"].size());

    // What a display may do (policy.rs FAMILY_ACTIONS), on the demo home's devices.
    check(cmd::light_power("mock-lamp", true), "ok");
    check(cmd::light_brightness("mock-lamp", 40), "ok");
    check(cmd::light_hue("mock-lamp", 210), "ok");
    check(cmd::lights_brightness(60), "ok");
    check(cmd::lights_power(false), "ok");
    check(cmd::speaker_volume("mock-speaker", 25), "ok");
    check(cmd::speaker_mute("mock-speaker", true), "ok");
    check(cmd::speaker_mute("mock-speaker", false), "ok");
    check(cmd::speaker_night("mock-speaker", true), "ok");
    check(cmd::speaker_play("mock-speaker"), "ok");
    check(cmd::speaker_pause("mock-speaker"), "ok");
    check(cmd::tv_power("mock-tv", true), "ok");
    check(cmd::tv_input("mock-tv", "HDMI 2"), "ok");
    check(cmd::tv_key("mock-tv", "volume_up"), "ok");
    check(cmd::scene_apply("evening"), "ok");
    check(cmd::scene_apply("focus"), "ok");
    check(cmd::scene_apply("all_off"), "ok");
    check(cmd::scene_movie(), "ok");
    check(cmd::pets_feed("mock-feeder", 20), "ok");
    if (const char *room = first_room_id()) {
        const std::string id = room;
        check(cmd::room_brightness(id, 35), "ok");
        check(cmd::room_power(id, false), "ok");
        check(cmd::room_scene(id, "Saved"), "ok");
    } else {
        printf("--   no room on the hub: room.power, room.brightness and room.scene not sent\n");
    }

    if (!owner_session.empty()) {
        // The hub keeps these for the owner: a display is refused even if it tries.
        check(cmd::plug_power("mock-plug", true), "Only the owner", "direct");
        check(cmd::ac_power("mock-aircon", true), "Only the owner", "direct");
        as_owner = true;
        hub.refresh();
        pump([] { return false; }, 1500);  // the owner's catalog, with its flows
        check(cmd::plug_power("mock-plug", true), "ok");
        check(cmd::plug_power("mock-plug", false), "ok");
        check(cmd::ac_power("mock-aircon", true), "ok");
        check(cmd::ac_mode("mock-aircon", "cool"), "ok");
        check(cmd::ac_temperature("mock-aircon", 22), "ok");
        check(cmd::ac_fan("mock-aircon", "medium_high"), "ok");
        check(cmd::ac_power("mock-aircon", false), "ok");
        check(cmd::make("mock.set_level", "mock-gadget", "level", 3), "ok");
        as_owner = false;
    }
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
    curl_global_cleanup();
    return failures ? 1 : 0;
}
