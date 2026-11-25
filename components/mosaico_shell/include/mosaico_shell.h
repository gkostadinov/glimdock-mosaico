// SPDX-License-Identifier: Apache-2.0
// ESP-Mosaico shell: display, shared Wi-Fi, the launcher and app switching.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the display, Wi-Fi from Vibe Mode shared information, the AI key
// (returns to the launcher) and the last-used application. Call once from
// app_main after NVS and Recovery are up.
esp_err_t mosaico_shell_start(void);

#ifdef __cplusplus
}
#endif
