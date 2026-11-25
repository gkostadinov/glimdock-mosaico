// SPDX-License-Identifier: Apache-2.0
// Tear-free display updates. Drawing the screen in strips straight to the
// panel while it refreshes showed half of the old frame and half of the new
// one while scrolling: an edge seemed to stick at the seam. Now LVGL renders
// whole frames into two PSRAM frame buffers (DIRECT mode: only what changed),
// and a sender task streams each finished frame's changed rows to the panel
// through two small internal DMA buffers, starting just behind the panel's
// scan (its TE pin marks each refresh).
//
// Why that is tear-free at 40 MHz: a full frame takes ~23 ms to send against
// a ~16.7 ms refresh. Started right behind the scan, the write is overtaken
// by the scan (which then shows the old frame, whole) and finishes before the
// next scan reaches each row (which then shows the new frame, whole).
#include "frame_sync.h"

#include <string.h>

#include <algorithm>
#include <atomic>

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
const char *TAG = "frame_sync";
// Lines per DMA transfer (15 KB): two chunks live in internal DMA memory,
// which TLS (the speech socket) and the camera driver also need.
constexpr int W = 480, H = 480, CHUNK = 16;
constexpr size_t FRAME_BYTES = size_t(W) * H * 2, CHUNK_BYTES = size_t(W) * CHUNK * 2;

struct {
    lv_display_t *disp = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    uint8_t *dma[2] = {};
    SemaphoreHandle_t job = nullptr, frame_done = nullptr, chunk_done = nullptr, scan = nullptr;
    std::atomic<bool> busy{false};
    // the frame being sent
    const uint8_t *src = nullptr;
    int y1 = 0, y2 = 0;
    // rows changed in the frame being rendered
    int dirty_y1 = H, dirty_y2 = -1;
    // TE timing (ISR)
    volatile int64_t last_rise = 0, last_fall = 0, high_us = 0, low_us = 0, scan_start = 0, period_us = 16667;
    volatile uint32_t edges = 0;
} F;

// TE marks vertical blanking. The shorter of its two phases is the blank; the
// edge that ends it is where the panel starts scanning from the top.
void IRAM_ATTR te_isr(void *)
{
    const int64_t now = esp_timer_get_time();
    const bool high = gpio_get_level(BSP_LCD_TE);
    F.edges = F.edges + 1;
    if (high) {
        if (F.last_fall) F.low_us = now - F.last_fall;
        if (F.last_rise) F.period_us = now - F.last_rise;
        F.last_rise = now;
    } else {
        if (F.last_rise) F.high_us = now - F.last_rise;
        F.last_fall = now;
    }
    // blank = high phase when it is the shorter one (the usual polarity)
    const bool blank_is_high = !F.low_us || !F.high_us || F.high_us <= F.low_us;
    if (high != blank_is_high) {  // this edge ends the blank: scanning starts
        F.scan_start = now;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(F.scan, &woken);
        if (woken) portYIELD_FROM_ISR();
    }
}

bool IRAM_ATTR chunk_sent(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(F.chunk_done, &woken);
    return woken == pdTRUE;
}

