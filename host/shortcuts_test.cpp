// SPDX-License-Identifier: Apache-2.0
// Host tests of the shortcuts' programs (shortcuts_program.cpp): the format 1
// migration, the checks and their errors, the interpreter (conditions, loops,
// variables, limits, running other shortcuts) and the schedules' arithmetic.
// Built by host/CMakeLists.txt as shortcuts-test, or on its own:
//   c++ -std=c++17 -Icomponents/mosaico_shell/src -Imanaged_components/bblanchon__arduinojson/src \
//       host/shortcuts_test.cpp components/mosaico_shell/src/shortcuts_program.cpp -o build-host/shortcuts-test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>
#include <map>
#include <string>

#include "shortcuts_program.h"

using namespace shortcuts;

namespace {
int failures = 0, checks = 0;

#define CHECK(cond) check_(cond, #cond, __LINE__)
#define CHECK_EQ(a, b) check_eq_(a, b, #a, __LINE__)

void check_(bool ok, const char *what, int line)
{
    ++checks;
    if (!ok) {
        ++failures;
        printf("FAIL line %d: %s\n", line, what);
    }
}

void check_eq_(const std::string &got, const std::string &want, const char *what, int line)
{
    ++checks;
    if (got != want) {
        ++failures;
        printf("FAIL line %d: %s\n  got:  %s\n  want: %s\n", line, what, got.c_str(), want.c_str());
    }
}

bool parse(const char *json, Shortcut &sc, std::string &error)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, DeserializationOption::NestingLimit(24))) {
        error = "bad JSON in the test";
        return false;
    }
    return parse_shortcut(doc.as<JsonObjectConst>(), sc, error);
}

std::string error_of(const char *json)
{
    Shortcut sc;
    std::string error;
    return parse(json, sc, error) ? "ok" : error;
}

std::string to_json(const Shortcut &sc)
{
    JsonDocument doc;
    shortcut_json(sc, doc.to<JsonObject>());
    std::string out;
    serializeJson(doc, out);
    return out;
}

// A board made of variables: actions are recorded, readings come from a map,
// time passes only when slept.
struct FakeEnv : Env {
    std::vector<std::string> done;
    std::map<std::string, bool> readings;  // "motion", "wifi", "home:Desk lamp:on", ...
    std::map<uint32_t, std::shared_ptr<const Shortcut>> others;
    std::vector<std::string> messages;
    int64_t clock = 0;
    struct tm local = {};
    bool clock_set = true;
    std::function<bool(const Action &)> fail_when;
    std::function<void()> on_sleep;

    bool act(const Action &a, bool dry, std::string &what, std::string &error) override
    {
        what = action_text(a);
        if (fail_when && fail_when(a)) {
            error = "No answer from Swifty Home.";
            return false;
        }
        done.push_back((dry ? "dry " : "") + what);
        return true;
    }
    bool read(const Cond &c, bool &value, std::string &error) override
    {
        std::string key = c.kind == Check::Motion ? "motion" : c.kind == Check::Wifi ? "wifi" : c.kind == Check::Vpn ? "vpn"
                          : c.kind == Check::Home ? "home:" + c.name : c.kind == Check::Button ? "button:" + c.name : "other";
        auto it = readings.find(key);
        if (it == readings.end()) {
            error = "no reading for " + key;
            return false;
        }
        value = it->second;
        if (c.kind == Check::Home && c.cmp == Cmp::IsOff) {
            value = !value;
        }
        return true;
    }
    bool sleep(int ms) override
    {
        clock += ms;
        if (on_sleep) {
            on_sleep();
        }
        return true;
    }
    int64_t now_ms() override
    {
        return clock;
    }
    bool local_time(struct tm &out) override
    {
        out = local;
        return clock_set;
    }
    void message(const std::string &text, bool, bool) override
    {
        messages.push_back(text);
    }
    std::shared_ptr<const Shortcut> find(uint32_t id) override
    {
        auto it = others.find(id);
        return it == others.end() ? nullptr : it->second;
    }
    bool stopped() override
    {
        return false;
    }
};

