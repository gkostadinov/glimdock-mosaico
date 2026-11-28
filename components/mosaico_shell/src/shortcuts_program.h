// SPDX-License-Identifier: Apache-2.0
// Shortcuts' programs (see shortcuts.h): the model, its JSON (stored, sent to
// Web settings and written by Swifty), the checks with readable errors, the
// steps and triggers in words, the interpreter and the schedules' clock
// arithmetic. Plain C++ over ArduinoJson with nothing of the board's: the
// engine (shortcuts.cpp) gives the interpreter its actions and readings
// through Env, and the host tests (host/shortcuts_test.cpp) give it fakes.
//
// A shortcut, as JSON (format 2; see shortcuts.h for every field):
//   {"id":3,"name":"Evening","icon":"bulb","accent":"amber","enabled":true,
//    "triggers":[{"on":"tile"},{"on":"time","at":"19:30","days":["mon","fri"]}],
//    "steps":[{"do":"if","if":{"check":"home","device":"Desk lamp","is":"off"},
//              "then":[{"do":"home","verb":"lights_on","target":"Living room"}],
//              "else":[{"do":"message","text":"Already on"}]}]}
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <strings.h>
#include <time.h>

#include <memory>
#include <string>
#include <vector>

#include "ArduinoJson.h"

namespace shortcuts {

// The stored format ("v"). Format 1 (Glimdock 1.2.0) had no "v": up to ten
// actions run in order, every shortcut a tile.
constexpr int FORMAT = 2;

// What one shortcut may hold. Checked when it is made, so a run never meets
// more than this.
constexpr size_t MAX_COUNT = 24;     // shortcuts on the board
constexpr size_t MAX_NAME = 32;      // bytes
constexpr size_t MAX_TEXT = 120;     // a message's bytes
constexpr size_t MAX_NODES = 48;     // steps, counting those inside ifs and repeats
constexpr int MAX_DEPTH = 4;         // ifs and repeats inside one another
constexpr size_t MAX_TRIGGERS = 6;
constexpr size_t MAX_CONDS = 12;     // parts of one condition (and, or, not count)
constexpr size_t MAX_VARS = 8;       // variables in one run
constexpr int MAX_WAIT_MS = 3600 * 1000;
constexpr int MAX_TIMES = 100;       // repeat N times
constexpr int MAX_WHILE = 100;       // repeat while: at most this many rounds
constexpr int DEFAULT_WHILE = 10;

// What one run may do, counted as it goes (a run that reaches one stops and
// fails, saying which).
struct Limits {
    int steps = 500;                      // steps run, all levels
    int rounds = 1000;                    // loop rounds, all loops
    int64_t run_ms = 2 * 3600 * 1000LL;   // time, waits included
    int calls = 3;                        // "run another shortcut" inside one another
};

enum class Do : uint8_t {
    // Actions (format 1's steps, wait aside): the engine runs these.
    Home, Ir, BoardLights, Brightness, Open, Flashlight,
    // The program's own.
    Wait, If, Repeat, Set, Add, Message, Run, Stop,
};

// An action. home: verb, target, value. ir: value (the key). board_lights:
// verb (the mode), value (the colour). open: verb (the app), value (a tool).
// brightness: n percent. flashlight: n 1 on, 0 off.
struct Action {
    Do kind = Do::Home;
    std::string verb, target, value;
    int n = 0;
};

enum class Check : uint8_t { And, Or, Not, Time, Day, Home, Motion, Button, Light, Wifi, Vpn, Battery, Charging, Var };
enum class Cmp : uint8_t { None, Above, Below, Equals, IsOn, IsOff };

// A condition. time: a from, b to (minutes of the day; to before from runs
// past midnight). day: a the weekdays (bit 0 Sunday). home: name the device
// (or room), cmp IsOn/IsOff or Above/Below num (a temperature). motion: a
// seconds (0: now). button: name left, right or any (pressed now). light,
// battery: Above/Below num percent. var: name, Equals/Above/Below num.
struct Cond {
    Check kind = Check::Wifi;
    std::vector<Cond> kids;  // and, or: two or more; not: one
    std::string name;
    Cmp cmp = Cmp::None;
    float num = 0;
    int a = 0, b = 0;
};

struct Node;
struct Branch {
    Cond cond;
    std::vector<Node> body;
};

// A step. wait: n ms. repeat: n times (0: while cond, up to max rounds).
// set, add: text the variable, n the value. message: text, n bit 0 a buzz,
// bit 1 the chime. run: n the shortcut's id (text its name when it was made).
// if: cond, body (then), elifs, other (else, when has_else).
struct Node {
    Do kind = Do::Stop;
    Action act;
    int n = 0, max = 0;
    std::string text;
    Cond cond;
    std::vector<Node> body, other;
    std::vector<Branch> elifs;
    bool has_else = false;
};

enum class On : uint8_t { Tile, Time, Every, Boot, Motion, Button, Home, Sun };

// A trigger. time: at (minute of the day), days. every: n minutes from at to
// to (minutes of the day; both -1: all day), days. boot: n seconds after
// start-up. motion: n seconds before it can start the shortcut again.
// button: name left, right or any. home: name the device, cmp IsOn/IsOff
// (or None: any change), or Above/Below num (crossing a temperature). sun:
// name sunrise or sunset, n minutes after it (negative: before), days.
struct Trigger {
    On kind = On::Tile;
    int at = -1, to = -1, n = 0;
    uint8_t days = 0x7F;
    std::string name;
    Cmp cmp = Cmp::None;
    float num = 0;
};

struct Shortcut {
    uint32_t id = 0;
    std::string name, icon;
    uint8_t accent = 0;
    bool enabled = true;
    std::vector<Trigger> triggers;
    std::vector<Node> steps;

