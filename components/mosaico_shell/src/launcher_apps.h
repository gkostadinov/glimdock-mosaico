// SPDX-License-Identifier: Apache-2.0
// The home screen's apps: one entry per app in launcher_apps.cpp. The launcher
// lays them out (launcher_layout.h); shortcut tiles join them at run time
// (launcher_shortcut_set in launcher_ui.h).
#pragma once

#include "mui.h"
#include "shell_internal.h"

// What an app icon opens besides a ShellApp.
constexpr int LAUNCHER_OPEN_SETTINGS = -1;  // the Settings sheet over the home screen

struct LauncherApp {
    // Stable and unique: saved in the layout (NVS), so never rename one. Up to
    // 23 characters, no ',' '|' '#'; "sc:" starts the shortcut tiles' keys.
    const char *key;
    const char *name;           // the label under the icon
    const char *glyph;          // a mui::icon name; nullptr draws the Glimdock mark
    const mui::Accent *accent;  // the icon's colour (its glow fills the tile)
    int open;                   // a ShellApp, or LAUNCHER_OPEN_SETTINGS
    bool keep;                  // can't be hidden (Settings: the way back to hidden apps)
};

// In order: a fresh layout fills the pages in this order, and an app added in
// a later firmware lands at the end of the last page.
extern const LauncherApp LAUNCHER_APPS[];
extern const int LAUNCHER_APP_COUNT;

// The entry for key, or nullptr.
const LauncherApp *launcher_app(const char *key);
// The key of the app that opens app (nullptr for the launcher itself).
const char *launcher_app_key(ShellApp app);