struct tm at(int wday, int hour, int minute)
{
    struct tm t = {};
    t.tm_wday = wday;
    t.tm_hour = hour;
    t.tm_min = minute;
    return t;
}

// ---- migration ---------------------------------------------------------------------------
// A format 1 document as Glimdock 1.2.0 saved it (NVS "shortcuts"/"data").
const char *FORMAT1 = R"({"next":7,"seeded":true,"list":[
 {"id":1,"name":"All lights off","icon":"power","accent":"violet","steps":[{"do":"home","verb":"lights_off","target":"all"}]},
 {"id":2,"name":"Bedroom lights off","icon":"bulb","accent":"violet","steps":[{"do":"home","verb":"lights_off","target":"room-bedroom"}]},
 {"id":4,"name":"Movie night","icon":"tv","accent":"cyan","steps":[
   {"do":"home","verb":"scene","value":"movie"},{"do":"wait","seconds":2.5},{"do":"ir","key":"input"},
   {"do":"board_lights","mode":"colour","colour":"#3070FF"},{"do":"brightness","percent":40},
   {"do":"open","app":"home"},{"do":"flashlight","on":false},{"do":"home","verb":"volume","target":"","value":"20"}]},
 {"id":6,"name":"Torch","icon":"bulb","accent":"green","steps":[{"do":"open","app":"tools","tool":"torch"},{"do":"wait","ms":1500}]}
]})";

void test_migration()
{
    JsonDocument doc;
    CHECK(!deserializeJson(doc, FORMAT1));
    bool migrated = false;
    CHECK(migrate(doc, &migrated));
    CHECK(migrated);
    CHECK((doc["v"] | 0) == FORMAT);
    CHECK((doc["next"] | 0) == 7);
    CHECK(doc["seeded"] == true);
    std::vector<Shortcut> list;
    for (JsonObjectConst o : doc["list"].as<JsonArrayConst>()) {
        Shortcut sc;
        std::string error;
        CHECK(parse_shortcut(o, sc, error));
        if (!error.empty()) {
            printf("  %s\n", error.c_str());
        }
        list.push_back(sc);
    }
    CHECK(list.size() == 4);
    for (const Shortcut &sc : list) {
        CHECK(sc.enabled);
        CHECK(sc.tile());
        CHECK(!sc.automatic());
        CHECK(sc.triggers.size() == 1);
    }
    CHECK_EQ(list[0].name, "All lights off");
    CHECK(list[0].steps.size() == 1 && list[0].steps[0].kind == Do::Home);
    CHECK_EQ(list[0].steps[0].act.verb, "lights_off");
    CHECK_EQ(list[0].steps[0].act.target, "all");
    CHECK_EQ(list[1].steps[0].act.target, "room-bedroom");
    const Shortcut &movie = list[2];
    CHECK(movie.steps.size() == 8);
    CHECK_EQ(movie.icon, "tv");
    CHECK(movie.accent == 1);
    CHECK(movie.steps[1].kind == Do::Wait && movie.steps[1].n == 2500);
    CHECK(movie.steps[2].kind == Do::Ir);
    CHECK_EQ(movie.steps[2].act.value, "input");
    CHECK_EQ(movie.steps[3].act.value, "#3070FF");
    CHECK(movie.steps[4].act.n == 40);
    CHECK_EQ(movie.steps[5].act.verb, "home");
    CHECK(movie.steps[6].kind == Do::Flashlight && movie.steps[6].act.n == 0);
    CHECK_EQ(movie.steps[7].act.value, "20");
    CHECK(list[3].steps[1].n == 1500);  // "ms", as the oldest saves had it
    CHECK_EQ(list[3].steps[0].act.value, "torch");

    // Saved again and read back: the same shortcuts.
    for (const Shortcut &sc : list) {
        Shortcut again;
        std::string error;
        const std::string json = to_json(sc);
        CHECK(parse(json.c_str(), again, error));
        CHECK_EQ(to_json(again), json);
    }
    // A run of the migrated Movie night does what format 1 did, in order.
    FakeEnv env;
    const RunResult r = run(movie, env, false);
    CHECK(r.ok);
    CHECK(env.done.size() == 7);
    CHECK(env.clock == 2500);
    CHECK_EQ(env.done[0], "scene movie");
    CHECK_EQ(env.done[1], "TV remote: input");

    // Format 2 is left alone.
    bool again = true;
    CHECK(migrate(doc, &again));
    CHECK(!again);
    JsonDocument junk;
    deserializeJson(junk, "[1,2]");
    CHECK(!migrate(junk));
}

