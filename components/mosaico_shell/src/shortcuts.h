// SPDX-License-Identifier: Apache-2.0
// Shortcuts: small programs, like Shortcuts on a phone. Each has a name, an
// icon, a colour, an on/off switch, triggers and steps:
//
// Triggers ("on"), any of: tile (a tile on the home screen to tap), time
// {at "07:00", days}, every {minutes, from, to, days}, boot {delay s},
// motion {cooldown s} and button {button left|right|any} on the Interaction
// board, home {device, to on|off|any, or above/below a temperature}, sun
// {event sunrise|sunset, offset min, days; needs the location set in Web
// settings}. days: ["mon","fri"], "weekdays" or "weekend" (absent: every day).
// Off, nothing but a tap (or Swifty, or the editor) starts it. Schedules
// missed while the board was off are not made up afterwards.
//
// Steps ("do"), run in order:
//   home         a Swifty Home command: verb lights_on, lights_off,
//                lights_toggle, brightness, color, scene, plug_on, plug_off,
//                plug_toggle, tv_on, tv_off, tv_toggle, volume, play, pause,
//                mute or unmute; target a room, light or plug, or all; value
//   ir           key: an LG TV key from the Interaction board's infrared LED
//   board_lights mode off|react|rainbow|night|colour, colour
//   brightness   percent: the screen's light
//   open         app (and tool, for Tools)
//   flashlight   on: the camera module's light
//   wait         seconds (or minutes), up to an hour
//   if           if COND, then [...], elif [{if, then}], else [...]
//   repeat       times N (up to 100) or while COND (max rounds, up to 100), steps [...]
//   set, add     var, value: whole numbers, for conditions and messages ({var})
//   message      text, haptic, sound: a card on the screen
//   run          shortcut: another shortcut (by name or id; never itself)
//   stop         ends this shortcut
// COND ("check"): time {from, to}, day {days}, home {device, is on|off, or
// above/below}, motion {within s}, button {button}, light {above/below %},
// wifi, vpn, battery {above/below %}, charging, var {var, equals/above/
// below}; or {"and": [...]}, {"or": [...]}, {"not": {...}}.
//
// Limits: 24 shortcuts, 48 steps each in all, ifs and repeats four deep; a
// run stops after 500 steps, 1000 loop rounds or two hours, and runs other
// shortcuts at most three deep. Three run at once; a shortcut that is
// already running is not started again by its triggers.
//
// A run shows a toast with how it went (or which step failed) and buzzes; a
// run nobody tapped stays quiet unless it fails or shows a message, and asks
// on the screen before a Home command that asks first (the TV's power). Kept
// in NVS (format 2: shortcuts_program.h; format 1 is read and moved on),
// saved on the LVGL task. Made and changed in Web settings, on the board (the
// editor), by Swifty (add_shortcut and its other tools; automatic triggers
// need a tap on the board to allow them) and over ESP-Iris (RPC 55).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

struct ShortcutHooks {
    void (*home_start)();                                 // Home's hub client running (LVGL task)
    bool (*open_app)(const char *app, const char *tool);  // false: no such app (LVGL task)
    void (*brightness)(int percent);                      // the screen, saved (LVGL task)
    bool (*flashlight)(bool on);                          // false: no camera module (LVGL task)
    int (*battery)(bool *charging);                       // percent, -1 unknown (LVGL task; optional)
};

// Loads them, gives Swifty its tools, registers RPC 55 and starts the save,
// tile and trigger timer (LVGL task, display lock held).
void shortcuts_start(const ShortcutHooks &hooks);
// Runs one, as a tap on its tile (any task). dry_run: Swifty Home is asked
// nothing, the Interaction board sends nothing and waits are skipped; each
// step is logged. False: no such shortcut, it is running, or too many runs wait.
bool shortcuts_run(uint32_t id, bool dry_run = false);
// The board's own editor, over any app: the shortcuts, each one's triggers,
// steps and last run (Run, Test, on/off, its tile, Delete), and New (lights
// off, on or toggled for all lights or a room, or a scene, in two taps).
// LVGL task.
void shortcuts_editor_open();
// The AI key: a shortcut's page goes back to the list, the list closes. False
// when the editor isn't open. LVGL task.
bool shortcuts_editor_back();

// Web settings' API (any task but the LVGL task: these take the display lock).
// The shortcuts (with each one's last run), the location and what their
// steps can use, as JSON.
std::string shortcuts_web_list();
// A change: {"op":"save","shortcut":{...}}, {"op":"delete","id":N},
// {"op":"run","id":N,"dry_run":false}, {"op":"test","shortcut":{...}} (a dry
// run of an unsaved one, waited for: its log), {"op":"enable","id":N,
// "on":true}, {"op":"move","id":N,"to":index}, {"op":"location","lat":L,
// "lon":L} (lat null: none), {"op":"starters"}. The reply is JSON:
// {"reply":"ok",...} or {"error":"..."}; ok false for an error.
std::string shortcuts_web_apply(const char *json, size_t length, bool *ok);

// Where profiles decide (any task): every action, just before it runs (dry
// runs too). kind: home, ir, board_lights, brightness, open or flashlight;
// verb, target and value as in the step; source: what started the run (tile,
// editor, swifty, web, rpc, test, time, every, boot, motion, button, home,
// sun). False refuses it, with why (shown in the run's log) or the default
// "not allowed". shortcuts_policy.cpp; it allows everything for now.
struct ShortcutActionInfo {
    const char *kind, *verb, *target, *value;
    int number;  // brightness percent, flashlight 1 or 0
    const char *source;
    const char *shortcut;  // its name
    bool dry_run;
};
bool shortcuts_action_allowed(const ShortcutActionInfo &action, std::string *why);