// RGB565 to the panel's byte order, two pixels at a time.
void copy_swapped(uint8_t *dst, const uint8_t *src, size_t bytes)
{
    auto *d = reinterpret_cast<uint32_t *>(dst);
    auto *s = reinterpret_cast<const uint32_t *>(src);
    for (size_t i = 0; i < bytes / 4; ++i) {
        const uint32_t v = s[i];
        d[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
}

// Waits until the scan has passed row y (plus a margin), so the write follows it.
void follow_scan(int y)
{
    xSemaphoreTake(F.scan, 0);  // drop a stale edge
    if (xSemaphoreTake(F.scan, pdMS_TO_TICKS(40)) != pdTRUE) return;  // no TE: send now
    const int64_t period = std::clamp<int64_t>(int64_t(F.period_us), 8000, 40000);
    const int64_t target = F.scan_start + int64_t(y) * period / H + 400;
    int64_t left = target - esp_timer_get_time();
    if (left > 2000) {
        vTaskDelay(pdMS_TO_TICKS(left / 1000 - 1));
        left = target - esp_timer_get_time();
    }
    if (left > 0) esp_rom_delay_us(uint32_t(left));
}

void sender(void *)
{
    for (;;) {
        xSemaphoreTake(F.job, portMAX_DELAY);
        const int y1 = std::max(0, F.y1 & ~1), y2 = std::min(H - 1, F.y2 | 1);
        follow_scan(y1);
        int inflight = 0, b = 0;
        for (int y = y1; y <= y2; y += CHUNK) {
            const int n = std::min(CHUNK, y2 - y + 1);
            if (inflight == 2) {
                xSemaphoreTake(F.chunk_done, portMAX_DELAY);  // the older transfer used this buffer
                --inflight;
            }
            copy_swapped(F.dma[b], F.src + size_t(y) * W * 2, size_t(n) * W * 2);
            if (esp_lcd_panel_draw_bitmap(F.panel, 0, y, W, y + n, F.dma[b]) == ESP_OK) ++inflight;
            b ^= 1;
        }
        while (inflight--) xSemaphoreTake(F.chunk_done, pdMS_TO_TICKS(200));
        F.busy = false;
        lv_display_flush_ready(F.disp);
        xSemaphoreGive(F.frame_done);
    }
}

// DIRECT mode: called per changed area; only the last one of a frame sends.
void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    F.dirty_y1 = std::min<int>(F.dirty_y1, area->y1);
    F.dirty_y2 = std::max<int>(F.dirty_y2, area->y2);
    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }
    F.src = px;
    F.y1 = F.dirty_y1, F.y2 = F.dirty_y2;
    F.dirty_y1 = H, F.dirty_y2 = -1;
    F.busy = true;
    xSemaphoreGive(F.job);  // the sender marks the flush ready once it is on the glass
}

// LVGL waits here (instead of spinning) before reusing a buffer being sent.
void flush_wait(lv_display_t *)
{
    while (F.busy) xSemaphoreTake(F.frame_done, pdMS_TO_TICKS(50));
}
}  // namespace

bool frame_sync_start(lv_display_t *display)
{
    F.disp = display;
    F.panel = bsp_display_get_panel();
    esp_lcd_panel_io_handle_t io = bsp_display_get_panel_io();
    uint8_t *fb0 = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t *fb1 = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (auto &d : F.dma) d = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    F.job = xSemaphoreCreateBinary();
    F.frame_done = xSemaphoreCreateBinary();
    F.chunk_done = xSemaphoreCreateCounting(4, 0);
    F.scan = xSemaphoreCreateBinary();
    if (!F.panel || !io || !fb0 || !fb1 || !F.dma[0] || !F.dma[1] || !F.job || !F.frame_done || !F.chunk_done || !F.scan) {
        ESP_LOGW(TAG, "not available; strips instead");
        heap_caps_free(fb0);
        heap_caps_free(fb1);
        for (auto &d : F.dma) heap_caps_free(d), d = nullptr;
        return false;
    }
    memset(fb0, 0, FRAME_BYTES);
    memset(fb1, 0, FRAME_BYTES);
    // Transfers now report to the sender (replacing the adapter's callback).
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = chunk_sent};
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, nullptr);
    // The panel's TE pin: an edge at each refresh.
    gpio_config_t te = {};
    te.pin_bit_mask = BIT64(BSP_LCD_TE);
    te.mode = GPIO_MODE_INPUT;
    te.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&te);
    const esp_err_t isr = gpio_install_isr_service(0);
    if (isr == ESP_OK || isr == ESP_ERR_INVALID_STATE) gpio_isr_handler_add(BSP_LCD_TE, te_isr, nullptr);
    lv_display_set_buffers(display, fb0, fb1, FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_flush_wait_cb(display, flush_wait);
    xTaskCreatePinnedToCore(sender, "frame_sync", 4096, nullptr, 8, nullptr, 0);
    lv_obj_invalidate(lv_screen_active());
    ESP_LOGI(TAG, "tear-free: full frames in PSRAM, sent behind the TE scan");
    return true;
}

bool frame_sync_wait_idle(int timeout_ms)
{
    const int64_t until = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    while (F.busy && esp_timer_get_time() < until) vTaskDelay(1);
    return !F.busy;
}

int frame_sync_te_period_us() { return F.edges > 2 ? int(F.period_us) : 0; }