// ---- checks ------------------------------------------------------------------------------
void test_errors()
{
    CHECK_EQ(error_of(R"({"steps":[{"do":"stop"}]})"), "a shortcut needs a name");
    CHECK_EQ(error_of(R"({"name":"x","steps":[]})"), "steps are a list of one step or more");
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"fly"}]})"),
             "step 1: do is one of home, ir, board_lights, brightness, open, flashlight, wait, if, repeat, set, add, message, run, stop");
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"wait","seconds":2},{"do":"if","if":{"check":"time","from":"7","to":"08:00"},"then":[]}]})"),
             "step 2: if: time from needs a time as HH:MM (24-hour), e.g. \"07:30\"");
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"if","if":{"check":"wifi"},"then":[{"do":"wait","seconds":9000}]}]})"),
             "step 1.1: wait needs seconds, 0.1 to 3600 (or minutes, up to 60)");
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"repeat","times":500,"steps":[{"do":"stop"}]}]})"),
             "step 1: repeat times: 1 to 100");
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"home","verb":"lights dance!"}]})").substr(0, 34),
             "step 1: home verb \"lights dance!\":");
    // A Home command this firmware does not know yet is Home's to check.
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"home","verb":"ac_temp","target":"Bedroom AC","value":"22"}]})"), "ok");
    CHECK_EQ(error_of(R"({"name":"x","triggers":[{"on":"time","at":"25:00"}],"steps":[{"do":"stop"}]})"),
             "trigger 1: time at needs a time as HH:MM (24-hour), e.g. \"07:30\"");
    CHECK_EQ(error_of(R"({"name":"x","triggers":[{"on":"time","at":"07:00","days":["mon","funday"]}],"steps":[{"do":"stop"}]})"),
             "trigger 1: day \"funday\": one of sun, mon, tue, wed, thu, fri, sat");
    CHECK_EQ(error_of(R"({"name":"x","triggers":[{"on":"every","minutes":0}],"steps":[{"do":"stop"}]})"),
             "trigger 1: every needs minutes, 1 to 1440");
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"set","var":"1x","value":2}]})"),
             "step 1: set needs var (a name: a letter, then letters, digits or _)");
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"if","if":{"and":[{"check":"wifi"},{"check":"bogus"}]},"then":[]}]})").substr(0, 24),
             "step 1: if: check: one o");
    // Depth: four levels of blocks, not five.
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"repeat","times":2,"steps":[{"do":"repeat","times":2,"steps":[
        {"do":"repeat","times":2,"steps":[{"do":"repeat","times":2,"steps":[{"do":"stop"}]}]}]}]}]})"), "ok");
    CHECK_EQ(error_of(R"({"name":"x","steps":[{"do":"repeat","times":2,"steps":[{"do":"repeat","times":2,"steps":[
        {"do":"repeat","times":2,"steps":[{"do":"repeat","times":2,"steps":[{"do":"if","if":{"check":"wifi"},"then":[]}]}]}]}]}]})"),
             "step 1.1.1.1.1: ifs and repeats nest at most 4 deep");
    // Too many steps in all.
    std::string many = R"({"name":"x","steps":[)";
    for (int i = 0; i < 49; ++i) {
        many += std::string(i ? "," : "") + R"({"do":"wait","seconds":1})";
    }
    many += "]}";
    CHECK_EQ(error_of(many.c_str()), "a shortcut has at most 48 steps in all");
    // Accepted spellings.
    Shortcut sc;
    std::string error;
    CHECK(parse(R"({"name":"Spellings","triggers":[{"on":"time","at":"7:30 pm","days":"weekdays"},{"on":"tile"},{"on":"tile"}],
        "steps":[{"if":{"all":[{"check":"wifi"}]},"then":[{"do":"say","text":"hi"}]},{"repeat":2,"steps":[{"do":"wait","minutes":1}]},
        {"do":"add","var":"n"}]})", sc, error));
    CHECK(sc.triggers.size() == 2);  // the second tile is the same as the first
    CHECK(sc.triggers[0].at == 19 * 60 + 30 && sc.triggers[0].days == 0x3E);
    CHECK(sc.steps[0].kind == Do::If && sc.steps[0].body[0].kind == Do::Message);
    CHECK(sc.steps[1].kind == Do::Repeat && sc.steps[1].n == 2 && sc.steps[1].body[0].n == 60000);
    CHECK(sc.steps[2].kind == Do::Add && sc.steps[2].n == 1);
    CHECK(sc.automatic() && sc.tile());
    // No triggers at all: run by hand (Swifty, the editor, another shortcut).
    CHECK(parse(R"({"name":"Helper","triggers":[],"steps":[{"do":"stop"}]})", sc, error));
    CHECK(!sc.tile() && !sc.automatic());
}

