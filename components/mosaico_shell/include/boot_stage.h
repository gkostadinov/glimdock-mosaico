// SPDX-License-Identifier: Apache-2.0
// The last start-up step reached, readable over ESP-Iris even when the display
// or the log ring shows nothing: `mosaico.py iris rpc 18244 9`.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void boot_stage_register(void);  // after ESP-Iris has started
void boot_stage(const char *stage);  // a string literal

#ifdef __cplusplus
}
#endif
