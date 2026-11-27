// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>

#include "lvgl.h"

void shell_screenshot_register();

// Applications hosted by the shell. Values are persisted; keep them stable.
// Webcam, Interaction and Calculator are Tools' screen without its tabs.
enum class ShellApp : uint8_t {
    Launcher = 0, Glimdock = 1, Home = 2, Camera = 3, Swifty = 4, Files = 5, Tools = 6, Gallery = 7,
    Webcam = 8, Interaction = 9, Calculator = 10, Vpn = 11, Games = 12
};
constexpr int SHELL_APPS = 13;

// The always-on display's timeouts that Settings offers, in seconds (0: never).
constexpr int ALWAYS_ON_CHOICES[] = {0, 30, 60, 120, 300, 600};
constexpr int ALWAYS_ON_COUNT = int(sizeof(ALWAYS_ON_CHOICES) / sizeof(ALWAYS_ON_CHOICES[0]));

// Implemented in shell.cpp; safe from the LVGL task only.
void shell_show(ShellApp app);
// "launcher", or the app's key on the home screen (launcher_apps.cpp): its name
// for the log and Swifty's open_app.
const char *shell_app_name(ShellApp app);
