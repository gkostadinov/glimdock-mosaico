// SPDX-License-Identifier: Apache-2.0
// The Interaction app: the Interaction sub-board. A worker task owns the board;
// the pane (LVGL task) and Swifty (its own task) go through it.
#pragma once

#include "lvgl.h"
#include "mui.h"

namespace interact {
void start();                                   // once: saved light mode, the worker
void build(lv_obj_t *pane, const mui::Accent &a);  // LVGL task
void update();                                  // LVGL task, while the pane shows
void set_visible(bool visible);                 // look for the board while shown
}  // namespace interact
