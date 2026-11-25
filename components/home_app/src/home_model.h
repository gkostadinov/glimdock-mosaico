// SPDX-License-Identifier: Apache-2.0
// Derives the Home view from hub data (home_model.lua / web core/store.js).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "home_hub.h"

namespace home {

struct Item {
    std::string target, name, cls, reason;
    bool known = false;
    JsonObjectConst dev;  // valid while the Derived that built it is alive
    bool on = false, color = false, mute = false, night = false, playing = false, busy = false;
    int bri = 0, hue = 30, vol = 0;
    std::string source, input, last, mode, temp, indoor, state;
    bool has_streaming = false, streaming = false, has_pct = false;
    double pct = 0;
};

struct Derived {
    DocPtr devices_hold, state_hold;  // keep JsonObjectConst views valid
    std::vector<Item> bulbs, plugs, acs, others;
    std::optional<Item> speaker, tv, feeder, litter, camera, printer;
};

struct LightsSummary {
    int n = 0, avg = 0, total = 0;
    double hue = 270;
    std::string state;
};

// What the person just asked for (web core/actions.js): held while the
// command runs, then until the device agrees or 10 s pass; a failure drops it.
void lean(const std::string &target, const char *field, double value);
void lean_settle(const std::string &target, bool ok);
// A held value has expired, so the page should show the reading again.
bool leans_due();
Derived build();
LightsSummary lights_summary(const std::vector<Item> &bulbs);
JsonObjectConst capability(const JsonObjectConst &dev, const char *name);
// web core/status.js: title and hint for a family-role display. tm may be null.
std::pair<std::string, std::string> status_sentence(const Derived &d, const struct tm *local, const std::string &noresp);
// Readable text for a JSON scalar (Lua tostring of a decoded value).
std::string to_text(JsonVariantConst value);

}  // namespace home
