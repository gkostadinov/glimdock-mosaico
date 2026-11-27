// SPDX-License-Identifier: Apache-2.0
// ESP-Iris screenshot backend for an LVGL display: `mosaico.py iris screenshot`
// captures the active screen as RGB565 through LVGL's snapshot renderer.
#include "shell_internal.h"

#include <string.h>

#include <algorithm>

#include "bsp/esp_mosaico.h"
#include "esp_iris.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
const char *TAG = "shell_shot";
lv_draw_buf_t *capture = nullptr;
bool ready = false;

SemaphoreHandle_t rendered = nullptr;

// Runs on the LVGL task: a full-screen render needs far more stack than the
// ESP-Iris service task has (it overflowed there).
// The 460 KB frame lives in PSRAM; LVGL's default allocator could not place it.
lv_draw_buf_t shot;
uint8_t *shot_data = nullptr;

// What is over the app (the top layer: a reminder card, a pop-up), blended
// onto the screen's snapshot, as the panel shows it.
void blend_top_layer(uint32_t w, uint32_t h, uint32_t stride)
{
    lv_obj_t *top = lv_layer_top();
    bool shown = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); ++i) {
        shown |= !lv_obj_has_flag(lv_obj_get_child(top, int32_t(i)), LV_OBJ_FLAG_HIDDEN);
    }
    if (!shown) {
        return;
    }
    lv_draw_buf_t *layer = lv_snapshot_take(top, LV_COLOR_FORMAT_ARGB8888);
    if (!layer) {
        return;
    }
    const uint32_t lw = std::min<uint32_t>(w, layer->header.w), lh = std::min<uint32_t>(h, layer->header.h);
    for (uint32_t y = 0; y < lh; ++y) {
        const uint8_t *src = layer->data + y * layer->header.stride;
        uint16_t *dst = reinterpret_cast<uint16_t *>(shot_data + y * stride);
        for (uint32_t x = 0; x < lw; ++x, src += 4) {
            const uint32_t a = src[3];
            if (!a) {
                continue;
            }
            const uint16_t d = dst[x];
            const uint32_t r = ((src[2] * a) + (((d >> 11) & 31) * 255 / 31) * (255 - a)) / 255;
            const uint32_t g = ((src[1] * a) + (((d >> 5) & 63) * 255 / 63) * (255 - a)) / 255;
            const uint32_t b = ((src[0] * a) + ((d & 31) * 255 / 31) * (255 - a)) / 255;
            dst[x] = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
    lv_draw_buf_destroy(layer);
}

void render_snapshot(void *)
{
    lv_obj_t *screen = lv_screen_active();
    const uint32_t w = uint32_t(lv_obj_get_width(screen)), h = uint32_t(lv_obj_get_height(screen));
    const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    if (!shot_data) {
        shot_data = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, size_t(stride) * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (shot_data && lv_draw_buf_init(&shot, w, h, LV_COLOR_FORMAT_RGB565, stride, shot_data, stride * h) == LV_RESULT_OK &&
            lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, &shot) == LV_RESULT_OK) {
        capture = &shot;
        blend_top_layer(w, h, stride);
    }
    xSemaphoreGive(rendered);
}

esp_err_t take()
{
    if (!rendered && !(rendered = xSemaphoreCreateBinary())) {
        return ESP_ERR_NO_MEM;
    }
    if (!bsp_display_lock(2000)) {
        return ESP_ERR_TIMEOUT;
    }
    capture = nullptr;
    xSemaphoreTake(rendered, 0);
    lv_async_call(render_snapshot, nullptr);
    bsp_display_unlock();
    if (xSemaphoreTake(rendered, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ready = capture != nullptr;
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t frame_bytes()
{
    return capture ? size_t(capture->header.stride) * capture->header.h : 0;
}

esp_err_t begin(const esp_iris_media_desc_t *, esp_iris_media_desc_t *actual, uint32_t *total, void *)
{
    if (!actual || !total) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = take();
    if (err != ESP_OK) {
        return err;
    }
    *actual = {};
    actual->width = uint16_t(capture->header.w);
    actual->height = uint16_t(capture->header.h);
    actual->stride = capture->header.stride;
    actual->format = ESP_IRIS_PIXEL_FORMAT_RGB565;
    *total = uint32_t(frame_bytes());
    return ESP_OK;
}

esp_err_t read(uint32_t offset, uint8_t *out, size_t capacity, size_t *out_size, void *)
{
    if (!out || !out_size || !capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    if (offset == 0 && !ready) {  // a mirror stream re-reads from the start
        esp_err_t err = take();
        if (err != ESP_OK) {
            return err;
        }
    }
    const size_t total = frame_bytes();
    if (!ready || offset >= total) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t size = std::min<size_t>(capacity, total - offset);
    memcpy(out, capture->data + offset, size);
    *out_size = size;
    if (offset + size == total) {
        ready = false;
    }
    return ESP_OK;
}

void end(void *)
{
    capture = nullptr;  // the PSRAM frame is reused by the next capture
    ready = false;
}
}  // namespace

void shell_screenshot_register()
{
    const esp_iris_screen_backend_t backend = {begin, read, end, nullptr};
    esp_err_t err = esp_iris_screen_register(&backend);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "screenshot backend unavailable: %s", esp_err_to_name(err));
    }
}
