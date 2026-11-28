// SPDX-License-Identifier: Apache-2.0
// Host preview: a stand-in for the shortcuts' engine (shortcuts.cpp), so the
// preview draws the real screens (shortcuts_ui.cpp) over demo shortcuts: a
// tile, an evening automation with an if and a repeat, a motion automation
// that is off, and a helper run by hand. Runs only change what is shown.
#include <string.h>

#include <algorithm>
#include <map>

#include "shortcuts_internal.h"

namespace shortcuts {
namespace {
std::vector<std::shared_ptr<const Shortcut>> demo;
std::map<uint32_t, RunStatus> runs;
uint32_t rev = 1;

const char *const DEMO[] = {
    R"({"id":1,"name":"All lights off","icon":"power","accent":"violet","triggers":[{"on":"tile"}],
        "steps":[{"do":"home","verb":"lights_off","target":"all"}]})",
    R"({"id":2,"name":"Evening","icon":"bulb","accent":"amber",
        "triggers":[{"on":"tile"},{"on":"sun","event":"sunset","offset":-15},{"on":"time","at":"19:30","days":"weekdays"}],
        "steps":[{"do":"if","if":{"and":[{"check":"home","device":"Desk lamp","is":"off"},{"not":{"check":"day","days":["sun"]}}]},
          "then":[{"do":"home","verb":"lights_on","target":"room-living"},{"do":"wait","seconds":2},
                  {"do":"home","verb":"brightness","target":"room-living","value":"60"}],
          "else":[{"do":"message","text":"The lamp is on already"}]},
          {"do":"repeat","times":3,"steps":[{"do":"board_lights","mode":"colour","colour":"warm"},{"do":"wait","seconds":1}]}]})",
    R"({"id":3,"name":"Hall motion","icon":"person","accent":"green","enabled":false,"triggers":[{"on":"motion","cooldown":120}],
        "steps":[{"do":"if","if":{"check":"time","from":"22:00","to":"06:00"},"then":[{"do":"home","verb":"lights_on","target":"Hall"},
          {"do":"wait","minutes":2},{"do":"home","verb":"lights_off","target":"Hall"}]}]})",
    R"({"id":4,"name":"Movie","icon":"tv","accent":"cyan","triggers":[{"on":"tile"}],
        "steps":[{"do":"home","verb":"scene","value":"movie"},{"do":"ir","key":"input"},{"do":"brightness","percent":30}]})",
};

void load_demo()
{
    if (!demo.empty()) {
        return;
    }
    for (const char *json : DEMO) {
        JsonDocument doc;
        deserializeJson(doc, json, DeserializationOption::NestingLimit(24));
        auto sc = std::make_shared<Shortcut>();
        std::string error;
        if (parse_shortcut(doc.as<JsonObjectConst>(), *sc, error)) {
            demo.push_back(sc);
        }
    }
    RunStatus &evening = runs[2];
    evening.ran = true;
    evening.ok = true;
    evening.source = Source::Sun;
    evening.message = "Done, 9 steps";
    evening.log = {"1 If Desk lamp is off and not it is Sun: yes", "1.1 Living room lights on: ok", "1.2 Wait 2 s",
                   "1.3 Living room brightness 60%: ok", "2 Round 1 of 3", "2.1 Interaction lights: colour warm: ok",
                   "2.2 Wait 1 s", "2 Round 2 of 3", "2.1 Interaction lights: colour warm: ok", "2.2 Wait 1 s"
                  };
    RunStatus &movie = runs[4];
    movie.ran = true;
    movie.source = Source::Tile;
    movie.message = "Step 2: No Interaction board found: it plugs into either side of the board.";
    movie.log = {"1 Scene Movie: ok", "2 failed: No Interaction board found"};
}

template <typename F> bool change(uint32_t id, F f)
{
    for (auto &p : demo)
        if (p->id == id) {
            auto copy = std::make_shared<Shortcut>(*p);
            f(*copy);
            p = copy;
            ++rev;
            return true;
        }
    return false;
}
}  // namespace

std::vector<std::shared_ptr<const Shortcut>> list_copy()
{
    load_demo();
    return demo;
}

std::shared_ptr<const Shortcut> find(uint32_t id)
{
    load_demo();
    for (const auto &p : demo)
        if (p->id == id) {
            return p;
        }
    return nullptr;
}

RunStatus status_of(uint32_t id)
{
    load_demo();
    return runs.count(id) ? runs[id] : RunStatus();
}

bool start(uint32_t id, bool dry, Source source)
{
    RunStatus &st = runs[id];
    st.running = true;
    st.dry = dry;
    st.source = source;
    ++rev;
    return true;
}

void stop(uint32_t id)
{
    runs[id].running = false;
    ++rev;
}

bool set_enabled(uint32_t id, bool on, std::string &)
{
    return change(id, [on](Shortcut & sc) {
        sc.enabled = on;
    });
}

bool set_tile(uint32_t id, bool on, std::string &)
{
    return change(id, [on](Shortcut & sc) {
        sc.triggers.erase(std::remove_if(sc.triggers.begin(), sc.triggers.end(), [](const Trigger & t) {
            return t.kind == On::Tile;
        }), sc.triggers.end());
        if (on) {
            sc.triggers.insert(sc.triggers.begin(), Trigger());
        }
    });
}

bool erase(uint32_t id, std::string &name)
{
    for (auto it = demo.begin(); it != demo.end(); ++it)
        if ((*it)->id == id) {
            name = (*it)->name;
            demo.erase(it);
            ++rev;
            return true;
        }
    return false;
}

bool add_made(Shortcut sc, std::string &)
{
    sc.id = 10 + uint32_t(demo.size());
    demo.push_back(std::make_shared<Shortcut>(sc));
    ++rev;
    return true;
}

bool name_taken(const std::string &name)
{
    for (const auto &p : demo)
        if (!strcasecmp(p->name.c_str(), name.c_str())) {
            return true;
        }
    return false;
}

// Home's words for the demo's Home actions.
std::string action_words(const Action &a)
{
    if (a.kind != Do::Home) {
        return action_text(a);
    }
    const std::string where = a.target == "room-living" ? "Living room" : a.target == "all" ? "All" : a.target;
    if (a.verb == "lights_on" || a.verb == "lights_off") {
        return where + " lights " + (a.verb == "lights_on" ? "on" : "off");
    }
    if (a.verb == "brightness") {
        return where + " brightness " + a.value + "%";
    }
    if (a.verb == "scene") {
        return "Scene " + std::string(a.value == "movie" ? "Movie" : a.value);
    }
    return action_text(a);
}

std::string home_choices()
{
    return R"({"paired":true,"ready":true,"rooms":[{"id":"room-living","name":"Living room","lights":3},
        {"id":"room-bed","name":"Bedroom","lights":2},{"id":"room-hall","name":"Hall","lights":1}],
        "lights":[{"target":"hue/1","name":"Floor lamp"}],"scenes":[{"id":"evening","name":"Evening"},{"id":"movie","name":"Movie"}]})";
}

std::string next_text(const Shortcut &sc)
{
    return sc.automatic() && sc.id == 2 ? "Fri 16:42" : "";
}

uint32_t revision()
{
    return rev;
}

void board_wake() {}
void board_buzz() {}
void board_chime() {}

}  // namespace shortcuts
