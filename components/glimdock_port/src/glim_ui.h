// SPDX-License-Identifier: Apache-2.0
// Glimdock for ESP-Mosaico: a full-panel UI written for the round-cornered
// 480x480 AMOLED on the Mosaico kit (mui.h). It reads the same data layer as
// the desk firmware (network.cpp: snapshots, node selection, connection
// settings); the firmware's own 320x240 UI is not used here.
#pragma once

#include "lvgl.h"

// Builds the UI on screen (a full-panel LVGL screen) and starts the network
// worker. Call once, on the LVGL task.
void glim_ui_start(lv_obj_t *screen);
// While inactive the UI is not rebuilt; the network keeps polling.
void glim_ui_set_active(bool active);

// Host preview only.
void glim_ui_preview(int page, int node, bool demo);
void glim_ui_preview_overlay(int which);
