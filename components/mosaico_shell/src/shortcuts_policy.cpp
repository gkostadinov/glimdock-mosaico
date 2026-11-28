// SPDX-License-Identifier: Apache-2.0
// The one place every shortcut action passes just before it runs (see
// shortcuts_action_allowed in shortcuts.h). Kept apart from the engine so
// that permissions (profiles: what a guest or a child may switch) change only
// this file. Called from the shortcuts' run tasks: no LVGL, no long waits.
#include "shortcuts.h"

#include <string.h>

#include "launcher_apps.h"
#include "profiles.h"

namespace {
bool is(const char *a, const char *b)
{
    return a && !strcmp(a, b);
}

bool refuse(std::string *why, const char *text)
{
    if (why) {
        *why = text;
    }
    return false;
}
}  // namespace

bool shortcuts_action_allowed(const ShortcutActionInfo &action, std::string *why)
{
    if (!profiles_configured() || action.dry_run) {
        return true;  // as before profiles; a dry run sends nothing
    }
    // Runs the board starts by itself (a time, a trigger, at boot) were set up
    // by someone allowed to edit shortcuts, and run whoever is signed in; so
    // do runs from a trusted computer (RPC 55).
    const char *s = action.source;
    if (!(is(s, "tile") || is(s, "editor") || is(s, "swifty") || is(s, "web"))) {
        return true;
    }
    if (!profiles_can(PROFILE_CAP_SHORTCUTS_RUN)) {
        return refuse(why, "Running shortcuts is not allowed for this profile");
    }
    const char *k = action.kind;
    if (is(k, "home") && (!profiles_can(PROFILE_CAP_HOME_CONTROL) ||
                          profiles_app_access(ShellApp::Home) != PROFILE_ACCESS_FULL)) {
        return refuse(why, "Controlling the home is not allowed for this profile");
    }
    if ((is(k, "ir") || is(k, "board_lights") || is(k, "flashlight")) && !profiles_can(PROFILE_CAP_BOARD_CONTROL)) {
        return refuse(why, "The board's lights and IR are not allowed for this profile");
    }
    if (is(k, "open")) {
        // The app's key (launcher_apps.cpp), "launcher" always.
        const LauncherApp *app = launcher_app(action.verb ? action.verb : "");
        if (app && app->open >= 0 && profiles_app_access(app->open) == PROFILE_ACCESS_HIDDEN) {
            return refuse(why, "That app is not available for this profile");
        }
    }
    return true;  // the screen's brightness, and anything newer
}
