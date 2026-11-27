// SPDX-License-Identifier: Apache-2.0
// Reminders Swifty sets (add_reminder, list_reminders, delete_reminder): kept
// in NVS, so they fire without the NAS and across restarts, once or every day,
// weekday or week. When one is due a card comes up over any app (the
// always-on clock gives way) with a buzz: Done, or Snooze for 10 minutes.
#pragma once

#include <stddef.h>
#include <time.h>

// Loads them, gives Swifty its tools and starts the check (on the LVGL task,
// display lock held).
void reminders_start();
// A reminder's card is up (LVGL task).
bool reminders_showing();
// The soonest reminder set: its time and text (any task). False: none.
bool reminders_next(time_t *at, char *text, size_t capacity);
