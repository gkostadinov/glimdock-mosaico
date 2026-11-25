// SPDX-License-Identifier: Apache-2.0
// Swifty Home wall display (a self-hosted Swifty Home hub) for the ESP-Mosaico shell.
// All calls run on the LVGL task (or with its lock held).
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the hub client in the background (no UI is built until opened).
void home_app_start(lv_obj_t *frame);
// First call builds the UI inside frame (the 320-unit-wide layout is scaled to
// the frame's width; a taller frame shows more of each page) and starts the hub client.
void home_app_open(lv_obj_t *frame);
// While inactive the UI is not rebuilt; polling continues at a slower pace.
void home_app_set_active(bool active);
// What the launcher shows on the Home page.
typedef struct {
    bool started, paired, connected;
    char title[80], hint[80];
    bool lights;
    int lights_pct, lights_on, lights_count, lights_hue;
    char lights_state[40];
    bool music;
    int volume;
    char music_state[40];
    bool tv, tv_on;
    char tv_name[28], tv_state[40];
    struct {
        char name[28];
        bool on, known;
    } plugs[2];
    int plug_count;
} home_summary_t;
void home_app_summary(home_summary_t *out);

// For Swifty (call with the display lock held): the home as text, and a
// command. verb: lights_on, lights_off, brightness (value 0-100), color (value
// a hue 0-359 or a colour name), scene (value Movie, Evening or Lights off),
// volume (0-100), play, pause, mute, unmute, tv_on, tv_off, plug_on, plug_off.
// target: a room, light, plug or "all" (may be empty). Writes what was sent or
// why not.
void home_app_describe(char *out, size_t capacity);
void home_app_command(const char *verb, const char *target, const char *value, char *out, size_t capacity);

// Settings offers "Exit to Mosaico launcher"; the shell provides the action.
void home_app_set_exit_handler(void (*exit_to_launcher)(void));

// Pairs with a Swifty Home hub using a one-time display code, as the Pair
// page does (LVGL task; Home must be started). The outcome shows in
// home_app_pairing_info: "pairing", "paired as NAME" or "failed: WHY".
void home_app_pair(const char *hub_url, const char *code);
typedef struct {
    bool paired;
    char hub[160], name[64], note[128];
} home_pairing_info_t;
void home_app_pairing_info(home_pairing_info_t *out);

#ifdef __cplusplus
}
#endif
