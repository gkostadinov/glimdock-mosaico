// SPDX-License-Identifier: Apache-2.0
// Shortcuts' programs (see shortcuts_program.h). Nothing here touches the
// board: the host tests build this file on its own.
#include "shortcuts_program.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace shortcuts {

const char *const HOME_VERBS[17] = {"lights_on", "lights_off", "lights_toggle", "brightness", "color", "scene",
                                    "plug_on", "plug_off", "plug_toggle", "tv_on", "tv_off", "tv_toggle",
                                    "volume", "play", "pause", "mute", "unmute"
                                   };
const char *const IR_KEYS[14] = {"power", "volume_up", "volume_down", "mute", "input", "ok", "up", "down", "left",
                                 "right", "back", "home", "channel_up", "channel_down"
                                };
const char *const BOARD_MODES[5] = {"off", "react", "rainbow", "night", "colour"};
const char *const APPS[11] = {"launcher", "glimdock", "home", "camera", "swifty", "files", "tools", "gallery",
                              "webcam", "interaction", "calculator"
                             };
const char *const TOOLS[3] = {"torch", "level", "compass"};
const char *const ICONS[24] = {"bulb", "power", "house", "rooms", "tv", "speaker", "play", "pause", "snow", "bell",
                               "clock", "lock", "key", "heart", "paw", "camera", "mic", "cloud", "printer", "laptop",
                               "nas", "puzzle", "pin", "person"
                              };
