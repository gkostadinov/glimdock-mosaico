// SPDX-License-Identifier: Apache-2.0
// Tools for the ESP-Mosaico shell: a torch (the camera module's flash, or the
// screen), a spirit level and a compass from the board's sensors (the Tools
// app's tabs), and three apps of their own drawn on the same screen: a
// calculator, the Interaction sub-board (buttons, motion, light, LEDs, an
// infrared remote) and the USB webcam. UI calls run on the LVGL task (or with
// its lock held).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

// The tools: Tools' tabs, then the apps of their own.
enum { TOOLS_TORCH, TOOLS_LEVEL, TOOLS_COMPASS, TOOLS_CALCULATOR, TOOLS_INTERACTION, TOOLS_WEBCAM };

void tools_app_init(const tools_hooks_t *hooks);
// Tools, on its last tab.
void tools_app_open(lv_obj_t *screen);
// A tool on screen: torch, level or compass as Tools (with its tabs), the
// calculator, Interaction board or webcam as an app of its own; -1 is Tools.
void tools_app_open_tool(lv_obj_t *screen, int tool);
void tools_app_set_active(bool active);
// Tools' tab (torch, level or compass), before or after opening.
void tools_app_select(int tool);

// The Interaction sub-board, for Swifty (any task; each may wait a few seconds
// for the board to be found). report: its buttons, motion, light and lights.
// lights: mode off, react, rainbow, night or colour, and/or a colour (a name or
// #RRGGBB). ir: an LG TV key (power, volume_up, ...) or, with key empty, an NEC
// address and command. They write a sentence for Swifty into out.
void tools_board_report(char *out, size_t capacity);
bool tools_board_lights(const char *mode, const char *colour, char *out, size_t capacity);
bool tools_board_ir(const char *key, int address, int command, char *out, size_t capacity);
// The Interaction sub-board for the Games app (any task): look for it and poll
// it fast while on; its two buttons (false: no board); a colour flash on its
// lights that fades in about half a second.
void tools_board_game(bool on);
bool tools_board_buttons(bool *left, bool *right);
void tools_board_flash(uint32_t rgb);

// The Interaction sub-board for shortcuts (any task). watch: keep looking for
// the board and reading it while shortcuts start on its motion or buttons.
// inputs: its state, with counts of presses and motion that only go up (a
// press between two looks still counts; they start again at 0 when the board
// is found). wait_ms: how long to wait for a board not found yet. False: none.
typedef struct {
    bool left, right, motion;
    int light;             // 0-100, relative; -1 unknown
    int32_t motion_age_s;  // since the last motion; -1 none seen
    uint32_t left_presses, right_presses, motions;
} tools_board_inputs_t;
void tools_board_watch(bool on);
bool tools_board_inputs(tools_board_inputs_t *out, int wait_ms);

#ifdef __cplusplus
}
#endif
