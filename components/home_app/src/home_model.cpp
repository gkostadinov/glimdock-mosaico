// SPDX-License-Identifier: Apache-2.0
#include "home_model.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>

namespace home {
namespace {
// until 0: the command is still running (held up to HOLD_MS in case its
// result never comes).
struct Lean {
    double value;
    uint32_t until, since;
    std::string text;  // a word asked for (a mode, a fan speed); empty for numbers
};
std::map<std::string, Lean> leans;
constexpr uint32_t SETTLED_MS = 10000, HOLD_MS = 90000;

// The web's tolerances: brightness within 1 (or the 10% floor), hue within 2.
bool agrees(const char *field, double actual, double want)
{
    if (!strcmp(field, "bri")) {
        return fabs(actual - want) <= 1 || (want < 10 && actual == 10);
    }
    if (!strcmp(field, "hue")) {
        const double d = fmod(fabs(actual - want), 360.0);
        return std::min(d, 360.0 - d) <= 2;
    }
    return actual == want;
}

bool expired(const Lean &l, uint32_t now)
{
    return l.until ? int32_t(now - l.until) > 0 : now - l.since > HOLD_MS;
}

std::string leaned_text(const std::string &target, const char *field, const std::string &actual)
{
    auto it = leans.find(target + "." + field);
    if (it == leans.end()) {
        return actual;
    }
    const Lean &l = it->second;
    if (expired(l, now_ms()) || (l.until && actual == l.text)) {
        leans.erase(it);
        return actual;
    }
    return l.text;
}

double leaned(const std::string &target, const char *field, double actual)
{
    auto it = leans.find(target + "." + field);
    if (it == leans.end()) {
        return actual;
    }
    const Lean &l = it->second;
    if (expired(l, now_ms()) || (l.until && agrees(field, actual, l.value))) {
        leans.erase(it);
        return actual;
    }
    return l.value;
}

int number_or(JsonVariantConst v, int fallback)
{
    return v.is<double>() || v.is<long>() ? int(lround(v.as<double>())) : fallback;
}
}  // namespace

void lean(const std::string &target, const char *field, double value)
{
    leans[target + "." + field] = {value, 0, now_ms(), std::string()};
}

void lean_text(const std::string &target, const char *field, const std::string &value)
{
    leans[target + "." + field] = {0, 0, now_ms(), value};
}

bool leaning(const std::string &target)
{
    const std::string prefix = target + ".";
    for (const auto &kv : leans)
        if (kv.first.compare(0, prefix.size(), prefix) == 0 && !kv.second.until) {
            return true;
        }
    return false;
}

void lean_settle(const std::string &target, bool ok)
{
    const std::string prefix = target + ".";
    const uint32_t now = now_ms();
    for (auto it = leans.begin(); it != leans.end();) {
        if (it->first.compare(0, prefix.size(), prefix) || it->second.until) {
            ++it;
        } else if (ok) {
            it->second.until = now + SETTLED_MS;
            ++it;
        } else {
            it = leans.erase(it);
        }
    }
}

bool leans_due()
{
    const uint32_t now = now_ms();
    for (const auto &kv : leans)
        if (expired(kv.second, now)) {
            return true;
        }
    return false;
}

std::string to_text(JsonVariantConst v)
{
    if (v.is<const char *>()) {
        return v.as<const char *>();
    }
    if (v.is<long>()) {
        return std::to_string(v.as<long>());
    }
    if (v.is<double>()) {
        char text[32];
        snprintf(text, sizeof(text), "%g", v.as<double>());
        return text;
    }
    if (v.is<bool>()) {
        return v.as<bool>() ? "true" : "false";
    }
    return std::string();
}

std::string sentence(const std::string &word)
{
    std::string out = word;
    for (char &ch : out)
        if (ch == '_') {
            ch = ' ';
        }
    if (!out.empty()) {
        out[0] = char(toupper(uint8_t(out[0])));
    }
    return out;
}

std::string ac_words(const Item &ac)
{
    if (!ac.known) {
        return ac.reason.empty() ? "Not reporting" : ac.reason;
    }
    if (!ac.on) {
        return "Off";
    }
    // web pages/home.js acWord: what it is doing, and the target unless it has none.
    static const char *const doing[][2] = {
        {"auto", "Automatic"}, {"cool", "Cooling"}, {"heat", "Heating"}, {"dry", "Drying"}, {"fan", "Fan only"}
    };
    std::string words = "On";
    for (const auto &d : doing)
        if (ac.mode == d[0]) {
            words = d[1];
        }
    if (ac.mode != "fan" && ac.has_target) {
        words += " to " + std::to_string(ac.target_c) + "\xC2\xB0";
    }
    return words;
}

JsonObjectConst capability(const JsonObjectConst &dev, const char *name)
{
    for (JsonObjectConst cap : dev["capabilities"].as<JsonArrayConst>()) {
        if (strcmp(cap["cap"] | "", name) == 0) {
            return cap;
        }
    }
    return JsonObjectConst();
}

Derived build()
{
    Derived d;
    d.devices_hold = hub.devices;
    d.state_hold = hub.state;
    if (!d.devices_hold) {
        return d;
    }
    JsonObjectConst readings = d.state_hold ? (*d.state_hold)["devices"].as<JsonObjectConst>() : JsonObjectConst();
    for (JsonObjectConst dev : (*d.devices_hold)["devices"].as<JsonArrayConst>()) {
        if (dev["enabled"] == false) {
            continue;
        }
        const char *target = dev["target"] | "";
        JsonObjectConst r = readings[target].as<JsonObjectConst>();
        JsonObjectConst st = r["state"].as<JsonObjectConst>();
        Item item;
        item.target = target;
        item.name = r["name"] | (dev["name"] | target);
        item.cls = dev["class"] | "";
        item.known = r["available"] == true && st.size() > 0;
        item.reason = to_text(r["error"]);
        item.dev = dev;
        const std::string &c = item.cls;
        if (c == "light") {
            item.on = leaned(target, "on", st["on"] == true) != 0;
            item.bri = int(leaned(target, "bri", number_or(st["bri"], 0)));
            item.hue = int(leaned(target, "hue", number_or(st["hue"], 30)));
            item.color = st["color"] == true;
            d.bulbs.push_back(item);
        } else if (c == "speaker" && !d.speaker) {
            item.vol = int(leaned(target, "vol", number_or(st["vol"], 0)));
            item.mute = leaned(target, "mute", st["mute"] == true) != 0;
            item.night = st["night"] == true;
            item.playing = st["playing"] == true;
            item.source = to_text(st["source"]);
            d.speaker = item;
        } else if (c == "tv" && !d.tv) {
            item.on = leaned(target, "on", st["on"] == true) != 0;
            item.input = to_text(st["input"]);
            d.tv = item;
        } else if (c == "feeder" && !d.feeder) {
            item.last = to_text(st["last"]);
            d.feeder = item;
        } else if (c == "litter" && !d.litter) {
            item.busy = st["busy"] == true;
            item.last = to_text(st["last"]);
            d.litter = item;
        } else if (c == "camera" && !d.camera) {
            item.has_streaming = st["streaming"].is<bool>();
            item.streaming = st["streaming"] == true;
            d.camera = item;
        } else if (c == "plug") {
            item.on = leaned(target, "on", st["on"] == true) != 0;
            d.plugs.push_back(item);
        } else if (c == "aircon") {
            // plugin-api state.rs State::Aircon: on, mode, target, fan, indoor, outdoor.
            item.on = leaned(target, "on", st["on"] == true) != 0;
            item.mode = leaned_text(target, "mode", to_text(st["mode"]));
            item.fan = leaned_text(target, "fan", to_text(st["fan"]));
            item.has_target = st["target"].is<long>() || leans.count(std::string(target) + ".target");
            item.target_c = int(leaned(target, "target", number_or(st["target"], 0)));
            item.temp = item.has_target ? std::to_string(item.target_c) : std::string();
            item.indoor = to_text(st["indoor"]);
            item.outdoor = to_text(st["outdoor"]);
            d.acs.push_back(item);
        } else if (c == "printer" && !d.printer) {
            item.state = to_text(st["state"]);
            item.has_pct = st["pct"].is<double>() || st["pct"].is<long>();
            item.pct = st["pct"] | 0.0;
            item.nozzle = st["nozzle"] | 0.0;
            item.bed = st["bed"] | 0.0;
            item.layer = number_or(st["layer"], 0);
            item.layers = number_or(st["layers"], 0);
            d.printer = item;
        } else if (c != "telemetry" && c != "vm" && c != "host" && c != "bridge" && c != "router" &&
                   c != "service" && c != "speaker" && c != "tv" && c != "camera" && c != "feeder" &&
                   c != "litter" && c != "printer" && c != "light") {
            // web cards/index.js: only classes without a card of their own (a
            // Hue bridge is a "bridge": listed with devices, never on Home).
            // Its readings, as the web's generic card shows them: a plugin's
            // own fields, or every plain value of a state Home knows.
            JsonObjectConst fields = !strcmp(st["kind"] | "", "custom") ? st["fields"].as<JsonObjectConst>() : st;
            for (JsonPairConst kv : fields) {
                if (!strcmp(kv.key().c_str(), "kind") || kv.value().is<JsonObjectConst>() || kv.value().is<JsonArrayConst>()) {
                    continue;
                }
                std::string value = kv.value().isNull() ? "\xE2\x80\x94" : kv.value().is<bool>() ? (kv.value().as<bool>() ? "Yes" : "No") : to_text(kv.value());
                item.readings.push_back({kv.key().c_str(), value});
            }
            d.others.push_back(item);
        }
    }
    std::sort(d.bulbs.begin(), d.bulbs.end(), [](const Item & a, const Item & b) {
        return a.name < b.name;
    });
    return d;
}

LightsSummary lights_summary(const std::vector<Item> &bulbs)
{
    int known = 0, lit = 0, sum = 0;
    double hx = 0, hy = 0;
    for (const Item &b : bulbs) {
        if (!b.known) {
            continue;
        }
        known++;
        if (b.on) {
            lit++;
            sum += b.bri;
            hx += cos(b.hue * M_PI / 180.0) * b.bri;
            hy += sin(b.hue * M_PI / 180.0) * b.bri;
        }
    }
    LightsSummary s;
    s.n = lit;
    s.total = int(bulbs.size());
    s.avg = lit > 0 ? int(floor(double(sum) / lit + 0.5)) : 0;
    s.hue = lit > 0 ? fmod(atan2(hy, hx) * 180.0 / M_PI + 360.0, 360.0) : 270;
    if (bulbs.empty()) {
        s.state = "No lights yet";
    } else if (known == 0) {
        s.state = "Not reporting";
    } else if (lit == 0) {
        s.state = "All off";
    } else if (lit == known) {
        s.state = known == 1 ? "On" : "All on";
    } else {
        s.state = std::to_string(lit) + " of " + std::to_string(known) + " on";
    }
    return s;
}

std::pair<std::string, std::string> status_sentence(const Derived &d, const struct tm *local, const std::string &noresp)
{
    if (!hub.state) {
        return {"Swifty Home", ""};
    }
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"January", "February", "March", "April", "May", "June",
                                         "July", "August", "September", "October", "November", "December"
                                        };
    std::string who = hub.me ? std::string((*hub.me)["session"]["user_name"] | "") : std::string();
    if (who.empty()) {
        who = hub.cfg.name.empty() ? "Wall display" : hub.cfg.name;
    }
    std::string when = who;
    if (local) {
        when = std::string(days[local->tm_wday % 7]) + " " + std::to_string(local->tm_mday) + " " +
               months[local->tm_mon % 12] + " \xC2\xB7 " + who;
    }
    if (d.camera && hub.status.live && d.camera->known && d.camera->has_streaming && !d.camera->streaming) {
        return {"The camera is not answering.", "Tap Camera to see more."};
    }
    if (!noresp.empty()) return {"The " + noresp + " didn't answer.", "Nothing changed. Try again when you like."};
    if (!hub.status.live) return {"Dry-run. No device is contacted.", when};
    std::vector<std::string> quiet;
    for (const Item &b : d.bulbs) if (!b.known) {
            quiet.push_back(b.name);
        }
    for (const auto *k : {
                &d.speaker, &d.tv
            }) if (*k && !(*k)->known) {
            quiet.push_back((*k)->name);
        }
    if (quiet.size() == 1) return {quiet[0] + " is not reporting.", when};
    if (quiet.size() > 1) return {std::to_string(quiet.size()) + " devices are not reporting.", when};
    return {"All is well at home.", when};
}

}  // namespace home