// ---- the interpreter ---------------------------------------------------------------------
void test_conditions()
{
    CHECK(in_window(23 * 60, 22 * 60, 6 * 60));
    CHECK(in_window(5 * 60 + 59, 22 * 60, 6 * 60));
    CHECK(!in_window(12 * 60, 22 * 60, 6 * 60));
    CHECK(in_window(6 * 60, 6 * 60, 6 * 60));
    CHECK(in_window(8 * 60, 7 * 60, 9 * 60));
    CHECK(!in_window(9 * 60 + 1, 7 * 60, 9 * 60));

    Shortcut sc;
    std::string error;
    CHECK(parse(R"({"name":"Night","steps":[
        {"do":"if","if":{"and":[{"check":"time","from":"22:00","to":"06:00"},{"not":{"check":"day","days":"weekend"}}]},
         "then":[{"do":"home","verb":"lights_off","target":"all"}],
         "elif":[{"if":{"or":[{"check":"home","device":"Desk lamp","is":"on"},{"check":"motion"}]},"then":[{"do":"ir","key":"power"}]}],
         "else":[{"do":"message","text":"nothing"}]}]})", sc, error));
    CHECK_EQ(error, "");
    {
        FakeEnv env;
        env.local = at(3, 23, 30);  // Wednesday night
        const RunResult r = run(sc, env, false);
        CHECK(r.ok);
        CHECK(env.done.size() == 1 && env.done[0] == "lights_off all");
        CHECK_EQ(r.log[0], "1 If it is 22:00\xE2\x80\x93" "06:00 and not it is the weekend: yes");
    }
    {
        FakeEnv env;
        env.local = at(6, 23, 30);  // Saturday night: the elif
        env.readings["home:Desk lamp"] = false;
        env.readings["motion"] = true;
        const RunResult r = run(sc, env, false);
        CHECK(r.ok);
        CHECK(env.done.size() == 1 && env.done[0] == "TV remote: power");
    }
    {
        FakeEnv env;
        env.local = at(2, 12, 0);  // noon: the else
        env.readings["home:Desk lamp"] = false;
        env.readings["motion"] = false;
        const RunResult r = run(sc, env, false);
        CHECK(r.ok && env.done.empty());
        CHECK(env.messages.size() == 1 && env.messages[0] == "nothing");
    }
    {
        FakeEnv env;  // no reading: the run fails, saying why
        env.local = at(2, 12, 0);
        const RunResult r = run(sc, env, false);
        CHECK(!r.ok);
        CHECK_EQ(r.message, "Step 1: could not check Desk lamp is on or there is motion: no reading for home:Desk lamp");
    }
    {
        FakeEnv env;  // the clock is not set
        env.clock_set = false;
        const RunResult r = run(sc, env, false);
        CHECK(!r.ok);
        CHECK(r.message.find("clock is not set") != std::string::npos);
    }
}

