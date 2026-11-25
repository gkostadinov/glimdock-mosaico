// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>

#include "lvgl.h"

void shell_screenshot_register();

// Applications hosted by the shell. Values are persisted; keep them stable.
enum class ShellApp : uint8_t { Launcher = 0, Glimdock = 1, Home = 2, Camera = 3, Swifty = 4, Files = 5, Tools = 6, Gallery = 7 };
constexpr int SHELL_APPS = 8;

// Implemented in shell.cpp; safe from the LVGL task only.
void shell_show(ShellApp app);
