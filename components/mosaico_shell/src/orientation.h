// SPDX-License-Identifier: Apache-2.0
// Auto-rotate from the accelerometer, with an animated turn (orientation.cpp).
// Turning auto-rotate off returns the UI to its native orientation.
#pragma once

#include <algorithm>

#include "lvgl.h"

void orientation_start();
bool orientation_enabled();
void orientation_set_enabled(bool enabled);
// A vector in the IMU's own axes (the magnetometer is taken to share them),
// turned into the UI's current coordinates: x right, y down, z out of the glass.
void orientation_to_ui(float *x, float *y, float *z);
// The UI rotation (0, 90, 180, 270) that would stand upright for the way the
// board is held, from the accelerometer, whether auto-rotate is on or off
// (the last one while the board lies flat); the UI's own without an IMU.
int orientation_board_rotation();
// Gravity (filtered, in g) in the UI's current coordinates; false without an IMU.
bool orientation_gravity(float *x, float *y, float *z);
// While a game steers by tilt: the UI keeps its rotation, and the IMU
// (accelerometer and gyroscope) is read every 10 ms instead of every 50.
void orientation_game_mode(bool on);
// The way down (g) in the UI's coordinates, x right, y down, z into the glass
// (unlike orientation_gravity, whose x is mirrored); the gyroscope's rates
// (rad/s) about the same axes; and the board's turn about the vertical (rad,
// + clockwise seen from above; gyro false until game mode has measured it).
// False without an IMU.
bool orientation_motion(float gravity[3], float rate[3], float *yaw, bool *gyro);
