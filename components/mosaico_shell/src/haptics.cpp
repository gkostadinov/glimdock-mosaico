// SPDX-License-Identifier: Apache-2.0
// Touch haptics on the Mosaico vibration motor. A pulse switches the motor on
// and an esp_timer switches it off, so callers (the LVGL touch read) never wait.
#include "haptics.h"

#include "Preferences.h"
#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
const char *TAG = "haptics";
bool ready = false, enabled = true;
esp_timer_handle_t off_timer = nullptr;

void motor_off(void *)
{
    bsp_motor_set(false);
}
}  // namespace

void haptics_init()
{
    Preferences p;
    if (p.begin("mshell", true)) {
        enabled = p.getBool("haptics", true);
        p.end();
    }
    const esp_timer_create_args_t args = {
        .callback = motor_off,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "haptic_off",
        .skip_unhandled_events = true,
    };
    if (bsp_motor_init() != ESP_OK || esp_timer_create(&args, &off_timer) != ESP_OK) {
        ESP_LOGW(TAG, "vibration motor unavailable");
        return;
    }
    bsp_motor_set(false);
    ready = true;
}

void haptics_pulse(HapticPulse pulse)
{
    if (!ready || !enabled) {
        return;
    }
    // Strength and length follow the BSP's own tap feedback (18-60 ms).
    const uint8_t strength = pulse == HapticPulse::Tap ? 24 : 45;
    const uint64_t duration_us = pulse == HapticPulse::Tap ? 18000 : 40000;
    esp_timer_stop(off_timer);
    bsp_motor_set_strength(strength);
    bsp_motor_set(true);
    esp_timer_start_once(off_timer, duration_us);
}

bool haptics_enabled()
{
    return enabled;
}

void haptics_set_enabled(bool value)
{
    enabled = value;
    Preferences p;
    if (p.begin("mshell", false)) {
        p.putBool("haptics", value);
        p.end();
    }
    if (!value && ready) {
        esp_timer_stop(off_timer);
        bsp_motor_set(false);
    }
}