    bool tile() const;       // a tile on the home screen
    bool automatic() const;  // something but a tap starts it
};

// Where the sun is reckoned from, for sunrise and sunset (set in Web settings).
struct Location {
    bool set = false;
    float lat = 0, lon = 0;
};

// ---- names -----------------------------------------------------------------------------
extern const char *const HOME_VERBS[17];
extern const char *const IR_KEYS[14];
extern const char *const BOARD_MODES[5];
extern const char *const APPS[11];
extern const char *const TOOLS[3];
extern const char *const ICONS[24];
extern const char *const ACCENTS[4];
extern const char *const DAY_NAMES[7];  // sun ... sat
template <size_t N> int index_of(const char *const(&names)[N], const std::string &name);
std::string trimmed(std::string s, size_t max_bytes);
std::string lowered(std::string s);
const char *kind_name(Do kind);  // "home", "wait", "if", ...

// ---- JSON ------------------------------------------------------------------------------
// Checks and reads a shortcut. A format 1 shortcut reads as one (no triggers:
// a tile). error: what is wrong and where ("step 3.2: wait needs seconds").
bool parse_shortcut(JsonObjectConst o, Shortcut &out, std::string &error);
void shortcut_json(const Shortcut &sc, JsonObject o);
void steps_json(const std::vector<Node> &steps, JsonArray a);
void cond_json(const Cond &c, JsonObject o);
void trigger_json(const Trigger &t, JsonObject o);
// The stored document, brought up to FORMAT in place: format 1's shortcuts
// get their tile trigger and "enabled" written out. False: not a shortcuts
// document. migrated: it was older.
bool migrate(JsonDocument &doc, bool *migrated = nullptr);
// Every node, depth first (for the engine's checks: Home names, run steps).
template <typename F> void each_node(std::vector<Node> &steps, F f);
template <typename F> void each_node(const std::vector<Node> &steps, F f);
template <typename F> void each_cond(const Cond &c, F f);

// ---- words -----------------------------------------------------------------------------
std::string hhmm(int minute);
bool parse_hhmm(const std::string &text, int &minute);
std::string days_text(uint8_t days);
// An action in plain words (Home's own words come from the engine).
std::string action_text(const Action &a);
std::string cond_text(const Cond &c);
std::string trigger_text(const Trigger &t);
// The steps as indented lines ("If ...", "  Lights off", "Otherwise").
// action: the words for an action (nullptr: action_text).
using ActionWords = std::string (*)(const Action &a);
void steps_lines(const std::vector<Node> &steps, std::vector<std::string> &lines, ActionWords action = nullptr,
                 int indent = 0);
// One line for a list: when it runs and how much it does.
std::string summary_text(const Shortcut &sc, ActionWords action = nullptr);

// ---- running ---------------------------------------------------------------------------
// Who started a run: shown in its log and handed to the action hook.
enum class Source : uint8_t { Tile, Editor, Swifty, Web, Rpc, Test, Time, Every, Boot, Motion, Button, Home, Sun };
const char *source_name(Source s);
bool source_automatic(Source s);  // started by a trigger, nobody tapping

// What the interpreter needs from the board.
struct Env {
    virtual ~Env() = default;
    // Runs an action (dry: checked and logged, nothing sent). what: what it did.
    virtual bool act(const Action &a, bool dry, std::string &what, std::string &error) = 0;
    // A reading: the leaves home, motion, button, light, wifi, vpn, battery and charging.
    virtual bool read(const Cond &leaf, bool &value, std::string &error) = 0;
    // Sleeps (false: the run was stopped meanwhile).
    virtual bool sleep(int ms) = 0;
    virtual int64_t now_ms() = 0;               // monotonic
    virtual bool local_time(struct tm &out) = 0;  // false: the clock is not set yet
    virtual void message(const std::string &text, bool buzz, bool chime) = 0;
    virtual std::shared_ptr<const Shortcut> find(uint32_t id) = 0;
    virtual bool stopped() = 0;  // stop asked from outside (the editor)
};

struct RunResult {
    bool ok = true;
    std::string message;            // how it went ("Done, 4 steps", "Step 3: ...")
    std::vector<std::string> log;   // a short line a step
    int steps = 0, actions = 0;
    int64_t ms = 0;
};

constexpr size_t MAX_LOG = 40;  // lines kept of one run

// Runs sc's steps. dry: no action is sent and waits are not waited (their
// time still counts toward the limit).
RunResult run(const Shortcut &sc, Env &env, bool dry, const Limits &limits = Limits());

// ---- schedules -------------------------------------------------------------------------
// minute in [from, to], both inclusive; past midnight when to < from.
bool in_window(int minute, int from, int to);
// Whether a time, every or sun trigger starts the shortcut in the local
// minute that begins at minute_start (local: that minute, broken down).
bool fires_at(const Trigger &t, const struct tm &local, time_t minute_start, const Location &loc);
// Sunrise or sunset on a date, in minutes after that date's midnight UTC (may
// fall outside 0..1440). False: the sun does not rise or set that day.
bool sun_utc_minutes(int year, int month, int day, float lat, float lon, bool rise, double &minutes);
// Days since 1970-01-01 of a civil date (no time zone).
int64_t days_from_civil(int year, int month, int day);
// The next time a time, every or sun trigger fires after `after` (looked for
// over eight days, minute by minute). 0: none.
time_t next_fire(const Trigger &t, time_t after, const Location &loc);

// ---- templates -------------------------------------------------------------------------
template <size_t N> int index_of(const char *const(&names)[N], const std::string &name)
{
    for (size_t i = 0; i < N; ++i)
        if (!strcasecmp(names[i], name.c_str())) {
            return int(i);
        }
    return -1;
}

template <typename F> void each_node(std::vector<Node> &steps, F f)
{
    for (Node &n : steps) {
        f(n);
        each_node(n.body, f);
        for (Branch &b : n.elifs) {
            each_node(b.body, f);
        }
        each_node(n.other, f);
    }
}

template <typename F> void each_node(const std::vector<Node> &steps, F f)
{
    for (const Node &n : steps) {
        f(n);
        each_node(n.body, f);
        for (const Branch &b : n.elifs) {
            each_node(b.body, f);
        }
        each_node(n.other, f);
    }
}

template <typename F> void each_cond(const Cond &c, F f)
{
    f(c);
    for (const Cond &k : c.kids) {
        each_cond(k, f);
    }
}

}  // namespace shortcuts
