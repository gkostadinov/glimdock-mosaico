// SPDX-License-Identifier: Apache-2.0
// Tear-free display updates (frame_sync.cpp): whole frames in PSRAM, sent to
// the panel behind its TE-marked scan. Call frame_sync_start with the display
// lock held, in place of strip buffers; false leaves the display as it was.
#pragma once

#include "lvgl.h"

bool frame_sync_start(lv_display_t *display);
// Waits (up to timeout_ms) until no frame is being sent, e.g. before the
// panel's orientation changes. True when idle.
bool frame_sync_wait_idle(int timeout_ms);
// The measured refresh period in microseconds (0 until TE has been seen).
int frame_sync_te_period_us();
