// SPDX-License-Identifier: Apache-2.0
// Push-to-talk speech recognition: the microphone at 16 kHz mono, streamed
// in 80 ms chunks to a Qwen/DashScope-style realtime ASR WebSocket
// (run-task, binary audio, finish-task; result-generated sentences), as in
// ESP-Claw's asr_provider_qwen. Audio is captured from the press, before the
// socket is up, so the first words are not lost. A dictation may last up to
// five minutes: when the connection drops on the way, a new one picks up from
// the last final words.
#pragma once

#include <functional>
#include <string>

namespace swifty {

struct AsrEvents {
    std::function<void(const std::string &text)> partial;  // replace-only snapshot
    std::function<void(float level)> level;                 // 0..1, every 20 ms frame
};

// Starts listening (returns false if the microphone or configuration is missing).
bool asr_start(const AsrEvents &events);
// Stops the microphone, waits for the final words and returns them. error is
// set when recognition failed: with no words, or with the words heard before
// it did (they may be cut short).
std::string asr_finish(std::string *error);
// Drops everything without waiting (the connection closes in the background).
void asr_cancel();
bool asr_active();
// While Swifty is open: keep a connection ready so a press does not wait for
// the TLS handshake (seconds). Off: the waiting connection closes.
void asr_warm(bool on);

}  // namespace swifty
