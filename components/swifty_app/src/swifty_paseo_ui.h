// SPDX-License-Identifier: Apache-2.0
// The card Swifty shows before it makes a Paseo agent act (a message, a new
// agent, a schedule run). LVGL only, so the host preview draws it too. Runs on
// the LVGL task (or with its lock held).
#pragma once

#include <string>

#include "lvgl.h"

namespace swifty {

struct PaseoAsk {
    std::string title;     // "Send to Homelab audit?"
    std::string subtitle;  // the agent's state, or the workspace and provider
    std::string detail;    // what the agent will get
    std::string action;    // the button: "Send", "Start", "Run"
};

// A dimmed panel over parent (lv_layer_top() on the board) that takes every
// touch, with the card on it: Cancel and the action. answer runs on the LVGL
// task, true for the action and false for Cancel; the caller deletes the
// returned object.
lv_obj_t *paseo_confirm_card(lv_obj_t *parent, const PaseoAsk &ask, void (*answer)(bool yes, void *ctx), void *ctx);

}  // namespace swifty
