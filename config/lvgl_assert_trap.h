// SPDX-License-Identifier: Apache-2.0
// LVGL's assert handler (CONFIG_LV_ASSERT_HANDLER_INCLUDE). The default spins
// forever (while(1);), freezing the display with no trace; trap instead so the
// panic handler records a backtrace that ESP-Iris keeps.
#pragma once
#include <assert.h>
#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER __builtin_trap();