void test_loops_and_variables()
{
    Shortcut sc;
    std::string error;
    CHECK(parse(R"({"name":"Count","steps":[
        {"do":"set","var":"n","value":0},
        {"do":"repeat","times":3,"steps":[{"do":"add","var":"n","value":2},{"do":"wait","seconds":1}]},
        {"do":"if","if":{"check":"var","var":"n","equals":6},"then":[{"do":"message","text":"n is {n}, m is {m}, {not a var}"}]},
        {"do":"repeat","while":{"check":"var","var":"n","above":0},"max":50,"steps":[{"do":"add","var":"n","value":-4}]},
        {"do":"message","text":"left {n}"}]})", sc, error));
    CHECK_EQ(error, "");
    FakeEnv env;
    RunResult r = run(sc, env, false);
    CHECK(r.ok);
    CHECK(env.clock == 3000);
    CHECK(env.messages.size() == 2);
    CHECK_EQ(env.messages[0], "n is 6, m is 0, {not a var}");
    CHECK_EQ(env.messages[1], "left -2");  // 6, 2, -2

    // A while loop that would never end stops at its max rounds, without failing.
    CHECK(parse(R"({"name":"Wait for motion","steps":[
        {"do":"repeat","while":{"not":{"check":"motion"}},"max":5,"steps":[{"do":"wait","seconds":10}]},
        {"do":"home","verb":"lights_on","target":"Hall"}]})", sc, error));
    FakeEnv quiet;
    quiet.readings["motion"] = false;
    r = run(sc, quiet, false);
    CHECK(r.ok);
    CHECK(quiet.clock == 50000);
    CHECK(quiet.done.size() == 1);
    // ... and ends early once its condition changes.
    FakeEnv lively;
    lively.readings["motion"] = false;
    lively.on_sleep = [&] {
        if (lively.clock >= 20000) {
            lively.readings["motion"] = true;
        }
    };
    r = run(sc, lively, false);
    CHECK(r.ok && lively.clock == 20000);

    // A dry run waits for nothing and sends nothing, but checks everything.
    FakeEnv dry;
    dry.readings["motion"] = false;
    r = run(sc, dry, true);
    CHECK(r.ok);
    CHECK(dry.clock == 0);
    CHECK(dry.done.size() == 1 && dry.done[0] == "dry lights_on Hall");
    CHECK(r.message.rfind("Test: nothing was sent", 0) == 0);

    // Stop ends the run, as a success.
    CHECK(parse(R"({"name":"Stopper","steps":[{"do":"ir","key":"mute"},{"do":"stop"},{"do":"ir","key":"power"}]})", sc, error));
    FakeEnv s;
    r = run(sc, s, false);
    CHECK(r.ok && s.done.size() == 1);
    CHECK_EQ(r.message, "Stopped at step 2");

    // Too many variables.
    CHECK(parse(R"({"name":"Vars","steps":[{"do":"set","var":"a","value":1},{"do":"set","var":"b","value":1},
        {"do":"set","var":"c","value":1},{"do":"set","var":"d","value":1},{"do":"set","var":"e","value":1},
        {"do":"set","var":"f","value":1},{"do":"set","var":"g","value":1},{"do":"set","var":"h","value":1},
        {"do":"set","var":"i","value":1}]})", sc, error));
    FakeEnv v;
    r = run(sc, v, false);
    CHECK(!r.ok);
    CHECK_EQ(r.message, "Step 9: a run has at most 8 variables.");

    // A failing action stops the run there.
    CHECK(parse(R"({"name":"Fails","steps":[{"do":"ir","key":"mute"},{"do":"repeat","times":2,"steps":[
        {"do":"home","verb":"tv_on"}]},{"do":"ir","key":"power"}]})", sc, error));
    FakeEnv f;
    f.fail_when = [](const Action & a) {
        return a.kind == Do::Home;
    };
    r = run(sc, f, false);
    CHECK(!r.ok);
    CHECK_EQ(r.message, "Step 2.1: No answer from Swifty Home.");
    CHECK(f.done.size() == 1);
}

