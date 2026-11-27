// SPDX-License-Identifier: Apache-2.0
// Glimdock notification chimes on the Mosaico ES8311 speaker. Same cues,
// envelope and volume curve as the firmware's audio.cpp; 48 kHz to match the
// BSP's I2S clock. Also the shell's reminder chime (glimdock_app_play_reminder).
#include "audio.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "glimdock_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
const char *TAG = "glim_audio";
constexpr int RATE = 48000;
QueueHandle_t queue = nullptr;
esp_codec_dev_handle_t speaker = nullptr;
std::atomic<bool> enabled{true};
std::atomic<uint8_t> volume{DEFAULT_SOUND_VOLUME};
bool ready = false;
// Not one of Glimdock's cues: the reminder chime, through the same queue.
constexpr uint8_t REMINDER = 0x40;
// The Games app's effects: GAME + games_sound_t.
constexpr uint8_t GAME = 0x50, GAME_SOUNDS = 7;
bool is_game(AudioCue cue)
{
    return uint8_t(cue) >= GAME && uint8_t(cue) < GAME + GAME_SOUNDS;
}

// A retro voice: a soft square (or noise) gliding from f0 to f1 over ms, with
// a short attack and release so it doesn't click.
bool voice(float f0, float f1, unsigned ms, int amplitude, bool noise = false)
{
    int16_t pcm[256];
    const unsigned frames = RATE * ms / 1000, fade = RATE * 4 / 1000;
    static float phase = 0;
    static uint32_t lfsr = 0xACE1u;
    for (unsigned offset = 0; offset < frames; offset += 128) {
        const unsigned count = std::min(128u, frames - offset);
        for (unsigned i = 0; i < count; i++) {
            const unsigned at = offset + i;
            const float t = float(at) / float(frames);
            const float hz = f0 + (f1 - f0) * t;
            phase += hz / RATE;
            phase -= floorf(phase);
            float v;
            if (noise) {
                lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
                v = (lfsr & 1) ? 0.7f : -0.7f;
            } else {
                // square with its edges rounded: the fundamental and a third
                v = std::sin(6.28318530718f * phase) + 0.33f * std::sin(3 * 6.28318530718f * phase);
            }
            const float envelope = std::min(1.0f, float(std::min(at, frames - 1 - at)) / fade) * (1 - 0.5f * t);
            const int16_t sample = int16_t(amplitude * envelope * v);
            pcm[2 * i] = pcm[2 * i + 1] = sample;
        }
        if (esp_codec_dev_write(speaker, pcm, int(count * 2 * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            return false;
        }
    }
    return true;
}

bool game_sound(uint8_t sound, int a)
{
    switch (sound) {
    case 0:  // a pickup: two bright blips up
        return voice(880, 1320, 55, a) && voice(1760, 1760, 45, a);
    case 1:  // a hit: a crunch over a falling thud
        return voice(0, 0, 90, a, true) && voice(300, 90, 140, a);
    case 2:  // a level: up the major chord
        return voice(523, 523, 70, a) && voice(659, 659, 70, a) && voice(784, 784, 70, a) && voice(1047, 1047, 160, a);
    case 3:  // game over: down and down
        return voice(660, 640, 150, a) && voice(494, 480, 150, a) && voice(392, 380, 150, a) && voice(262, 200, 320, a);
    case 4:  // the action: a rising whoosh
        return voice(260, 1300, 200, a);
    case 5:  // running low: two low beeps
        return voice(440, 440, 80, a / 2) && voice(0, 0, 50, 0) && voice(440, 440, 80, a / 2);
    case 6:  // start
        return voice(523, 523, 60, a) && voice(784, 784, 60, a) && voice(1047, 1047, 110, a);
    default:
        return true;
    }
}

bool tone(float hz, unsigned duration_ms, int amplitude_override = -1)
{
    int16_t pcm[256];
    const unsigned frames = RATE * duration_ms / 1000, fade = RATE * 8 / 1000;
    for (unsigned offset = 0; offset < frames; offset += 128) {
        const unsigned count = std::min(128u, frames - offset);
        const int amplitude = amplitude_override >= 0 ? amplitude_override : soundAmplitude(volume.load());
        for (unsigned i = 0; i < count; i++) {
            const unsigned at = offset + i;
            const float envelope = std::min(1.0f, float(std::min(at, frames - 1 - at)) / fade);
            const int16_t sample = int16_t(amplitude * envelope * std::sin(6.28318530718f * hz * at / RATE));
            pcm[2 * i] = pcm[2 * i + 1] = sample;
        }
        if (esp_codec_dev_write(speaker, pcm, int(count * 2 * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            return false;
        }
    }
    return true;
}

void worker(void *)
{
    AudioCue cue;
    bool have = false;  // a cue already taken from the queue
    for (;;) {
        if (!have && xQueueReceive(queue, &cue, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        have = false;
        if (is_game(cue)) {
            // Games: louder than the chimes, still by the volume; the speaker
            // stays open while effects follow each other closely.
            if (!volume.load()) {
                continue;
            }
            esp_codec_dev_sample_info_t format = {};
            format.bits_per_sample = 16;
            format.channel = 2;
            format.sample_rate = RATE;
            if (esp_codec_dev_open(speaker, &format) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "speaker open failed");
                continue;
            }
            esp_codec_dev_set_out_vol(speaker, 80);
            bool ok = game_sound(uint8_t(cue) - GAME, std::min(32767, 2 * soundAmplitude(volume.load())));
            while (ok && xQueueReceive(queue, &cue, pdMS_TO_TICKS(250)) == pdTRUE) {
                if (!is_game(cue)) {
                    have = true;  // a chime: after the speaker closes
                    break;
                }
                ok = game_sound(uint8_t(cue) - GAME, std::min(32767, 2 * soundAmplitude(volume.load())));
            }
            ok = ok && tone(0, 60, 0);  // let the DMA tail drain as silence
            esp_codec_dev_close(speaker);
            if (!ok) {
                ESP_LOGW(TAG, "speaker write failed; game sound stopped");
            }
            continue;
        }
        const bool reminder = uint8_t(cue) == REMINDER;
        if (!reminder && (!volume.load() || (!enabled.load() && cue != AudioCue::Test))) {
            continue;
        }
        esp_codec_dev_sample_info_t format = {};
        format.bits_per_sample = 16;
        format.channel = 2;
        format.sample_rate = RATE;
        if (esp_codec_dev_open(speaker, &format) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "speaker open failed");
            continue;
        }
        esp_codec_dev_set_out_vol(speaker, 80);
        if (reminder) {
            // G5 C6 E6, twice: louder than Glimdock's quiet chimes (meant to
            // be noticed across a room), whatever Glimdock's sound setting.
            const int amp = soundAmplitude(std::max<unsigned>(volume.load(), 60));
            bool ok = true;
            for (int round = 0; round < 2 && ok; ++round) {
                ok = tone(784, 130, amp) && tone(0, 30, 0) && tone(1047, 130, amp) && tone(0, 30, 0) && tone(1319, 260, amp) &&
                     tone(0, round ? 80 : 320, 0);
            }
            esp_codec_dev_close(speaker);
            if (!ok) {
                ESP_LOGW(TAG, "speaker write failed; reminder chime stopped");
            }
            continue;
        }
        const float first = cue == AudioCue::Alert ? 660 : 880, second = cue == AudioCue::Alert ? 440 : 1175;
        const bool ok = tone(first, 90) && tone(0, 45) && tone(second, cue == AudioCue::PrintComplete ? 150 : 110) &&
                        tone(0, 80);  // let the DMA tail drain as silence
        esp_codec_dev_close(speaker);
        if (!ok) {
            ESP_LOGW(TAG, "speaker write failed; chime stopped");
        }
    }
}
}  // namespace

bool boardAudioInit(bool sound_enabled)
{
    enabled.store(sound_enabled);
    if (ready) {
        return true;
    }
    speaker = bsp_audio_codec_speaker_init();
    queue = speaker ? xQueueCreate(4, sizeof(AudioCue)) : nullptr;
    if (!queue || xTaskCreate(worker, "glim-audio", 4096, nullptr, 2, nullptr) != pdPASS) {
        if (queue) {
            vQueueDelete(queue);
        }
        queue = nullptr;
        ESP_LOGW(TAG, "speaker unavailable");
        return false;
    }
    ready = true;
    ESP_LOGI(TAG, "ES8311 speaker ready; quiet notification chimes");
    return true;
}

bool boardAudioReady()
{
    return ready;
}
void boardAudioEnable(bool value)
{
    enabled.store(value);
}
void boardAudioVolume(unsigned value)
{
    volume.store(clampSoundVolume(value));
}
uint8_t boardAudioVolume()
{
    return volume.load();
}

extern "C" bool glimdock_app_play_reminder(void)
{
    if (!ready) {
        boardAudioInit(enabled.load());
    }
    const AudioCue cue = static_cast<AudioCue>(REMINDER);
    return ready && xQueueSend(queue, &cue, 0) == pdTRUE;
}

extern "C" bool glimdock_app_play_game_sound(int sound)
{
    if (sound < 0 || sound >= GAME_SOUNDS) {
        return false;
    }
    if (!ready) {
        boardAudioInit(enabled.load());
    }
    const AudioCue cue = static_cast<AudioCue>(GAME + sound);
    return ready && volume.load() && xQueueSend(queue, &cue, 0) == pdTRUE;
}

bool boardAudioPlay(AudioCue cue)
{
    return ready && volume.load() && (enabled.load() || cue == AudioCue::Test) && xQueueSend(queue, &cue, 0) == pdTRUE;
}
