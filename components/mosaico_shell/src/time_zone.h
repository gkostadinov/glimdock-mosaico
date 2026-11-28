// SPDX-License-Identifier: Apache-2.0
// The clocks' time zone (display/clockZone): set in Settings, or automatic
// from the network's location (looked up when Wi-Fi connects). The same lookup
// gives the weather its place (weather.h): one request for both.
#pragma once

#include <stddef.h>

void time_zone_start();
void time_zone_connected();          // Wi-Fi is up: look the zone up (automatic only)
void time_zone_step(int step);       // Settings: the next/previous choice (automatic first)
void time_zone_text(char *out, size_t capacity);  // as Settings shows it
void time_zone_set(int choice);      // -1 automatic, else a zone (time_zone_name)
const char *time_zone_name(int zone);
int time_zone_count();
// The network's location from the last lookup: city-level coordinates and the
// city's name. False until one succeeded. Any task.
bool time_zone_location(float *latitude, float *longitude, char *city, size_t capacity);
// Someone needs the location (the weather, automatic): look it up even while
// the time zone is set by hand. Any task.
void time_zone_want_location();
