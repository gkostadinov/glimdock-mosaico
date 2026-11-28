// SPDX-License-Identifier: Apache-2.0
// Between the shortcuts' engine (shortcuts.cpp: the list, runs, triggers,
// Swifty, Web settings, RPC 55) and its screens (shortcuts_ui.cpp: the
// editor, toasts, message cards and the cards that ask). The host preview
// draws the screens over a stand-in engine (host/shortcuts_host.cpp).
#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "shortcuts_program.h"

namespace shortcuts {

// How a shortcut's last run went (kept in memory until a restart).
struct RunStatus {
    bool running = false;
    bool ran = false;  // since start-up
    bool ok = false, dry = false;
    Source source = Source::Tile;
    time_t at = 0;  // when it ended (0: the clock was not set)
    std::string message;
    std::vector<std::string> log;
};

// ---- the engine, for the screens (LVGL task) ---------------------------------------------
std::vector<std::shared_ptr<const Shortcut>> list_copy();
std::shared_ptr<const Shortcut> find(uint32_t id);
RunStatus status_of(uint32_t id);
bool start(uint32_t id, bool dry, Source source);
void stop(uint32_t id);
bool set_enabled(uint32_t id, bool on, std::string &error);
bool set_tile(uint32_t id, bool on, std::string &error);
bool erase(uint32_t id, std::string &name);
// Adds one made here (its Home targets came from Home: nothing to resolve).
bool add_made(Shortcut sc, std::string &error);
bool name_taken(const std::string &name);
// A Home action in Home's words ("Bedroom lights off"), else action_text.
std::string action_words(const Action &a);
// Home's rooms, lights, plugs and scenes (home_app_choices_json).
std::string home_choices();
// The next time something but a tap starts it (0: none), as "Fri 19:30".
std::string next_text(const Shortcut &sc);
uint32_t revision();  // bumped by every change to the list or a run's status
// The board, for the screens: the always-on display gives way, a buzz, the
// reminder chime.
void board_wake();
void board_buzz();
void board_chime();

// ---- the screens, for the engine (LVGL task) ---------------------------------------------
enum class Feedback : uint8_t { Running, Done, Failed };
void ui_toast(const std::string &name, const std::string &glyph, uint8_t accent, Feedback state, const std::string &detail);
void ui_buzz(bool failed);
// A message step: a card over any app with the shortcut's name and the text.
void ui_message(const std::string &title, const std::string &text, uint8_t accent, bool buzz, bool chime);
// A card that asks (Allow, Not now). done(true) on Allow; done(false) on Not
// now, after timeout_s, or when another card that asks replaces it.
void ui_ask(const std::string &title, const std::string &body, const std::string &allow, int timeout_s,
            std::function<void(bool)> done);
void ui_editor_open();
bool ui_editor_back();
void ui_tick();  // once a second: the editor follows changes

}  // namespace shortcuts