void test_limits()
{
    Shortcut sc;
    std::string error;
    // Nested repeats that would run 100 x 100 x 100 steps: the step budget ends it.
    CHECK(parse(R"({"name":"Spin","steps":[{"do":"repeat","times":100,"steps":[{"do":"repeat","times":100,"steps":[
        {"do":"repeat","times":100,"steps":[{"do":"add","var":"n"}]}]}]}]})", sc, error));
    FakeEnv env;
    RunResult r = run(sc, env, false);
    CHECK(!r.ok);
    CHECK(r.message.find("too many") != std::string::npos);
    CHECK(r.log.size() <= MAX_LOG);
    CHECK(r.log.back().find("more") != std::string::npos);

    // Loop rounds have their own budget.
    Limits tight;
    tight.rounds = 10;
    CHECK(parse(R"({"name":"Rounds","steps":[{"do":"repeat","times":20,"steps":[{"do":"wait","seconds":1}]}]})", sc, error));
    FakeEnv e2;
    r = run(sc, e2, false, tight);
    CHECK(!r.ok);
    CHECK_EQ(r.message, "Step 1: too many loop rounds (10).");

    // Run time: waits count, dry runs too.
    CHECK(parse(R"({"name":"Long","steps":[{"do":"repeat","times":3,"steps":[{"do":"wait","minutes":60}]}]})", sc, error));
    FakeEnv e3;
    r = run(sc, e3, true);
    CHECK(!r.ok);
    CHECK_EQ(r.message, "Step 1.1: this wait would pass the run's limit of 120 min.");
    CHECK(e3.clock == 0);

    // Running other shortcuts: found by id, never itself, at most three deep.
    auto make = [&](uint32_t id, const std::string &steps) {
        auto p = std::make_shared<Shortcut>();
        std::string e;
        parse(("{\"name\":\"S" + std::to_string(id) + "\",\"steps\":" + steps + "}").c_str(), *p, e);
        p->id = id;
        return p;
    };
    FakeEnv calls;
    calls.others[2] = make(2, R"([{"do":"ir","key":"ok"},{"do":"stop"},{"do":"ir","key":"back"}])");
    calls.others[3] = make(3, R"([{"do":"run","id":4}])");
    calls.others[4] = make(4, R"([{"do":"run","id":3}])");
    calls.others[5] = make(5, R"([{"do":"run","id":6}])");
    calls.others[6] = make(6, R"([{"do":"run","id":7}])");
    calls.others[7] = make(7, R"([{"do":"run","id":8}])");
    calls.others[8] = make(8, R"([{"do":"ir","key":"up"}])");
    auto top = make(1, R"([{"do":"run","id":2},{"do":"ir","key":"home"}])");
    r = run(*top, calls, false);
    CHECK(r.ok);  // 2's stop ends 2, not 1
    CHECK(calls.done.size() == 2 && calls.done[1] == "TV remote: home");
    r = run(*make(1, R"([{"do":"run","id":3}])"), calls, false);
    CHECK(!r.ok);
    CHECK_EQ(r.message, "Step 1 (S3): Step 1>1 (S4): Step 1>1>1: \"S3\" is already running here (a shortcut cannot run itself).");
    r = run(*make(1, R"([{"do":"run","id":5}])"), calls, false);
    CHECK(!r.ok);
    CHECK(r.message.find("at most 3 deep") != std::string::npos);
    r = run(*make(1, R"([{"do":"run","id":99,"shortcut":"Gone"}])"), calls, false);
    CHECK_EQ(r.message, "Step 1: there is no shortcut \"Gone\" any more.");
}

