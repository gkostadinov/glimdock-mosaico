// SPDX-License-Identifier: Apache-2.0
// Tools for the ESP-Mosaico shell: a torch (the camera module's flash, or the
// screen), a spirit level and a compass from the board's sensors, a
// calculator, and the Interaction sub-board (buttons, motion, light, LEDs, an
// infrared remote). UI calls run on the LVGL task (or with its lock held).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*exit)(void);
    // Light or darken the camera module's flash; false when there is none.
    bool (*torch)(bool on);
    // Gravity in g in the UI's coordinates (x right, y down, z out of the
    // glass); false without an accelerometer.
    bool (*gravity)(float *x, float *y, float *z);
    // A sensor-axis vector (the magnetometer's) turned into UI coordinates.
    void (*to_ui)(float *x, float *y, float *z);
    void (*haptic)(void);
    void (*brightness)(int percent);  // the screen light; -1 restores
    // USB webcam mode: switch it (returns at once; the switch takes a few seconds).
    void (*webcam)(bool on);
    // Its state as a line for the screen; true while on (or turning on).
    bool (*webcam_status)(char *text, size_t capacity);
} tools_hooks_t;

void tools_app_init(const tools_hooks_t *hooks);
void tools_app_open(lv_obj_t *screen);
void tools_app_set_active(bool active);
// 0 torch, 1 level, 2 compass, 3 calculator, 4 the Interaction board, 5 the USB webcam (before or after opening).
void tools_app_select(int tool);

// The Interaction sub-board, for Swifty (any task; each may wait a few seconds
// for the board to be found). report: its buttons, motion, light and lights.
// lights: mode off, react, rainbow, night or colour, and/or a colour (a name or
// #RRGGBB). ir: an LG TV key (power, volume_up, ...) or, with key empty, an NEC
// address and command. They write a sentence for Swifty into out.
void tools_board_report(char *out, size_t capacity);
bool tools_board_lights(const char *mode, const char *colour, char *out, size_t capacity);
bool tools_board_ir(const char *key, int address, int command, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
