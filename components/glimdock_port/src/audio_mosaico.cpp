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
    for (;;) {
        if (xQueueReceive(queue, &cue, portMAX_DELAY) != pdTRUE) {
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

bool boardAudioPlay(AudioCue cue)
{
    return ready && volume.load() && (enabled.load() || cue == AudioCue::Test) && xQueueSend(queue, &cue, 0) == pdTRUE;
}