// ---- words -------------------------------------------------------------------------------
void test_words()
{
    Shortcut sc;
    std::string error;
    CHECK(parse(R"({"name":"Evening","triggers":[{"on":"tile"},{"on":"time","at":"19:30","days":["mon","fri"]},
        {"on":"every","minutes":15,"from":"08:00","to":"20:00"},{"on":"sun","event":"sunset","offset":-15},
        {"on":"home","device":"Desk lamp","to":"on"},{"on":"motion","cooldown":120}],
        "steps":[{"do":"if","if":{"check":"battery","below":20},"then":[{"do":"message","text":"Low","sound":true}],
        "else":[{"do":"repeat","times":2,"steps":[{"do":"wait","seconds":90}]}]},{"do":"run","id":4,"shortcut":"Movie"}]})", sc, error));
    CHECK_EQ(error, "");
    CHECK_EQ(trigger_text(sc.triggers[1]), "At 19:30 on Mon, Fri");
    CHECK_EQ(trigger_text(sc.triggers[2]), "Every 15 min, 08:00\xE2\x80\x93" "20:00");
    CHECK_EQ(trigger_text(sc.triggers[3]), "15 min before sunset");
    CHECK_EQ(trigger_text(sc.triggers[4]), "When Desk lamp turns on");
    CHECK_EQ(trigger_text(sc.triggers[5]), "On motion (at most every 2 min)");
    std::vector<std::string> lines;
    steps_lines(sc.steps, lines);
    CHECK(lines.size() == 6);
    CHECK_EQ(lines[0], "If battery is below 20%:");
    CHECK_EQ(lines[1], "   Show \xE2\x80\x9CLow\xE2\x80\x9D with a chime");
    CHECK_EQ(lines[2], "Otherwise:");
    CHECK_EQ(lines[3], "   Repeat 2 times:");
    CHECK_EQ(lines[4], "      Wait 90 s");
    CHECK_EQ(lines[5], "Run Movie");
    CHECK_EQ(summary_text(sc), "At 19:30 on Mon, Fri +4 \xC2\xB7 5 steps");
    // The JSON goes round unchanged.
    Shortcut again;
    const std::string json = to_json(sc);
    CHECK(parse(json.c_str(), again, error));
    CHECK_EQ(to_json(again), json);
}

// ---- schedules ---------------------------------------------------------------------------
time_t local_time(int y, int mo, int d, int h, int mi)
{
    struct tm t = {};
    t.tm_year = y - 1900, t.tm_mon = mo - 1, t.tm_mday = d, t.tm_hour = h, t.tm_min = mi, t.tm_isdst = -1;
    return mktime(&t);
}

