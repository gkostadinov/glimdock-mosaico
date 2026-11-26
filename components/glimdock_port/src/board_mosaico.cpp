// SPDX-License-Identifier: Apache-2.0
// Glimdock board interface (board.h) for ESP-Mosaico. The shell owns display,
// touch and LVGL start-up; this file maps the remaining board calls.
#include "board.h"

#include <algorithm>

#include "bsp/esp_mosaico.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
const char *TAG = "glim_board";
constexpr int64_t BATTERY_PERIOD_US = 2000 * 1000;
}

void boardHoldPower() {}
void boardInit() {}
void boardLvglInit() {}

void boardBacklight(uint8_t percent)
{
    bsp_display_brightness_set(std::clamp<int>(percent, 1, 100));
}

// Read-only voltage from the BQ27220 (standard command Voltage(), 0x08, mV).
// Deliberately not bsp_battery_init(): it rewrites the gauge's battery profile
// whenever it differs from the BSP's, and Vibe Mode stores its own, so the two
// would rewrite gauge flash on every switch (and block the UI for ~6 s).
BoardBatteryStatus boardBattery()
{
    static BatteryMonitor monitor;
    static BoardBatteryStatus last;
    static int64_t next_read = 0;
    static i2c_master_dev_handle_t gauge = nullptr;
    static bool failed = false;
    const int64_t now = esp_timer_get_time();
    if (failed || now < next_read) {
        return last;
    }
    next_read = now + BATTERY_PERIOD_US;
    if (!gauge) {
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        i2c_device_config_t device = {};
        device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device.device_address = BSP_BATTERY_I2C_ADDR;
        device.scl_speed_hz = 100000;
        if (!bus || i2c_master_bus_add_device(bus, &device, &gauge) != ESP_OK) {
            failed = true;
            ESP_LOGW(TAG, "fuel gauge unavailable");
            return last;
        }
    }
    const uint8_t command = 0x08;  // Voltage()
    uint8_t value[2] = {};
    if (i2c_master_transmit_receive(gauge, &command, 1, value, sizeof(value), 50) == ESP_OK) {
        last = monitor.update(float(uint16_t(value[0] | (value[1] << 8))) / 1000.0f);
    }
    return last;
}

// The Mosaico exposes no dedicated power key to applications.
bool boardPowerButtonDown()
{
    return false;
}

bool boardPowerOff()
{
    // On battery this removes power. On USB the board stays up, so sleep with
    // the display and radios off until the AI key is pressed.
    bsp_power_set_shutdown(true);
    vTaskDelay(pdMS_TO_TICKS(400));
    if (esp_sleep_enable_ext1_wakeup_io(1ULL << BSP_BUTTON_AI_GPIO, ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
        bsp_power_set_shutdown(false);
        ESP_LOGW(TAG, "AI key cannot wake from deep sleep; staying on");
        return false;
    }
    bsp_power_enter_deep_sleep();
}

// The shell forwards touch transitions here (see glimdock_app.cpp).
