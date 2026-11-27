// SPDX-License-Identifier: Apache-2.0
// Always-on display (always_on.cpp): after a while without a touch or the AI
// key, the app fades to low light for a few seconds (a touch brings it back),
// then the screen becomes a dim clock on black. The AMOLED lights only the
// pixels drawn, so a clock on black at a low panel brightness costs a small
// part of an app's light. It is drawn once a minute, a few pixels away each
// time against burn-in, and the app under it stops its own drawing meanwhile.
// A touch, the AI key or a due reminder brings back what was shown.
#pragma once

#include <stddef.h>
#include <time.h>

struct AlwaysOnInfo {
    bool timed = false;  // the clock is set
    tm local{};          // the clocks' time
    int battery = -1;    // percent; -1 unknown
    bool charging = false;
};

struct AlwaysOnHooks {
    // Something on screen must stay up: the viewfinder, Swifty listening or
    // answering, a reminder, the screen light, the webcam switching. Asked
    // every second; the wait starts again once it ends (and the clock gives
    // way when it starts).
    bool (*busy)();
    // The app shown stops its work (true) while the clock is up, and resumes it (false).
    void (*pause)(bool paused);
    // The panel's light now: a percent, or -1 for the person's setting.
    void (*light)(int percent);
    // The person's brightness setting, a percent.
    int (*brightness)();
    void (*info)(AlwaysOnInfo *out);
    // The screen may dim but the clock must not come up (webcam mode: what is
    // shown stays, at the clock's light once the wait is over).
    bool (*no_clock)();
};

// Builds the clock's screen, loads the setting and starts the check (LVGL
// task, display lock held).
void always_on_start(const AlwaysOnHooks &hooks);
// Seconds without use before the clock comes up (one of ALWAYS_ON_CHOICES; 0
// never). Any task; the LVGL task saves a change to NVS within a second.
int always_on_timeout();
void always_on_set_timeout(int seconds);
bool always_on_showing();  // the clock is up (any task)
bool always_on_resting();  // dimmed, or the clock is up (any task)
// The clock, or the dimmed stage, now (LVGL task; for testing over ESP-Iris).
void always_on_show();
void always_on_dim();
// Back to what was shown at full light, and counts as use (LVGL task;
// nothing when awake).
void always_on_wake();
// For the touch read (LVGL task): a touch on the clock or the dimmed app
// wakes the board and goes no further: true while the touch must not reach
// the app (that touch, until it is lifted).
bool always_on_touch(bool pressed);
// The board configuration's "display" section: {"always_on": seconds}.
const char *always_on_config_apply(const char *json, size_t length);
void always_on_config_status(char *json, size_t capacity);
// State and counts as text (RPC 49; LVGL task or with its lock held).
void always_on_report(char *out, size_t capacity);
