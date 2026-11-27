// SPDX-License-Identifier: Apache-2.0
// Reminders Swifty sets (add_reminder, list_reminders, delete_reminder): kept
// in NVS, so they fire without the NAS and across restarts, once or every day,
// weekday or week. When one is due a card comes up over any app with a buzz:
// Done, or Snooze for 10 minutes.
#pragma once

// Loads them, gives Swifty its tools and starts the check (on the LVGL task,
// display lock held).
void reminders_start();
