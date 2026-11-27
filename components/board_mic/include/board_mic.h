// SPDX-License-Identifier: Apache-2.0
// The board's one microphone (the ES8311's ADC, one esp_codec_dev handle) has
// two users: Swifty's speech and webcam mode's USB microphone. Each claims it
// before opening the codec and releases it after closing, so neither reads or
// reformats it under the other.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// true when it was free (or already this user's); any task.
bool board_mic_claim(const char *who);
void board_mic_release(const char *who);
// Who has it ("swifty", "webcam"), or NULL.
const char *board_mic_owner(void);

#ifdef __cplusplus
}
#endif
