// SPDX-License-Identifier: Apache-2.0
// Frame profiler and scripted scroll over ESP-Iris (perf.cpp). Call with the
// display lock held.
#pragma once

#include <algorithm>

#include "lvgl.h"

void perf_start(lv_display_t *display);
// CPU per task (% of one core, the busiest 8 at 2 % or more) since the
// previous call; out may be null to only start a window. One caller at a time.
void perf_cpu_window(char *out, size_t capacity);
