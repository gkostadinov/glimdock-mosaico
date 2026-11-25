// SPDX-License-Identifier: Apache-2.0
// LVGL configuration for the host preview: the device's generated Kconfig
// (build/config/sdkconfig.h), minus the FreeRTOS binding.
#pragma once
#include "sdkconfig.h"
#undef CONFIG_LV_OS_FREERTOS
#undef CONFIG_LV_USE_FREERTOS_TASK_NOTIFY
#define CONFIG_LV_OS_NONE 1
// The device allocates LVGL memory in PSRAM (lv_mem_psram.c); the host uses libc.
#undef CONFIG_LV_USE_CUSTOM_MALLOC
#undef CONFIG_LV_USE_BUILTIN_MALLOC
#define CONFIG_LV_USE_CLIB_MALLOC 1
// The device renders on both cores; without an OS the preview has one draw unit.
#undef CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT
#define CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT 1
