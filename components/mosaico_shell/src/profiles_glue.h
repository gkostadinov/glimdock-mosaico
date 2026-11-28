// SPDX-License-Identifier: Apache-2.0
// The shell's side of profiles (profiles.h) on the board: face sign-in's
// hooks, the screens' background work, the auto-lock, Swifty's policy (which
// tools the person signed in may use), RPC 54 · 60 and 54 · 61 and the JSON
// Web settings uses.
#pragma once

#include <stddef.h>

#include <string>

// After the launcher is built (LVGL task, display lock held). apply runs on
// the LVGL task after every change of who is signed in or what they may do.
void profiles_glue_start(void (*apply)());
// The launcher's visible hook: an app's key or a shortcut tile's ("sc:...").
bool profiles_key_visible(const char *key);
// Swifty's policy (swifty_policy_t): the tool's name; a line for its prompt.
bool profiles_tool_allowed(const char *tool);
void profiles_describe(char *out, size_t capacity);
// Web settings (any task but the LVGL task: changes take the display lock).
// The profiles as JSON (never a PIN or its hash), and a change in the JSON of
// RPC 54 · 60 with the rights of whoever is signed in on the board.
std::string profiles_web_list();
std::string profiles_web_apply(const char *json, size_t length, bool *ok);