void test_schedules()
{
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);  // the clock's UK zone
    tzset();
    Shortcut sc;
    std::string error;
    CHECK(parse(R"({"name":"S","triggers":[{"on":"time","at":"07:00","days":"weekdays"},
        {"on":"every","minutes":20,"from":"22:00","to":"01:00"},{"on":"every","minutes":45},
        {"on":"sun","event":"sunrise"},{"on":"sun","event":"sunset","offset":30,"days":["sat"]}],"steps":[{"do":"stop"}]})", sc, error));
    Location loc;
    const Trigger &morning = sc.triggers[0], &night = sc.triggers[1], &often = sc.triggers[2], &rise = sc.triggers[3],
                   &set = sc.triggers[4];
    auto fires = [&](const Trigger & t, time_t when) {
        struct tm l;
        localtime_r(&when, &l);
        return fires_at(t, l, when - when % 60, loc);
    };
    // 2026-11-27 is a Friday, 2026-11-28 a Saturday.
    CHECK(fires(morning, local_time(2026, 11, 27, 7, 0)));
    CHECK(!fires(morning, local_time(2026, 11, 27, 7, 1)));
    CHECK(!fires(morning, local_time(2026, 11, 28, 7, 0)));
    CHECK(fires(night, local_time(2026, 11, 27, 22, 0)));
    CHECK(fires(night, local_time(2026, 11, 27, 23, 40)));
    CHECK(fires(night, local_time(2026, 11, 28, 0, 20)));   // past midnight, counted from 22:00
    CHECK(fires(night, local_time(2026, 11, 28, 1, 0)));
    CHECK(!fires(night, local_time(2026, 11, 28, 1, 20)));
    CHECK(!fires(night, local_time(2026, 11, 27, 22, 10)));
    CHECK(fires(often, local_time(2026, 11, 27, 0, 45)));
    CHECK(!fires(often, local_time(2026, 11, 27, 1, 0)));
    // Next times, across a weekend.
    CHECK(next_fire(morning, local_time(2026, 11, 27, 7, 0), loc) == local_time(2026, 11, 30, 7, 0));
    CHECK(next_fire(night, local_time(2026, 11, 28, 0, 30), loc) == local_time(2026, 11, 28, 0, 40));
    CHECK(next_fire(night, local_time(2026, 11, 28, 1, 0), loc) == local_time(2026, 11, 28, 22, 0));
    CHECK(next_fire(often, local_time(2026, 11, 27, 23, 50), loc) == local_time(2026, 11, 28, 0, 0));
    // Across the change to summer time (29 March 2026, 01:00 GMT -> 02:00 BST).
    CHECK(next_fire(morning, local_time(2026, 3, 27, 8, 0), loc) == local_time(2026, 3, 30, 7, 0));

    // The sun, without a location: never.
    CHECK(next_fire(rise, local_time(2026, 6, 21, 0, 0), loc) == 0);
    // London (51.51 N, 0.13 W) on 21 June 2026: sunrise about 04:43 BST, sunset about 21:21
    // (NOAA's figures).
    loc.set = true, loc.lat = 51.51f, loc.lon = -0.13f;
    const time_t sunrise = next_fire(rise, local_time(2026, 6, 21, 0, 0), loc);
    const time_t want = local_time(2026, 6, 21, 4, 43);
    CHECK(sunrise >= want - 4 * 60 && sunrise <= want + 4 * 60);
    CHECK(fires(rise, sunrise));
    CHECK(!fires(rise, sunrise + 60));
    // Sunset + 30 min on Saturdays only: 27 June 2026 is a Saturday.
    const time_t evening = next_fire(set, local_time(2026, 6, 22, 0, 0), loc);
    const time_t want2 = local_time(2026, 6, 27, 21, 51);
    CHECK(evening >= want2 - 4 * 60 && evening <= want2 + 4 * 60);
    // Far north in midwinter: no sunrise for days.
    loc.lat = 78.2f, loc.lon = 15.6f;
    CHECK(next_fire(rise, local_time(2026, 12, 21, 0, 0), loc) == 0);
    double minutes = 0;
    CHECK(!sun_utc_minutes(2026, 12, 21, 78.2f, 15.6f, true, minutes));
    // London at the March equinox: sunrise about 06:03 UTC.
    CHECK(sun_utc_minutes(2026, 3, 20, 51.5074f, -0.1278f, true, minutes));
    CHECK(minutes > 6 * 60 - 4 && minutes < 6 * 60 + 8);
    CHECK(days_from_civil(1970, 1, 1) == 0);
    CHECK(days_from_civil(2026, 11, 28) == 20785);
}
}  // namespace

int main()
{
    test_migration();
    test_errors();
    test_conditions();
    test_loops_and_variables();
    test_limits();
    test_words();
    test_schedules();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
