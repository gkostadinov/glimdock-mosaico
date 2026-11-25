// SPDX-License-Identifier: Apache-2.0
// Swifty's endpoints and keys, in NVS ("swifty_agent"). They arrive from a 0600
// file on the Mac over ESP-Iris (tools/mosaico-config.py, service
// 0x4744 method 40) and are never sent back: the status call reports only
// which keys are set.
#pragma once

#include <stddef.h>

#include <string>

namespace swifty {

struct Config {
    std::string llm_url, llm_key, llm_model;  // OpenAI-compatible, e.g. http://host:8080/v1
    std::string asr_url, asr_key, asr_model;  // Qwen/DashScope-style realtime ASR WebSocket
    std::string asr_ca;                        // optional PEM when the ASR host is not publicly signed
    std::string search_key;                    // Tavily web search (api.tavily.com); empty: no web search
    bool llm_ready() const { return !llm_url.empty(); }
    bool asr_ready() const { return !asr_url.empty() && !asr_key.empty() && !asr_model.empty(); }
};

// A copy of the current configuration (thread-safe).
Config config();
// Loads NVS and registers the provisioning RPC.
void config_start();
// Bumped whenever a new configuration is saved.
unsigned config_revision();

// Another app's section of the board configuration (see swifty_app.h).
using SectionApply = const char *(*)(const char *json, size_t length);
using SectionStatus = void (*)(char *json, size_t capacity);
void config_section(const char *name, SectionApply apply, SectionStatus status);

}  // namespace swifty
