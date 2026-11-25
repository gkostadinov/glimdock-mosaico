// SPDX-License-Identifier: Apache-2.0
// The Waveshare build records reset and heap history for a USB health command.
// On ESP-Mosaico, USB belongs to ESP-Iris (logs, screenshots, recovery), so
// health is reported through the normal log instead.
#include "runtime_health.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

namespace {
const char *TAG = "glim_health";
uint32_t last_report = 0;
}

void startRuntimeHealth()
{
    ESP_LOGI(TAG, "reset reason %d; internal heap %u (largest block %u), PSRAM %u", int(esp_reset_reason()),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void tickRuntimeHealth()
{
    const uint32_t now = millis();
    if (now - last_report < 60000) {
        return;
    }
    last_report = now;
    // DMA-capable internal memory feeds every display SPI transfer.
    ESP_LOGI(TAG, "uptime %lus; internal heap %u (min %u, largest block %u), DMA %u (largest %u), PSRAM %u",
             (unsigned long)(now / 1000), unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

bool handleRuntimeHealthLine(char *, size_t, Stream &)
{
    return false;
}
