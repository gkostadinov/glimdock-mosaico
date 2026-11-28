// SPDX-License-Identifier: Apache-2.0
// Swifty Home wall display (a self-hosted Swifty Home hub) for the ESP-Mosaico shell.
// All calls run on the LVGL task (or with its lock held).
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the hub client in the background (no UI is built until opened).
void home_app_start(lv_obj_t *frame);
// First call builds the UI inside frame (the 320-unit-wide layout is scaled to
// the frame's width; a taller frame shows more of each page) and starts the hub client.
void home_app_open(lv_obj_t *frame);
// While inactive the UI is not rebuilt; polling continues at a slower pace.
void home_app_set_active(bool active);
// What the launcher shows on the Home page.
typedef struct {
    bool started, paired, connected;
    char title[80], hint[80];
    bool lights;
    int lights_pct, lights_on, lights_count, lights_hue;
    char lights_state[40];
    bool music;
    int volume;
    char music_state[40];
    bool tv, tv_on;
    char tv_name[28], tv_state[40];
    struct {
        char name[28];
        bool on, known;
    } plugs[2];
    int plug_count;
} home_summary_t;
void home_app_summary(home_summary_t *out);

// For Swifty (call with the display lock held): the home as text, and a
// command. verb: lights_on, lights_off, brightness (value 0-100), color (value
// a hue 0-359 or a colour name), scene (value: one of the hub's scenes, by
// name or id), volume (0-100), play, pause, mute, unmute, tv_on, tv_off,
// plug_on, plug_off (or switch_on, switch_off: a switch is the hub's "plug"),
// ac_on, ac_off, ac_mode (value: a mode the unit lists, such as cool),
// ac_temp (value in degrees Celsius), ac_fan (value: a fan speed it lists),
// feed (value: grams, the nearest portion the feeder offers) and clean.
// target: a room, light, switch, air conditioner or "all" (may be empty; an
// air conditioner or a switch may be left out when there is one); exact
// names win over near ones. Writes what was sent or why not. The hub decides
// what a display may run: switches and the air conditioner are the owner's
// unless the hub shares them with family.
void home_app_describe(char *out, size_t capacity);
void home_app_command(const char *verb, const char *target, const char *value, char *out, size_t capacity);

// Settings offers "Exit to Mosaico launcher"; the shell provides the action.
void home_app_set_exit_handler(void (*exit_to_launcher)(void));
// Read-only for the profile signed in (the shell's profiles): pages show,
// lights, music, scenes and unpairing are refused with a note.
void home_app_set_read_only(bool read_only);

// Pairs with a Swifty Home hub using a one-time display code, as the Pair
// page does (LVGL task; Home must be started). The outcome shows in
// home_app_pairing_info: "pairing", "paired as NAME" or "failed: WHY".
void home_app_pair(const char *hub_url, const char *code);
typedef struct {
    bool paired;
    char hub[160], name[64], note[128];
} home_pairing_info_t;
void home_app_pairing_info(home_pairing_info_t *out);

#ifdef __cplusplus
}

#include <functional>
#include <string>

// For shortcuts (LVGL task, or with the display lock held). Verbs as
// home_app_command's, plus lights_toggle, plug_toggle (switch_toggle),
// tv_toggle and ac_toggle (off when on, else on). A target may also be a room
// id or a device target.
//
// Home is started, paired and has the hub's lists (rooms, devices, actions).
bool home_app_ready();
// Checks a command without sending it: what it would do ("Bedroom lights
// off") or why not, and the target as the hub knows it (a room id, a device
// target, "all" or a scene id).
bool home_app_resolve(const char *verb, const char *target, const char *value, std::string &what, std::string &resolved);
// Sends a command and calls done once, on the LVGL task, when the hub reports
// how it ended (a failure names the part that did not answer), or after 15 s.
// Actions that ask first (the TV's power, Movie, Feed, Clean cycle) are
// confirmed: the shortcut was the tap. dry_run: resolved and logged, nothing
// sent. False: it could not start (what says why; done is not called). what:
// what it does.
using HomeRunDone = std::function<void(bool ok, const std::string &message)>;
bool home_app_run(const char *verb, const char *target, const char *value, bool dry_run, HomeRunDone done, std::string &what);
// Rooms (with their number of lights), lights, switches, scenes, the TV, the
// speaker, air conditioners and the pets, as JSON for shortcut editors:
// {"paired","ready","rooms":[{"id","name","lights"}],"lights":[{"target",
// "name","color"}],"plugs":[{"target","name"}] (the switches),"scenes":[{"id",
// "name"}],"tv":"name","speaker":"name","acs":[{"target","name","modes":[...],
// "fans":[...],"min","max"}],"feeder":"name","litter":"name"}. Keys without a
// device are left out (acs is always there, maybe empty).
std::string home_app_choices_json();
// For shortcuts' conditions and triggers (LVGL task): each device and its
// reading as JSON, [{"target","name","room","class","known","on","temp"}]:
// "on" for what switches, "temp" for what reports a temperature (an air
// conditioner's indoor reading, a sensor's). "[]" before the hub answers.
std::string home_app_states_json();
// Whether a command asks before it runs (the TV's power, Movie): a run that
// nobody tapped asks on the screen first. False when Home cannot tell.
bool home_app_asks_first(const char *verb, const char *target, const char *value);
#endif
