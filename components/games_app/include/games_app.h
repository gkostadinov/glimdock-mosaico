// SPDX-License-Identifier: Apache-2.0
// Games for the ESP-Mosaico shell: three retro 3D games (Ember Crypt, a
// lantern-lit maze crawl; Starbore, a neon tunnel flyer; Skyglide, a glider
// over voxel hills), a picker, title screens and best scores kept in NVS.
// Played by touch, or by tilting and turning the board (accelerometer and
// gyroscope), with the Interaction sub-board's buttons and lights when one is
// fitted. UI calls run on the LVGL task (or with its lock held).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Short sounds the games ask for (games_hooks_t.sound).
typedef enum {
    GAMES_SOUND_PICKUP = 0,
    GAMES_SOUND_HIT,
    GAMES_SOUND_LEVEL,
    GAMES_SOUND_OVER,
    GAMES_SOUND_POWER,
    GAMES_SOUND_WARN,
    GAMES_SOUND_START,
} games_sound_t;

typedef struct {
    float gravity[3];  // g, the way down, in UI coordinates: x right, y down, z into the glass
    float rate[3];     // rad/s about the same axes (0 without a gyroscope)
    float yaw;         // the board's turn about the vertical since start-up, rad, + to the right
    bool gyro;         // rate and yaw are measured
} games_motion_t;

typedef struct {
    void (*exit)(void);
    // The board's motion (any task); false without an IMU.
    bool (*motion)(games_motion_t *out);
    // While a game is played: keep the UI's rotation and read the IMU faster.
    void (*hold)(bool playing);
    void (*haptic)(bool strong);
    void (*sound)(games_sound_t sound);  // only called while the app's sound is on
    // The Interaction sub-board, when fitted (any task): look for it while
    // on, its two buttons, and a colour flash on its lights.
    void (*board_link)(bool on);
    bool (*board_buttons)(bool *left, bool *right);
    void (*board_flash)(uint32_t rgb);
} games_hooks_t;

void games_app_init(const games_hooks_t *hooks);
void games_app_open(lv_obj_t *screen);
// Leaving the app pauses a game (and stops drawing).
void games_app_set_active(bool active);
// A game is being played (not paused): the always-on display waits.
bool games_app_playing(void);
// Straight to a game's title screen (0 Ember Crypt, 1 Starbore, 2 Skyglide), -1 the picker.
void games_app_select(int game);

#ifdef __cplusplus
}
#endif
