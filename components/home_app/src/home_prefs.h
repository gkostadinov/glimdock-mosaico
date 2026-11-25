// SPDX-License-Identifier: Apache-2.0
// Pairing and preferences storage (NVS namespace "swifty" on the device).
#pragma once

#include <string>

namespace home {
void prefs_load(std::string &hub, std::string &token, std::string &name, int &theme, std::string &room);
void prefs_save(const std::string &hub, const std::string &token, const std::string &name, int theme,
                const std::string &room);
// Glimdock's display clock zone, shared so both apps show the same time.
int prefs_clock_zone();
}  // namespace home
