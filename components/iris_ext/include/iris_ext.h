// SPDX-License-Identifier: Apache-2.0
// One ESP-Iris RPC that carries many. ESP-Iris holds at most 32 handlers (the
// Kconfig maximum of CONFIG_ESP_IRIS_MAX_RPC_HANDLERS) and the board already
// uses them all, so newer features register here under a sub-command instead:
// the Mac calls 0x4744/54 with the sub-command as the first payload byte and
// the feature's own payload after it. The handler sees only its own payload.
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_iris.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IRIS_EXT_SERVICE 0x4744
#define IRIS_EXT_METHOD 54

// Registers (or replaces) the handler for one sub-command. Any task, any time;
// the first call registers RPC 54 itself.
esp_err_t iris_ext_register(uint8_t sub, esp_iris_rpc_handler_t handler, void *user_ctx);

#ifdef __cplusplus
}
#endif
