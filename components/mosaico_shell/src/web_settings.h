// SPDX-License-Identifier: Apache-2.0
// Web settings: the board's URLs and keys edited from a browser on the same
// network, while Settings' switch is on. A page at http://<the board's
// address>/ asks for a six-digit code shown on the board, then edits what
// tools/mosaico-config.py sets, through the same code. Keys are never sent
// back: the page shows only whether each is set. The server stops when
// switched off, after 10 minutes without a request, after 5 wrong codes and
// at every restart.
#pragma once

#include <stddef.h>

struct WebSettingsInfo {
    bool running;
    char url[40];    // "http://192.0.2.20"
    char code[8];    // "482913"
    char note[64];   // why it stopped, or what it needs
};

// Start (on the network the board is on) or stop the server; any task.
// start returns false (and sets the note) when it cannot.
bool web_settings_start();
void web_settings_stop(const char *note = nullptr);
void web_settings_info(WebSettingsInfo *out);
// Restarts the board to apply settings (set by the shell).
void web_settings_set_restart(void (*restart)());
