// SPDX-License-Identifier: Apache-2.0
// Shortcuts: one-tap tiles on the home screen, like Shortcuts on a phone. A
// shortcut is a name, an icon, a colour and up to ten steps run in order:
//   home         a Swifty Home command: lights on, off or toggle (a room, a
//                light or all), brightness, colour, a scene, a plug, the TV,
//                the speaker (the verbs of Swifty's home_control, plus
//                lights_toggle, plug_toggle and tv_toggle)
//   wait         seconds (up to 60)
//   ir           an LG TV key from the Interaction board's infrared LED
//   board_lights the Interaction board's LEDs (mode, colour)
//   brightness   the screen's light
//   open         one of the board's apps (and a Tools tool)
//   flashlight   the camera module's light
// A run shows a toast with how it went (or which step failed) and buzzes.
// They are kept in NVS, saved on the LVGL task. Made and changed in Web
// settings, by Swifty (add_shortcut, list_shortcuts, run_shortcut,
// delete_shortcut) and over ESP-Iris (RPC 55). The first time Swifty Home has
// answered, a few starters are made from its rooms ("All lights off",
// "Living room lights off", ...).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

struct ShortcutHooks {
    void (*home_start)();                                 // Home's hub client running (LVGL task)
    bool (*open_app)(const char *app, const char *tool);  // false: no such app (LVGL task)
    void (*brightness)(int percent);                      // the screen, saved (LVGL task)
    bool (*flashlight)(bool on);                          // false: no camera module (LVGL task)
};

// Loads them, gives Swifty its tools, registers RPC 55 and starts the save
// and tile timer (LVGL task, display lock held).
void shortcuts_start(const ShortcutHooks &hooks);
// Runs one, as a tap on its tile (any task). dry_run: Swifty Home is asked
// nothing and the Interaction board sends nothing; each step is logged. False:
// no such shortcut, or too many runs waiting.
bool shortcuts_run(uint32_t id, bool dry_run = false);
// The board's own editor, over any app: the shortcuts (Run, Test, Delete) and
// New (lights off, on or toggled for all lights or a room, or a scene, in two
// taps). LVGL task.
void shortcuts_editor_open();

// Web settings' API (any task but the LVGL task: these take the display lock).
// The shortcuts and what their steps can use, as JSON.
std::string shortcuts_web_list();
// A change: {"op":"save","shortcut":{...}}, {"op":"delete","id":N},
// {"op":"run","id":N,"dry_run":false}, {"op":"move","id":N,"to":index},
// {"op":"starters"}. The reply is JSON: {"reply":"ok",...} or {"error":"..."};
// ok false for an error.
std::string shortcuts_web_apply(const char *json, size_t length, bool *ok);
