// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

enum class HapticPulse : uint8_t { Tap, Key };

void haptics_init();
// Non-blocking: starts the motor and schedules it off.
void haptics_pulse(HapticPulse pulse);
bool haptics_enabled();
void haptics_set_enabled(bool enabled);
