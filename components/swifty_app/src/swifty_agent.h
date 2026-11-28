// SPDX-License-Identifier: Apache-2.0
// Swifty's agent: an OpenAI-compatible /chat/completions loop (streamed),
// with tools for the board (camera, photos on screen, Swifty Home status,
// time) and camera photos sent to the model as images. One request at a
// time, on its own task; events arrive on that task, not the LVGL one.
#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "swifty_figure.h"

namespace swifty {

using Jpeg = std::shared_ptr<std::vector<uint8_t>>;

struct AgentEvents {
    std::function<void(const std::string &text)> text;         // the reply so far (replace-only)
    std::function<void(const std::string &what)> tool;         // a tool started, in words
    std::function<void(int photo)> show_photo;                 // put a camera photo in the chat
    std::function<void()> camera;                              // start the camera app and its photo list (waits)
    std::function<void(const std::string &text, Shot mood, bool failed)> done;
};

void agent_start();
// Sends a message with optional photos; false while a reply is running.
bool agent_send(const std::string &text, const std::vector<Jpeg> &photos, bool spoken, const AgentEvents &events);
void agent_stop();
void agent_reset();  // a new conversation (a reply under way is not kept)
bool agent_busy();
bool agent_developer();  // developer mode now (swifty_policy_t)

}  // namespace swifty
