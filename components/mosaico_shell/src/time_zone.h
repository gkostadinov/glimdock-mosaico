// SPDX-License-Identifier: Apache-2.0
// The clocks' time zone (display/clockZone): set in Settings, or automatic
// from the network's location (looked up when Wi-Fi connects).
#pragma once

#include <stddef.h>

void time_zone_start();
void time_zone_connected();          // Wi-Fi is up: look the zone up (automatic only)
void time_zone_step(int step);       // Settings: the next/previous choice (automatic first)
void time_zone_text(char *out, size_t capacity);  // as Settings shows it
void time_zone_set(int choice);      // -1 automatic, else a zone (time_zone_name)
const char *time_zone_name(int zone);
int time_zone_count();
