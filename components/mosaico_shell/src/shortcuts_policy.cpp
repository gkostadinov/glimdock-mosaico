// SPDX-License-Identifier: Apache-2.0
// The one place every shortcut action passes just before it runs (see
// shortcuts_action_allowed in shortcuts.h). Kept apart from the engine so
// that permissions (profiles: what a guest or a child may switch) change only
// this file. Called from the shortcuts' run tasks: no LVGL, no long waits.
#include "shortcuts.h"

bool shortcuts_action_allowed(const ShortcutActionInfo &action, std::string *why)
{
    (void)action;
    (void)why;
    return true;
}