const char *const ACCENTS[4] = {"violet", "cyan", "amber", "green"};
const char *const DAY_NAMES[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

namespace {
// The JSON names of the kinds, in their enums' order.
const char *const DO_NAMES[] = {"home", "ir", "board_lights", "brightness", "open", "flashlight",
                                "wait", "if", "repeat", "set", "add", "message", "run", "stop"
                               };
const char *const CHECK_NAMES[] = {"and", "or", "not", "time", "day", "home", "motion", "button", "light",
                                   "wifi", "vpn", "battery", "charging", "var"
                                  };
const char *const ON_NAMES[] = {"tile", "time", "every", "boot", "motion", "button", "home", "sun"};
const char *const BUTTONS[] = {"left", "right", "any"};
const char *const SOURCE_NAMES[] = {"tile", "editor", "swifty", "web", "rpc", "test", "time", "every", "boot",
                                    "motion", "button", "home", "sun"
                                   };

template <size_t N> std::string joined(const char *const(&names)[N], size_t from = 0, size_t to = N)
{
    std::string out;
    for (size_t i = from; i < to; ++i) {
        out += (i > from ? ", " : "") + std::string(names[i]);
    }
    return out;
}

std::string lower(std::string s)
{
    for (char &c : s) {
        c = char(tolower(uint8_t(c)));
    }
    return s;
}

std::string text_of(JsonVariantConst v)
{
    if (v.is<const char *>()) {
        return v.as<const char *>();
    }
    if (v.is<long>()) {
        return std::to_string(v.as<long>());
    }
    if (v.is<float>()) {
        char t[24];
        snprintf(t, sizeof(t), "%g", double(v.as<float>()));
        return t;
    }
    return "";
}

std::string number_text(float v)
{
    char t[24];
    snprintf(t, sizeof(t), "%g", double(v));
    return t;
}

bool is_number(JsonVariantConst v)
{
    return v.is<float>() || v.is<long>();
}

// A variable's name: a letter, then letters, digits or _, up to 16.
bool var_name_ok(const std::string &n)
{
    if (n.empty() || n.size() > 16 || !isalpha(uint8_t(n[0]))) {
        return false;
    }
    return std::all_of(n.begin(), n.end(), [](char c) {
        return isalnum(uint8_t(c)) || c == '_';
    });
}

// Weekdays: ["mon","tue"], "weekdays", "weekend", "every day", "mon,wed" or
// [1,3] (0 Sunday). Absent: every day.
bool parse_days(JsonVariantConst v, uint8_t &days, std::string &error)
{
    days = 0x7F;
    if (v.isNull()) {
        return true;
    }
    std::vector<std::string> words;
    if (v.is<const char *>()) {
        const std::string s = lower(v.as<const char *>());
        if (s == "weekdays" || s == "workdays") {
            days = 0x3E;
            return true;
        }
        if (s == "weekend" || s == "weekends") {
            days = 0x41;
            return true;
        }
        if (s == "every day" || s == "daily" || s == "all" || s.empty()) {
            return true;
        }
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find_first_of(", ", start);
            if (end == std::string::npos) {
                end = s.size();
            }
            if (end > start) {
                words.push_back(s.substr(start, end - start));
            }
            start = end + 1;
        }
    } else if (v.is<JsonArrayConst>()) {
        days = 0;
        for (JsonVariantConst d : v.as<JsonArrayConst>()) {
            if (d.is<int>() && d.as<int>() >= 0 && d.as<int>() <= 7) {
                days |= uint8_t(1 << (d.as<int>() % 7));
            } else if (d.is<const char *>()) {
                words.push_back(lower(d.as<const char *>()));
            } else {
                error = "days are names: sun, mon, tue, wed, thu, fri, sat";
                return false;
            }
        }
    } else {
        error = "days: a list of sun, mon, tue, wed, thu, fri, sat (or \"weekdays\", \"weekend\")";
        return false;
    }
    if (!words.empty()) {
        days = v.is<const char *>() ? 0 : days;
        for (const std::string &w : words) {
            int i = -1;
            for (int d = 0; d < 7 && i < 0; ++d)
                if (w.compare(0, 3, DAY_NAMES[d]) == 0 && w.size() >= 3) {
                    i = d;
                }
            if (i < 0) {
                error = "day \"" + w + "\": one of sun, mon, tue, wed, thu, fri, sat";
                return false;
            }
            days |= uint8_t(1 << i);
        }
    }
    if (!days) {
        error = "days: at least one day";
        return false;
    }
    return true;
}

void days_json(uint8_t days, JsonObject o)
{
    if ((days & 0x7F) == 0x7F) {
        return;
    }
    JsonArray a = o["days"].to<JsonArray>();
    for (int d = 1; d <= 7; ++d)  // Monday first, as people write them
        if (days & (1 << (d % 7))) {
            a.add(DAY_NAMES[d % 7]);
        }
}

bool parse_time(JsonVariantConst v, const char *what, int &minute, std::string &error)
{
    if (!v.is<const char *>() || !parse_hhmm(v.as<const char *>(), minute)) {
        error = std::string(what) + " needs a time as HH:MM (24-hour), e.g. \"07:30\"";
        return false;
    }
    return true;
}

// above / below / equals: which one is there, and its number.
bool parse_compare(JsonObjectConst o, bool equals_too, Cmp &cmp, float &num)
{
    if (is_number(o["above"])) {
        cmp = Cmp::Above, num = o["above"].as<float>();
    } else if (is_number(o["below"])) {
        cmp = Cmp::Below, num = o["below"].as<float>();
    } else if (equals_too && is_number(o["equals"])) {
        cmp = Cmp::Equals, num = o["equals"].as<float>();
    } else {
        return false;
    }
    return true;
}

void compare_json(Cmp cmp, float num, JsonObject o)
{
    if (cmp == Cmp::Above) {
        o["above"] = num;
    } else if (cmp == Cmp::Below) {
        o["below"] = num;
    } else if (cmp == Cmp::Equals) {
        o["equals"] = num;
    }
}

// ---- actions (format 1's steps) ----------------------------------------------------------
bool parse_action(JsonObjectConst o, Do kind, Action &a, std::string &error)
{
    a = Action();
    a.kind = kind;
    switch (kind) {
    case Do::Home: {
        a.verb = lower(o["verb"] | (o["action"] | ""));
        // Home's own commands, and any it learns later (Home checks those
        // when the shortcut is saved, or when it runs).
        const bool word = !a.verb.empty() && a.verb.size() <= 24 && isalpha(uint8_t(a.verb[0])) &&
        std::all_of(a.verb.begin(), a.verb.end(), [](char c) {
            return islower(uint8_t(c)) || isdigit(uint8_t(c)) || c == '_';
        });
        if (!word) {
            error = "home verb \"" + a.verb + "\": one of " + joined(HOME_VERBS) + ", or another Swifty Home command";
            return false;
        }
        a.target = trimmed(o["target"] | "", 64);
        a.value = trimmed(text_of(o["value"]), 40);
        if ((a.verb == "brightness" || a.verb == "volume") && a.value.empty()) {
            error = a.verb + " needs value, 0 to 100";
            return false;
        }
        if ((a.verb == "color" || a.verb == "scene") && a.value.empty() && (a.verb == "color" || a.target.empty())) {
            error = a.verb == "color" ? "color needs value (a colour name or a hue)" : "scene needs value (the scene's name)";
            return false;
        }
        return true;
    }
    case Do::Ir:
        a.value = o["key"] | "";
        if (index_of(IR_KEYS, a.value) < 0) {
            error = "ir key: one of " + joined(IR_KEYS);
            return false;
        }
        a.value = IR_KEYS[index_of(IR_KEYS, a.value)];
        return true;
    case Do::BoardLights:
        a.verb = o["mode"] | "";
        a.value = trimmed(o["colour"] | (o["color"] | ""), 16);
        if (a.verb.empty() && !a.value.empty()) {
            a.verb = "colour";
        }
        if (a.verb == "color") {
            a.verb = "colour";
        }
        if (index_of(BOARD_MODES, a.verb) < 0) {
            error = "board_lights mode: one of " + joined(BOARD_MODES);
            return false;
        }
        return true;
    case Do::Brightness:
        a.n = o["percent"] | -1;
        if (a.n < 5 || a.n > 100) {
            error = "brightness needs percent, 5 to 100";
            return false;
        }
        return true;
    case Do::Open:
        a.verb = lower(o["app"] | "");
        a.value = lower(o["tool"] | "");
        if (a.verb == "tools" && (a.value == "calculator" || a.value == "interact")) {
            a.verb = a.value == "interact" ? "interaction" : a.value;  // Tools' tabs once, apps now
            a.value.clear();
        }
        if (index_of(APPS, a.verb) < 0) {
            error = "open app: one of " + joined(APPS);
            return false;
        }
        if (!a.value.empty() && index_of(TOOLS, a.value) < 0) {
            error = "open tool: one of " + joined(TOOLS);
            return false;
        }
        return true;
    case Do::Flashlight:
        if (!o["on"].is<bool>()) {
            error = "flashlight needs on (true or false)";
            return false;
        }
        a.n = o["on"].as<bool>() ? 1 : 0;
        return true;
    default:
        return false;
    }
}

void action_json(const Action &a, JsonObject o)
{
    o["do"] = DO_NAMES[int(a.kind)];
    switch (a.kind) {
    case Do::Home:
        o["verb"] = a.verb;
        if (!a.target.empty()) {
            o["target"] = a.target;
        }
        if (!a.value.empty()) {
            o["value"] = a.value;
        }
        break;
    case Do::Ir:
        o["key"] = a.value;
        break;
    case Do::BoardLights:
        o["mode"] = a.verb;
        if (!a.value.empty()) {
            o["colour"] = a.value;
        }
        break;
    case Do::Brightness:
        o["percent"] = a.n;
        break;
    case Do::Open:
        o["app"] = a.verb;
        if (!a.value.empty()) {
            o["tool"] = a.value;
        }
        break;
    case Do::Flashlight:
        o["on"] = a.n != 0;
        break;
    default:
        break;
    }
}

// ---- conditions ------------------------------------------------------------------------
bool parse_cond(JsonVariantConst v, Cond &c, int depth, size_t &count, std::string &error)
{
    c = Cond();
    if (!v.is<JsonObjectConst>()) {
        error = "a condition is an object: {\"check\":\"time\",...}, {\"and\":[...]}, {\"or\":[...]} or {\"not\":{...}}";
        return false;
    }
    if (++count > MAX_CONDS) {
        error = "a condition has at most " + std::to_string(MAX_CONDS) + " parts";
        return false;
    }
    if (depth > MAX_DEPTH) {
        error = "conditions nest at most " + std::to_string(MAX_DEPTH) + " deep";
        return false;
    }
    JsonObjectConst o = v.as<JsonObjectConst>();
    for (const char *logic : {"and", "or", "all", "any"}) {
        if (!o[logic].isNull()) {
            c.kind = !strcmp(logic, "or") || !strcmp(logic, "any") ? Check::Or : Check::And;
            JsonArrayConst kids = o[logic].as<JsonArrayConst>();
            if (kids.size() < 1) {
                error = std::string(logic) + " needs a list of conditions";
                return false;
            }
            for (JsonVariantConst k : kids) {
                c.kids.emplace_back();
                if (!parse_cond(k, c.kids.back(), depth + 1, count, error)) {
                    return false;
                }
            }
            return true;
        }
    }
    if (!o["not"].isNull()) {
        c.kind = Check::Not;
        c.kids.emplace_back();
        return parse_cond(o["not"], c.kids.back(), depth + 1, count, error);
    }
    const std::string check = lower(o["check"] | "");
    const int k = index_of(CHECK_NAMES, check);
    if (k < 3) {
        error = "check: one of " + joined(CHECK_NAMES, 3) + " (or and, or, not)";
        return false;
    }
    c.kind = Check(k);
    switch (c.kind) {
    case Check::Time:
        return parse_time(o["from"], "time from", c.a, error) && parse_time(o["to"], "time to", c.b, error);
    case Check::Day: {
        uint8_t days;
        if (!parse_days(o["days"], days, error)) {
            return false;
        }
        c.a = days;
        return true;
    }
    case Check::Home: {
        c.name = trimmed(o["device"] | (o["room"] | ""), 64);
        if (c.name.empty()) {
            error = "home check needs device (a light, plug, TV, sensor or room, by name)";
            return false;
        }
        const std::string is = lower(o["is"] | "");
        if (is == "on" || is == "off") {
            c.cmp = is == "on" ? Cmp::IsOn : Cmp::IsOff;
        } else if (!parse_compare(o, false, c.cmp, c.num)) {
            error = "home check needs is (\"on\" or \"off\") or above / below (a temperature in degrees)";
            return false;
        }
        return true;
    }
    case Check::Motion:
        c.a = o["within"] | 0;
        if (c.a < 0 || c.a > 24 * 3600) {
            error = "motion within: seconds, 0 (now) to 86400";
            return false;
        }
        return true;
    case Check::Button:
        c.name = lower(o["button"] | "any");
        if (index_of(BUTTONS, c.name) < 0) {
            error = "button: left, right or any";
            return false;
        }
        return true;
    case Check::Light:
    case Check::Battery:
        if (!parse_compare(o, false, c.cmp, c.num) || c.num < 0 || c.num > 100) {
            error = check + " needs above or below, a percent 0 to 100";
            return false;
        }
        return true;
    case Check::Var:
        c.name = o["var"] | "";
        if (!var_name_ok(c.name)) {
            error = "var check needs var (a name: a letter, then letters, digits or _)";
            return false;
        }
        if (!parse_compare(o, true, c.cmp, c.num)) {
            error = "var check needs equals, above or below (a number)";
            return false;
        }
        return true;
    default:
        return true;  // wifi, vpn, charging: nothing more
    }
}

// ---- steps -----------------------------------------------------------------------------
struct ParseCtx {
    size_t nodes = 0;
    std::string error;
};

bool parse_steps(JsonVariantConst v, std::vector<Node> &out, int depth, const std::string &path, ParseCtx &ctx,
                 bool allow_empty);

bool parse_node(JsonObjectConst o, Node &n, int depth, const std::string &path, ParseCtx &ctx)
{
    if (++ctx.nodes > MAX_NODES) {
        ctx.error = "a shortcut has at most " + std::to_string(MAX_NODES) + " steps in all";
        return false;
    }
    std::string kind = lower(o["do"] | "");
    if (kind.empty()) {  // {"if": ...} or {"repeat": 3} without a do
        kind = !o["if"].isNull() ? "if" : !o["repeat"].isNull() || !o["while"].isNull() ? "repeat" : "";
    }
    if (kind == "say" || kind == "show" || kind == "notify") {
        kind = "message";
    }
    if (kind == "while") {
        kind = "repeat";
    }
    const int k = index_of(DO_NAMES, kind);
    if (k < 0) {
        ctx.error = "step " + path + ": do is one of " + joined(DO_NAMES);
        return false;
    }
    n = Node();
    n.kind = Do(k);
    std::string &error = ctx.error;
    auto fail = [&](const std::string & why) {
        error = "step " + path + ": " + why;
        return false;
    };
    switch (n.kind) {
    case Do::Wait: {
        float seconds = -1;
        if (is_number(o["seconds"])) {
            seconds = o["seconds"].as<float>();
        } else if (is_number(o["minutes"])) {
            seconds = o["minutes"].as<float>() * 60;
        } else if (is_number(o["ms"])) {
            seconds = o["ms"].as<float>() / 1000.0f;
        }
        if (seconds < 0.1f || seconds * 1000 > MAX_WAIT_MS) {
            return fail("wait needs seconds, 0.1 to 3600 (or minutes, up to 60)");
        }
        n.n = int(seconds * 1000 + 0.5f);
        return true;
    }
    case Do::If: {
        size_t count = 0;
        std::string why;
        if (!parse_cond(o["if"], n.cond, 1, count, why)) {
            return fail("if: " + why);
        }
        if (depth >= MAX_DEPTH) {
            return fail("ifs and repeats nest at most " + std::to_string(MAX_DEPTH) + " deep");
        }
        if (!o["then"].is<JsonArrayConst>()) {
            return fail("if needs then (a list of steps)");
        }
        if (!parse_steps(o["then"], n.body, depth + 1, path + ".", ctx, true)) {
            return false;
        }
        if (!o["elif"].isNull() || !o["else_if"].isNull()) {
            JsonArrayConst elifs = (!o["elif"].isNull() ? o["elif"] : o["else_if"]).as<JsonArrayConst>();
            if (elifs.isNull()) {
                return fail("elif is a list of {\"if\":{...},\"then\":[...]}");
            }
            int i = 0;
            for (JsonObjectConst e : elifs) {
                ++i;
                n.elifs.emplace_back();
                count = 0;
                if (!parse_cond(e["if"], n.elifs.back().cond, 1, count, why)) {
                    return fail("elif " + std::to_string(i) + ": " + why);
                }
                if (!parse_steps(e["then"], n.elifs.back().body, depth + 1, path + ".", ctx, true)) {
                    return false;
                }
            }
            if (n.elifs.size() > 4) {
                return fail("an if has at most 4 elifs");
            }
        }
        if (!o["else"].isNull()) {
            n.has_else = true;
            if (!parse_steps(o["else"], n.other, depth + 1, path + ".", ctx, true)) {
                return false;
            }
        }
        return true;
    }
    case Do::Repeat: {
        if (depth >= MAX_DEPTH) {
            return fail("ifs and repeats nest at most " + std::to_string(MAX_DEPTH) + " deep");
        }
        JsonVariantConst times = o["times"].isNull() ? o["repeat"] : o["times"];
        if (!o["while"].isNull()) {
            size_t count = 0;
            std::string why;
            if (!parse_cond(o["while"], n.cond, 1, count, why)) {
                return fail("while: " + why);
            }
            n.n = 0;
            n.max = o["max"] | DEFAULT_WHILE;
            if (n.max < 1 || n.max > MAX_WHILE) {
                return fail("repeat while: max rounds 1 to " + std::to_string(MAX_WHILE));
            }
        } else if (times.is<int>()) {
            n.n = times.as<int>();
            if (n.n < 1 || n.n > MAX_TIMES) {
                return fail("repeat times: 1 to " + std::to_string(MAX_TIMES));
            }
        } else {
            return fail("repeat needs times (1 to 100) or while (a condition)");
        }
        JsonVariantConst body = o["steps"].isNull() ? o["do_steps"] : o["steps"];
        return parse_steps(body, n.body, depth + 1, path + ".", ctx, false);
    }
    case Do::Set:
    case Do::Add: {
        n.text = o["var"] | "";
        if (!var_name_ok(n.text)) {
            return fail(kind + " needs var (a name: a letter, then letters, digits or _)");
        }
        JsonVariantConst value = o["value"];
        if (value.isNull() && n.kind == Do::Add) {
            n.n = 1;
        } else if (is_number(value)) {
            const float f = value.as<float>();
            if (f < -1000000 || f > 1000000) {
                return fail("value: -1000000 to 1000000");
            }
            n.n = int(lroundf(f));
        } else {
            return fail(kind + " needs value (a whole number)");
        }
        return true;
    }
    case Do::Message:
        n.text = trimmed(o["text"] | (o["message"] | ""), MAX_TEXT);
        if (n.text.empty()) {
            return fail("message needs text");
        }
        n.n = ((o["haptic"] | (o["buzz"] | false)) ? 1 : 0) | ((o["sound"] | (o["chime"] | false)) ? 2 : 0);
        return true;
    case Do::Run:
        n.n = o["id"] | 0;
        n.text = trimmed(o["shortcut"].is<const char *>() ? o["shortcut"].as<const char *>() : (o["name"] | ""), MAX_NAME);
        if (o["shortcut"].is<int>() && !n.n) {
            n.n = o["shortcut"].as<int>();
        }
        if (!n.n && n.text.empty()) {
            return fail("run needs shortcut (another shortcut's name or id)");
        }
        return true;
    case Do::Stop:
        return true;
    default:
        if (!parse_action(o, n.kind, n.act, error)) {
            error = "step " + path + ": " + error;
            return false;
        }
        return true;
    }
}

bool parse_steps(JsonVariantConst v, std::vector<Node> &out, int depth, const std::string &path, ParseCtx &ctx,
                 bool allow_empty)
{
    out.clear();
    JsonArrayConst a = v.as<JsonArrayConst>();
    if (a.isNull() || (!allow_empty && a.size() == 0)) {
        ctx.error = (path.empty() ? std::string("steps") : "step " + path.substr(0, path.size() - 1) + ": its steps") +
                    (allow_empty ? " are a list" : " are a list of one step or more");
        return false;
    }
    out.reserve(a.size());
    int i = 0;
    for (JsonVariantConst s : a) {
        ++i;
        if (!s.is<JsonObjectConst>()) {
            ctx.error = "step " + path + std::to_string(i) + ": each step is an object with \"do\"";
            return false;
        }
        out.emplace_back();
        if (!parse_node(s.as<JsonObjectConst>(), out.back(), depth, path + std::to_string(i), ctx)) {
            return false;
        }
    }
    return true;
}

// ---- triggers --------------------------------------------------------------------------
bool parse_trigger(JsonObjectConst o, Trigger &t, std::string &error)
{
    t = Trigger();
    const std::string on = lower(o["on"] | (o["type"] | ""));
    const int k = index_of(ON_NAMES, on == "tap" || on == "manual" ? std::string("tile") : on == "schedule" ? std::string("time") : on);
    if (k < 0) {
        error = "on is one of " + joined(ON_NAMES);
        return false;
    }
    t.kind = On(k);
    switch (t.kind) {
    case On::Time:
        return parse_time(o["at"], "time at", t.at, error) && parse_days(o["days"], t.days, error);
    case On::Every:
        t.n = o["minutes"] | 0;
        if (t.n < 1 || t.n > 1440) {
            error = "every needs minutes, 1 to 1440";
            return false;
        }
        if (!o["from"].isNull() || !o["to"].isNull()) {
            if (!parse_time(o["from"], "every from", t.at, error) || !parse_time(o["to"], "every to", t.to, error)) {
                return false;
            }
        }
        return parse_days(o["days"], t.days, error);
    case On::Boot:
        t.n = o["delay"] | 20;
        if (t.n < 0 || t.n > 3600) {
            error = "boot delay: seconds, 0 to 3600";
            return false;
        }
        return true;
    case On::Motion:
        t.n = o["cooldown"] | 60;
        if (t.n < 10 || t.n > 24 * 3600) {
            error = "motion cooldown: seconds, 10 to 86400";
            return false;
        }
        return true;
    case On::Button:
        t.name = lower(o["button"] | "any");
        if (index_of(BUTTONS, t.name) < 0) {
            error = "button: left, right or any";
            return false;
        }
        return true;
    case On::Home: {
        t.name = trimmed(o["device"] | "", 64);
        if (t.name.empty()) {
            error = "home trigger needs device (a light, plug, TV or sensor, by name)";
            return false;
        }
        const std::string to = lower(o["to"] | "any");
        if (to == "on" || to == "off") {
            t.cmp = to == "on" ? Cmp::IsOn : Cmp::IsOff;
        } else if (!parse_compare(o, false, t.cmp, t.num) && to != "any") {
            error = "home trigger: to \"on\", \"off\" or \"any\", or above / below a temperature";
            return false;
        }
        return true;
    }
    case On::Sun: {
        t.name = lower(o["event"] | "");
        if (t.name != "sunrise" && t.name != "sunset") {
            error = "sun needs event: sunrise or sunset";
            return false;
        }
        t.n = o["offset"] | 0;
        if (t.n < -180 || t.n > 180) {
            error = "sun offset: minutes, -180 to 180";
            return false;
        }
        return parse_days(o["days"], t.days, error);
    }
    default:
        return true;
    }
}

const char *name_of(Cmp c)
{
    return c == Cmp::Above ? "above" : c == Cmp::Below ? "below" : "equals";
}
}  // namespace

