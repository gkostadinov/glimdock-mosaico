// SPDX-License-Identifier: Apache-2.0
// Settings > Wi-Fi: a full-panel sheet over the launcher (the top layer) with
// the board's network, the networks in range (scanned when it opens), a
// password on the on-screen keyboard, Other network... for a hidden one, and
// Forget. The network itself is board_wifi.h's. LVGL task.
#pragma once

void wifi_settings_open();
void wifi_settings_close();
bool wifi_settings_showing();
// The AI key: from a password or name page to the list, from the list to
// Settings. False when the sheet isn't open.
bool wifi_settings_back();