// ---- shared helpers --------------------------------------------------------------------
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

std::string lowered(std::string s)
{
    return lower(std::move(s));
}

const char *kind_name(Do kind)
{
    return DO_NAMES[int(kind)];
}

bool Shortcut::tile() const
{
    return std::any_of(triggers.begin(), triggers.end(), [](const Trigger & t) {
        return t.kind == On::Tile;
    });
}

bool Shortcut::automatic() const
{
    return std::any_of(triggers.begin(), triggers.end(), [](const Trigger & t) {
        return t.kind != On::Tile;
    });
}

// ---- JSON ------------------------------------------------------------------------------
bool parse_shortcut(JsonObjectConst o, Shortcut &sc, std::string &error)
{
    sc = Shortcut();
    if (o.isNull()) {
        error = "expected a shortcut (an object with name and steps)";
        return false;
    }
    sc.id = o["id"] | 0u;
    sc.name = trimmed(o["name"] | "", MAX_NAME);
    if (sc.name.empty()) {
        error = "a shortcut needs a name";
        return false;
    }
    ParseCtx ctx;
    if (!parse_steps(o["steps"], sc.steps, 0, "", ctx, false)) {
        error = ctx.error;
        return false;
    }
    sc.enabled = o["enabled"] | true;
    if (o["triggers"].isNull()) {
        sc.triggers.push_back(Trigger());  // format 1, or none said: a tile
    } else {
        JsonArrayConst ts = o["triggers"].as<JsonArrayConst>();
        if (ts.isNull()) {
            error = "triggers are a list, e.g. [{\"on\":\"tile\"}]";
            return false;
        }
        if (ts.size() > MAX_TRIGGERS) {
            error = "a shortcut has at most " + std::to_string(MAX_TRIGGERS) + " triggers";
            return false;
        }
        int i = 0;
        for (JsonVariantConst tv : ts) {
            ++i;
            Trigger t;
            if (!tv.is<JsonObjectConst>() || !parse_trigger(tv.as<JsonObjectConst>(), t, error)) {
                error = "trigger " + std::to_string(i) + ": " + (tv.is<JsonObjectConst>() ? error : "an object with \"on\"");
                return false;
            }
            const bool again = (t.kind == On::Tile || t.kind == On::Boot) &&
            std::any_of(sc.triggers.begin(), sc.triggers.end(), [&](const Trigger & x) {
                return x.kind == t.kind;
            });
            if (!again) {
                sc.triggers.push_back(std::move(t));
            }
        }
    }
    // An icon and a colour that suit the first action, when none is given.
    const Action *first = nullptr;
    each_node(sc.steps, [&](const Node & n) {
        if (!first && n.kind <= Do::Flashlight) {
            first = &n.act;
        }
    });
    sc.icon = sc.automatic() && !sc.tile() ? "clock" : "bulb";
    sc.accent = 3;
    if (first) {
        const bool off = first->verb.find("off") != std::string::npos;
        if (first->kind == Do::Home) {
            const std::string &v = first->verb;
            sc.icon = v.rfind("lights", 0) == 0 || v == "brightness" || v == "color" ? (off ? "power" : "bulb")
                      : v == "scene" ? "house" : v.rfind("tv", 0) == 0 ? "tv" : v.rfind("plug", 0) == 0 ? "power" : "speaker";
            sc.accent = off ? 0 : v.rfind("tv", 0) == 0 ? 1 : 2;
        } else if (first->kind == Do::Ir) {
            sc.icon = "tv", sc.accent = 1;
        }
    }
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

void cond_json(const Cond &c, JsonObject o)
{
    if (c.kind == Check::And || c.kind == Check::Or) {
        JsonArray a = o[c.kind == Check::And ? "and" : "or"].to<JsonArray>();
        for (const Cond &k : c.kids) {
            cond_json(k, a.add<JsonObject>());
        }
        return;
    }
    if (c.kind == Check::Not) {
        cond_json(c.kids.front(), o["not"].to<JsonObject>());
        return;
    }
    o["check"] = CHECK_NAMES[int(c.kind)];
    switch (c.kind) {
    case Check::Time:
        o["from"] = hhmm(c.a);
        o["to"] = hhmm(c.b);
        break;
    case Check::Day:
        days_json(uint8_t(c.a), o);
        break;
    case Check::Home:
        o["device"] = c.name;
        if (c.cmp == Cmp::IsOn || c.cmp == Cmp::IsOff) {
            o["is"] = c.cmp == Cmp::IsOn ? "on" : "off";
        } else {
            compare_json(c.cmp, c.num, o);
        }
        break;
    case Check::Motion:
        if (c.a) {
            o["within"] = c.a;
        }
        break;
    case Check::Button:
        o["button"] = c.name;
        break;
    case Check::Var:
        o["var"] = c.name;
        compare_json(c.cmp, c.num, o);
        break;
    case Check::Light:
    case Check::Battery:
        compare_json(c.cmp, c.num, o);
        break;
    default:
        break;
    }
}

void steps_json(const std::vector<Node> &steps, JsonArray a)
{
    for (const Node &n : steps) {
        JsonObject o = a.add<JsonObject>();
        if (n.kind <= Do::Flashlight) {
            action_json(n.act, o);
            continue;
        }
        o["do"] = DO_NAMES[int(n.kind)];
        switch (n.kind) {
        case Do::Wait:
            o["seconds"] = n.n / 1000.0f;
            break;
        case Do::If:
            cond_json(n.cond, o["if"].to<JsonObject>());
            steps_json(n.body, o["then"].to<JsonArray>());
            if (!n.elifs.empty()) {
                JsonArray e = o["elif"].to<JsonArray>();
                for (const Branch &b : n.elifs) {
                    JsonObject bo = e.add<JsonObject>();
                    cond_json(b.cond, bo["if"].to<JsonObject>());
                    steps_json(b.body, bo["then"].to<JsonArray>());
                }
            }
            if (n.has_else) {
                steps_json(n.other, o["else"].to<JsonArray>());
            }
            break;
        case Do::Repeat:
            if (n.n) {
                o["times"] = n.n;
            } else {
                cond_json(n.cond, o["while"].to<JsonObject>());
                o["max"] = n.max;
            }
            steps_json(n.body, o["steps"].to<JsonArray>());
            break;
        case Do::Set:
        case Do::Add:
            o["var"] = n.text;
            o["value"] = n.n;
            break;
        case Do::Message:
            o["text"] = n.text;
            if (n.n & 1) {
                o["haptic"] = true;
            }
            if (n.n & 2) {
                o["sound"] = true;
            }
            break;
        case Do::Run:
            o["id"] = n.n;
            if (!n.text.empty()) {
                o["shortcut"] = n.text;
            }
            break;
        default:
            break;
        }
    }
}

void trigger_json(const Trigger &t, JsonObject o)
{
    o["on"] = ON_NAMES[int(t.kind)];
    switch (t.kind) {
    case On::Time:
        o["at"] = hhmm(t.at);
        days_json(t.days, o);
        break;
    case On::Every:
        o["minutes"] = t.n;
        if (t.at >= 0) {
            o["from"] = hhmm(t.at);
            o["to"] = hhmm(t.to);
        }
        days_json(t.days, o);
        break;
    case On::Boot:
        o["delay"] = t.n;
        break;
    case On::Motion:
        o["cooldown"] = t.n;
        break;
    case On::Button:
        o["button"] = t.name;
        break;
    case On::Home:
        o["device"] = t.name;
        if (t.cmp == Cmp::IsOn || t.cmp == Cmp::IsOff) {
            o["to"] = t.cmp == Cmp::IsOn ? "on" : "off";
        } else if (t.cmp == Cmp::None) {
            o["to"] = "any";
        } else {
            compare_json(t.cmp, t.num, o);
        }
        break;
    case On::Sun:
        o["event"] = t.name;
        if (t.n) {
            o["offset"] = t.n;
        }
        days_json(t.days, o);
        break;
    default:
        break;
    }
}

void shortcut_json(const Shortcut &sc, JsonObject o)
{
    o["id"] = sc.id;
    o["name"] = sc.name;
    o["icon"] = sc.icon;
    o["accent"] = ACCENTS[sc.accent % 4];
    o["enabled"] = sc.enabled;
    JsonArray ts = o["triggers"].to<JsonArray>();
    for (const Trigger &t : sc.triggers) {
        trigger_json(t, ts.add<JsonObject>());
    }
    steps_json(sc.steps, o["steps"].to<JsonArray>());
}

bool migrate(JsonDocument &doc, bool *migrated)
{
    if (migrated) {
        *migrated = false;
    }
    if (!doc.is<JsonObject>() || !doc["list"].is<JsonArray>()) {
        return false;
    }
    const int v = doc["v"] | 1;
    if (v >= FORMAT) {
        return true;
    }
    // Format 1: every shortcut was a tile, always on; its steps were actions
    // and waits, which read the same now.
    for (JsonObject sc : doc["list"].as<JsonArray>()) {
        if (sc["triggers"].isNull()) {
            sc["triggers"].to<JsonArray>().add<JsonObject>()["on"] = "tile";
        }
        if (sc["enabled"].isNull()) {
            sc["enabled"] = true;
        }
        for (JsonObject step : sc["steps"].as<JsonArray>()) {
            if (step["do"] == "wait" && step["seconds"].isNull() && step["ms"].is<int>()) {
                step["seconds"] = step["ms"].as<int>() / 1000.0f;
                step.remove("ms");
            }
        }
    }
    doc["v"] = FORMAT;
    if (migrated) {
        *migrated = true;
    }
    return true;
}

// ---- words -----------------------------------------------------------------------------
std::string hhmm(int minute)
{
    char t[8];
    snprintf(t, sizeof(t), "%02d:%02d", (minute / 60) % 24, minute % 60);
    return t;
}

bool parse_hhmm(const std::string &text, int &minute)
{
    int h = -1, m = -1;
    char extra = 0;
    if (sscanf(text.c_str(), "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        if (sscanf(text.c_str(), "%d:%d %c", &h, &m, &extra) == 3 && (extra == 'p' || extra == 'P' || extra == 'a' || extra == 'A') &&
                h >= 1 && h <= 12 && m >= 0 && m <= 59) {
            h = h % 12 + (extra == 'p' || extra == 'P' ? 12 : 0);  // "7:30 pm"
        } else {
            return false;
        }
    }
    minute = h * 60 + m;
    return true;
}

std::string days_text(uint8_t days)
{
    days &= 0x7F;
    if (days == 0x7F) {
        return "every day";
    }
    if (days == 0x3E) {
        return "on weekdays";
    }
    if (days == 0x41) {
        return "at weekends";
    }
    static const char *const SHORT[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::string out = "on ";
    bool first = true;
    for (int d = 1; d <= 7; ++d)
        if (days & (1 << (d % 7))) {
            out += (first ? "" : ", ") + std::string(SHORT[d % 7]);
            first = false;
        }
    return out;
}

std::string action_text(const Action &a)
{
    char t[96];
    switch (a.kind) {
    case Do::Home:
        return a.verb + (a.target.empty() ? "" : " " + a.target) + (a.value.empty() ? "" : " " + a.value);
    case Do::Ir:
        return "TV remote: " + a.value;
    case Do::BoardLights:
        return "Interaction lights: " + a.verb + (a.value.empty() ? "" : " " + a.value);
    case Do::Brightness:
        snprintf(t, sizeof(t), "Screen brightness %d%%", a.n);
        return t;
    case Do::Open:
        return "Open " + a.verb + (a.value.empty() ? "" : " " + a.value);
    case Do::Flashlight:
        return a.n ? "Flashlight on" : "Flashlight off";
    default:
        return "";
    }
}

std::string cond_text(const Cond &c)
{
    switch (c.kind) {
    case Check::And:
    case Check::Or: {
        std::string out;
        for (size_t i = 0; i < c.kids.size(); ++i) {
            const bool group = c.kids[i].kind == Check::And || c.kids[i].kind == Check::Or;
            out += (i ? (c.kind == Check::And ? " and " : " or ") : "") + std::string(group ? "(" : "") +
                   cond_text(c.kids[i]) + (group ? ")" : "");
        }
        return out;
    }
    case Check::Not: {
        const Cond &k = c.kids.front();
        return k.kind == Check::And || k.kind == Check::Or ? "not (" + cond_text(k) + ")" : "not " + cond_text(k);
    }
    case Check::Time:
        return "it is " + hhmm(c.a) + "\xE2\x80\x93" + hhmm(c.b);
    case Check::Day: {
        const uint8_t days = uint8_t(c.a) & 0x7F;
        if (days == 0x3E || days == 0x41) {
            return days == 0x3E ? "it is a weekday" : "it is the weekend";
        }
        return "it is " + (days == 0x7F ? std::string("any day") : days_text(days).substr(3));  // "on Mon, Tue"
    }
    case Check::Home:
        if (c.cmp == Cmp::IsOn || c.cmp == Cmp::IsOff) {
            return c.name + " is " + (c.cmp == Cmp::IsOn ? "on" : "off");
        }
        return c.name + " is " + name_of(c.cmp) + " " + number_text(c.num) + "\xC2\xB0";
    case Check::Motion:
        return c.a ? "motion in the last " + std::to_string(c.a) + " s" : "there is motion";
    case Check::Button:
        return c.name == "any" ? "a button is held" : "the " + c.name + " button is held";
    case Check::Light:
        return "light is " + std::string(name_of(c.cmp)) + " " + number_text(c.num) + "%";
    case Check::Wifi:
        return "Wi-Fi is connected";
    case Check::Vpn:
        return "the VPN is up";
    case Check::Battery:
        return "battery is " + std::string(name_of(c.cmp)) + " " + number_text(c.num) + "%";
    case Check::Charging:
        return "the board is charging";
    case Check::Var:
        return c.name + (c.cmp == Cmp::Equals ? " = " : c.cmp == Cmp::Above ? " > " : " < ") + number_text(c.num);
    }
    return "";
}

std::string trigger_text(const Trigger &t)
{
    switch (t.kind) {
    case On::Tile:
        return "Tap its tile";
    case On::Time:
        return "At " + hhmm(t.at) + " " + days_text(t.days);
    case On::Every: {
        std::string out = "Every " + (t.n % 60 == 0 && t.n >= 60 ? std::to_string(t.n / 60) + " h" : std::to_string(t.n) + " min");
        if (t.at >= 0) {
            out += ", " + hhmm(t.at) + "\xE2\x80\x93" + hhmm(t.to);
        }
        return out + (t.days == 0x7F ? "" : " " + days_text(t.days));
    }
    case On::Boot:
        return "At start-up" + (t.n ? " (after " + std::to_string(t.n) + " s)" : std::string());
    case On::Motion:
        return "On motion (at most every " + (t.n >= 60 && t.n % 60 == 0 ? std::to_string(t.n / 60) + " min)" : std::to_string(t.n) + " s)");
    case On::Button:
        return t.name == "any" ? "Either Interaction button" : "The " + t.name + " Interaction button";
    case On::Home:
        if (t.cmp == Cmp::IsOn || t.cmp == Cmp::IsOff) {
            return "When " + t.name + " turns " + (t.cmp == Cmp::IsOn ? "on" : "off");
        }
        if (t.cmp == Cmp::None) {
            return "When " + t.name + " changes";
        }
        return "When " + t.name + " goes " + name_of(t.cmp) + " " + number_text(t.num) + "\xC2\xB0";
    case On::Sun: {
        std::string out = "At " + t.name;
        if (t.n) {
            out = std::to_string(std::abs(t.n)) + " min " + (t.n < 0 ? "before " : "after ") + t.name;
        }
        return out + (t.days == 0x7F ? "" : " " + days_text(t.days));
    }
    }
    return "";
}

void steps_lines(const std::vector<Node> &steps, std::vector<std::string> &lines, ActionWords action, int indent)
{
    const std::string pad(size_t(indent) * 3, ' ');
    for (const Node &n : steps) {
        switch (n.kind) {
        case Do::Wait: {
            char t[48];
            if (n.n >= 60000 && n.n % 60000 == 0) {
                snprintf(t, sizeof(t), "Wait %d min", n.n / 60000);
            } else {
                snprintf(t, sizeof(t), "Wait %g s", n.n / 1000.0);
            }
            lines.push_back(pad + t);
            break;
        }
        case Do::If:
            lines.push_back(pad + "If " + cond_text(n.cond) + ":");
            steps_lines(n.body, lines, action, indent + 1);
            for (const Branch &b : n.elifs) {
                lines.push_back(pad + "Else if " + cond_text(b.cond) + ":");
                steps_lines(b.body, lines, action, indent + 1);
            }
            if (n.has_else) {
                lines.push_back(pad + "Otherwise:");
                steps_lines(n.other, lines, action, indent + 1);
            }
            break;
        case Do::Repeat:
            lines.push_back(pad + (n.n ? "Repeat " + std::to_string(n.n) + " times:" :
                                   "While " + cond_text(n.cond) + " (" + std::to_string(n.max) + " rounds at most):"));
            steps_lines(n.body, lines, action, indent + 1);
            break;
        case Do::Set:
            lines.push_back(pad + "Set " + n.text + " to " + std::to_string(n.n));
            break;
        case Do::Add:
            lines.push_back(pad + (n.n < 0 ? "Take " + std::to_string(-n.n) + " from " : "Add " + std::to_string(n.n) + " to ") + n.text);
            break;
        case Do::Message:
            lines.push_back(pad + "Show \xE2\x80\x9C" + n.text + "\xE2\x80\x9D" + (n.n & 2 ? " with a chime" : n.n & 1 ? " with a buzz" : ""));
            break;
        case Do::Run:
            lines.push_back(pad + "Run " + (n.text.empty() ? "shortcut " + std::to_string(n.n) : n.text));
            break;
        case Do::Stop:
            lines.push_back(pad + "Stop");
            break;
        default:
            lines.push_back(pad + (action ? action(n.act) : action_text(n.act)));
            break;
        }
    }
}

std::string summary_text(const Shortcut &sc, ActionWords action)
{
    std::string when;
    for (const Trigger &t : sc.triggers)
        if (t.kind != On::Tile) {
            when = trigger_text(t);
            if (sc.triggers.size() > (sc.tile() ? 2u : 1u)) {
                when += " +" + std::to_string(sc.triggers.size() - (sc.tile() ? 2 : 1));
            }
            break;
        }
    size_t count = 0;
    each_node(sc.steps, [&](const Node &) {
        ++count;
    });
    std::string what;
    if (count == 1 && sc.steps.front().kind <= Do::Flashlight) {
        what = action ? action(sc.steps.front().act) : action_text(sc.steps.front().act);
    } else {
        what = std::to_string(count) + " steps";
    }
    if (when.empty()) {
        return sc.triggers.empty() ? "Run by hand \xC2\xB7 " + what : what;
    }
    return (sc.enabled ? "" : "Off \xC2\xB7 ") + when + " \xC2\xB7 " + what;
}

// ---- running ---------------------------------------------------------------------------
const char *source_name(Source s)
{
    return SOURCE_NAMES[int(s)];
}

bool source_automatic(Source s)
{
    return s >= Source::Time;
}

namespace {
struct Vars {
    std::vector<std::pair<std::string, int32_t>> v;
    int32_t get(const std::string &name) const
    {
        for (const auto &[k, x] : v)
            if (k == name) {
                return x;
            }
        return 0;
    }
    bool set(const std::string &name, int64_t x)
    {
        x = std::clamp<int64_t>(x, INT32_MIN, INT32_MAX);
        for (auto &[k, y] : v)
            if (k == name) {
                y = int32_t(x);
                return true;
            }
        if (v.size() >= MAX_VARS) {
            return false;
        }
        v.push_back({name, int32_t(x)});
        return true;
    }
};

enum class Flow : uint8_t { Next, Stop, Fail };

class Interpreter {
public:
    Interpreter(Env &env, bool dry, const Limits &limits, RunResult &r) : env_(env), dry_(dry), lim_(limits), r_(r)
    {
        start_ = env.now_ms();
    }

    Flow shortcut(const Shortcut &sc, const std::string &prefix)
    {
        calls_.push_back(sc.id);
        Vars vars;
        const Flow f = block(sc.steps, prefix, vars);
        calls_.pop_back();
        return f;
    }

    int64_t elapsed() const
    {
        return env_.now_ms() - start_ + virtual_ms_;
    }

private:
    Env &env_;
    bool dry_;
    const Limits &lim_;
    RunResult &r_;
    int64_t start_ = 0, virtual_ms_ = 0;
    int rounds_ = 0;
    size_t dropped_ = 0;
    std::vector<uint32_t> calls_;

public:
    void note(const std::string &path, const std::string &text)
    {
        if (r_.log.size() + 1 >= MAX_LOG) {
            ++dropped_;
            return;
        }
        r_.log.push_back(trimmed((path.empty() ? "" : path + " ") + text, 96));
    }

    void finish()
    {
        if (dropped_) {
            r_.log.push_back("\xE2\x80\xA6 " + std::to_string(dropped_) + " more");
        }
    }

private:
    Flow fail(const std::string &path, const std::string &why)
    {
        r_.ok = false;
        r_.message = (path.empty() ? "" : "Step " + path + ": ") + why;
        note(path, "failed: " + why);
        return Flow::Fail;
    }

    bool budget(const std::string &path, Flow &f)
    {
        if (env_.stopped()) {
            f = fail("", "Stopped.");
            return false;
        }
        if (++r_.steps > lim_.steps) {
            f = fail(path, "too many steps run (" + std::to_string(lim_.steps) + "): does a loop never end?");
            return false;
        }
        if (elapsed() > lim_.run_ms) {
            f = fail(path, "ran longer than " + std::to_string(lim_.run_ms / 60000) + " min.");
            return false;
        }
        return true;
    }

    bool round(const std::string &path, Flow &f)
    {
        if (++rounds_ > lim_.rounds) {
            f = fail(path, "too many loop rounds (" + std::to_string(lim_.rounds) + ").");
            return false;
        }
        return true;
    }

    bool eval(const Cond &c, const Vars &vars, bool &value, std::string &error)
    {
        switch (c.kind) {
        case Check::And:
        case Check::Or:
            value = c.kind == Check::And;
            for (const Cond &k : c.kids) {
                bool v = false;
                if (!eval(k, vars, v, error)) {
                    return false;
                }
                if (c.kind == Check::And ? !v : v) {
                    value = !value;
                    return true;
                }
            }
            return true;
        case Check::Not:
            if (!eval(c.kids.front(), vars, value, error)) {
                return false;
            }
            value = !value;
            return true;
        case Check::Time:
        case Check::Day: {
            struct tm local;
            if (!env_.local_time(local)) {
                error = "the board's clock is not set yet";
                return false;
            }
            value = c.kind == Check::Time ? in_window(local.tm_hour * 60 + local.tm_min, c.a, c.b)
                    : (c.a & (1 << local.tm_wday)) != 0;
            return true;
        }
        case Check::Var: {
            const float x = float(vars.get(c.name));
            value = c.cmp == Cmp::Equals ? fabsf(x - c.num) < 0.5f : c.cmp == Cmp::Above ? x > c.num : x < c.num;
            return true;
        }
        default:
            return env_.read(c, value, error);
        }
    }

    Flow block(const std::vector<Node> &steps, const std::string &prefix, Vars &vars)
    {
        for (size_t i = 0; i < steps.size(); ++i) {
            const Flow f = step(steps[i], prefix + std::to_string(i + 1), vars);
            if (f != Flow::Next) {
                return f;
            }
        }
        return Flow::Next;
    }

    std::string interpolate(const std::string &text, const Vars &vars)
    {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            const size_t close = text[i] == '{' ? text.find('}', i) : std::string::npos;
            if (close != std::string::npos && var_name_ok(text.substr(i + 1, close - i - 1))) {
                out += std::to_string(vars.get(text.substr(i + 1, close - i - 1)));
                i = close;
            } else {
                out += text[i];
            }
        }
        return out;
    }

    Flow step(const Node &n, const std::string &path, Vars &vars)
    {
        Flow f = Flow::Next;
        if (!budget(path, f)) {
            return f;
        }
        switch (n.kind) {
        case Do::Wait: {
            char t[48];
            snprintf(t, sizeof(t), "Wait %g s%s", n.n / 1000.0, dry_ ? " (not waited: a test)" : "");
            note(path, t);
            if (elapsed() + n.n > lim_.run_ms) {
                return fail(path, "this wait would pass the run's limit of " + std::to_string(lim_.run_ms / 60000) + " min.");
            }
            if (dry_) {
                virtual_ms_ += n.n;
            } else if (!env_.sleep(n.n)) {
                return fail("", "Stopped.");
            }
            return Flow::Next;
        }
        case Do::If: {
            std::string error;
            bool value = false;
            if (!eval(n.cond, vars, value, error)) {
                return fail(path, "could not check " + cond_text(n.cond) + ": " + error);
            }
            note(path, "If " + cond_text(n.cond) + ": " + (value ? "yes" : "no"));
            if (value) {
                return block(n.body, path + ".", vars);
            }
            for (const Branch &b : n.elifs) {
                if (!eval(b.cond, vars, value, error)) {
                    return fail(path, "could not check " + cond_text(b.cond) + ": " + error);
                }
                note(path, "Else if " + cond_text(b.cond) + ": " + (value ? "yes" : "no"));
                if (value) {
                    return block(b.body, path + ".", vars);
                }
            }
            if (n.has_else) {
                note(path, "Otherwise");
                return block(n.other, path + ".", vars);
            }
            return Flow::Next;
        }
        case Do::Repeat: {
            if (n.n) {
                for (int i = 1; i <= n.n; ++i) {
                    if (!round(path, f)) {
                        return f;
                    }
                    note(path, "Round " + std::to_string(i) + " of " + std::to_string(n.n));
                    f = block(n.body, path + ".", vars);
                    if (f != Flow::Next) {
                        return f;
                    }
                }
                return Flow::Next;
            }
            for (int i = 1;; ++i) {
                std::string error;
                bool value = false;
                if (!eval(n.cond, vars, value, error)) {
                    return fail(path, "could not check " + cond_text(n.cond) + ": " + error);
                }
                if (!value) {
                    note(path, "While " + cond_text(n.cond) + ": no, after " + std::to_string(i - 1) + " rounds");
                    return Flow::Next;
                }
                if (i > n.max) {
                    note(path, "While: stopped at its " + std::to_string(n.max) + " rounds");
                    return Flow::Next;
                }
                if (!round(path, f)) {
                    return f;
                }
                note(path, "Round " + std::to_string(i));
                f = block(n.body, path + ".", vars);
                if (f != Flow::Next) {
                    return f;
                }
                if (!budget(path, f)) {
                    return f;
                }
            }
        }
        case Do::Set:
        case Do::Add: {
            const int64_t x = n.kind == Do::Set ? int64_t(n.n) : int64_t(vars.get(n.text)) + n.n;
            if (!vars.set(n.text, x)) {
                return fail(path, "a run has at most " + std::to_string(MAX_VARS) + " variables.");
            }
            note(path, n.text + " = " + std::to_string(vars.get(n.text)));
            return Flow::Next;
        }
        case Do::Message: {
            const std::string text = interpolate(n.text, vars);
            note(path, "Show \xE2\x80\x9C" + text + "\xE2\x80\x9D");
            env_.message(text, n.n & 1, n.n & 2);
            return Flow::Next;
        }
        case Do::Run: {
            std::shared_ptr<const Shortcut> other = env_.find(uint32_t(n.n));
            if (!other) {
                return fail(path, "there is no shortcut " + (n.text.empty() ? std::to_string(n.n) : "\"" + n.text + "\"") +
                            " any more.");
            }
            if (std::find(calls_.begin(), calls_.end(), other->id) != calls_.end()) {
                return fail(path, "\"" + other->name + "\" is already running here (a shortcut cannot run itself).");
            }
            if (int(calls_.size()) > lim_.calls) {
                return fail(path, "shortcuts run inside one another at most " + std::to_string(lim_.calls) + " deep.");
            }
            note(path, "Run \"" + other->name + "\"");
            const Flow inner = shortcut(*other, path + ">");
            if (inner == Flow::Fail) {
                r_.message = "Step " + path + " (" + other->name + "): " + r_.message;
                return Flow::Fail;
            }
            return Flow::Next;  // its stop ends it, not this one
        }
        case Do::Stop:
            note(path, "Stop");
            r_.message = "Stopped at step " + path;
            return Flow::Stop;
        default: {
            std::string what, error;
            ++r_.actions;
            const bool ok = env_.act(n.act, dry_, what, error);
            if (!ok) {
                return fail(path, error.empty() ? "that did not work." : error);
            }
            note(path, (what.empty() ? action_text(n.act) : what) + (dry_ ? ": checked" : ": ok"));
            return Flow::Next;
        }
        }
    }
};
}  // namespace

RunResult run(const Shortcut &sc, Env &env, bool dry, const Limits &limits)
{
    RunResult r;
    Interpreter in(env, dry, limits, r);
    const Flow f = in.shortcut(sc, "");
    in.finish();
    r.ms = in.elapsed();
    if (f != Flow::Fail) {
        r.ok = true;
        if (f != Flow::Stop) {
            r.message = dry ? "Test: nothing was sent, " + std::to_string(r.steps) + (r.steps == 1 ? " step" : " steps")
                        : r.steps > 1 ? "Done, " + std::to_string(r.steps) + " steps" : "Done";
        }
    }
    return r;
}

// ---- schedules -------------------------------------------------------------------------
bool in_window(int minute, int from, int to)
{
    return from <= to ? minute >= from && minute <= to : minute >= from || minute <= to;
}

int64_t days_from_civil(int y, int m, int d)
{
    // H. Hinnant's days_from_civil: proleptic Gregorian, no time zone.
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

bool sun_utc_minutes(int year, int month, int day, float lat, float lon, bool rise, double &minutes)
{
    // The Almanac for Computers' sunrise and sunset (zenith 90°50'), within a
    // minute or two away from the poles.
    constexpr double RAD = M_PI / 180.0;
    const int n = int(days_from_civil(year, month, day) - days_from_civil(year, 1, 1)) + 1;
    const double lng_hour = lon / 15.0;
    const double t = n + ((rise ? 6.0 : 18.0) - lng_hour) / 24.0;
    const double m = 0.9856 * t - 3.289;
    double l = m + 1.916 * sin(m * RAD) + 0.020 * sin(2 * m * RAD) + 282.634;
    l = fmod(l + 720.0, 360.0);
    double ra = atan(0.91764 * tan(l * RAD)) / RAD;
    ra = fmod(ra + 720.0, 360.0);
    ra += floor(l / 90.0) * 90.0 - floor(ra / 90.0) * 90.0;
    ra /= 15.0;
    const double sin_dec = 0.39782 * sin(l * RAD);
    const double cos_dec = cos(asin(sin_dec));
    const double cos_h = (cos(90.833 * RAD) - sin_dec * sin(lat * RAD)) / (cos_dec * cos(lat * RAD));
    if (cos_h > 1 || cos_h < -1) {
        return false;  // polar night or midnight sun
    }
    double h = acos(cos_h) / RAD;
    h = (rise ? 360.0 - h : h) / 15.0;
    const double local_t = h + ra - 0.06571 * t - 6.622;
    minutes = fmod(local_t - lng_hour + 48.0, 24.0) * 60.0;
    return true;
}

namespace {
// The sun's event on the local date of `local`, as a time. 0: none.
time_t sun_event(const Trigger &t, const struct tm &local, const Location &loc)
{
    const int y = local.tm_year + 1900, m = local.tm_mon + 1, d = local.tm_mday;
    // The event can fall on the UTC day before or after the local one.
    for (int k = -1; k <= 1; ++k) {
        const int64_t day = days_from_civil(y, m, d) + k;
        // That UTC day's date back from the day count.
        const time_t noon = time_t(day * 86400 + 43200);
        struct tm u;
        gmtime_r(&noon, &u);
        double minutes = 0;
        if (!sun_utc_minutes(u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, loc.lat, loc.lon, t.name == "sunrise", minutes)) {
            continue;
        }
        const time_t at = time_t(day * 86400 + int64_t(llround(minutes * 60.0)) + t.n * 60);
        struct tm l;
        localtime_r(&at, &l);
        if (l.tm_year == local.tm_year && l.tm_yday == local.tm_yday) {
            return at;
        }
    }
    return 0;
}
}  // namespace

bool fires_at(const Trigger &t, const struct tm &local, time_t minute_start, const Location &loc)
{
    if (!(t.days & (1 << local.tm_wday))) {
        return false;
    }
    const int minute = local.tm_hour * 60 + local.tm_min;
    switch (t.kind) {
    case On::Time:
        return minute == t.at;
    case On::Every: {
        if (t.at < 0) {
            return minute % t.n == 0;
        }
        return in_window(minute, t.at, t.to) && ((minute - t.at + 1440) % 1440) % t.n == 0;
    }
    case On::Sun: {
        if (!loc.set) {
            return false;
        }
        const time_t at = sun_event(t, local, loc);
        return at && at >= minute_start && at < minute_start + 60;
    }
    default:
        return false;
    }
}

time_t next_fire(const Trigger &t, time_t after, const Location &loc)
{
    if (t.kind != On::Time && t.kind != On::Every && t.kind != On::Sun) {
        return 0;
    }
    struct tm base;
    localtime_r(&after, &base);
    for (int day = 0; day <= 8; ++day) {
        struct tm d = base;
        d.tm_mday += day;
        d.tm_hour = 12, d.tm_min = 0, d.tm_sec = 0;
        d.tm_isdst = -1;
        const time_t noon = mktime(&d);
        struct tm local;
        localtime_r(&noon, &local);  // normalised: its weekday
        if (!(t.days & (1 << local.tm_wday))) {
            continue;
        }
        if (t.kind == On::Sun) {
            if (!loc.set) {
                return 0;
            }
            const time_t at = sun_event(t, local, loc);
            if (at > after) {
                return at - at % 60;
            }
            continue;
        }
        // Each minute it may fire that day, earliest first.
        std::vector<int> minutes;
        if (t.kind == On::Time) {
            minutes.push_back(t.at);
        } else if (t.at < 0) {
            for (int m = 0; m < 1440; m += t.n) {
                minutes.push_back(m);
            }
        } else {
            for (int k = 0; k < 1440; k += t.n) {
                const int m = (t.at + k) % 1440;
                if (!in_window(m, t.at, t.to)) {
                    break;
                }
                minutes.push_back(m);
            }
            std::sort(minutes.begin(), minutes.end());
        }
        for (int m : minutes) {
            struct tm c = local;
            c.tm_hour = m / 60, c.tm_min = m % 60, c.tm_sec = 0;
            c.tm_isdst = -1;
            const time_t at = mktime(&c);
            if (at > after) {
                return at;
            }
        }
    }
    return 0;
}

}  // namespace shortcuts
