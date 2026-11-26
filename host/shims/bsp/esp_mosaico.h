// SPDX-License-Identifier: Apache-2.0
// Host preview: no magnetometer.
#pragma once
#include "nvs.h"
#include <stdint.h>
struct bmm150_mag_data {
    int16_t x, y, z;
};
typedef enum { BSP_MAGNETOMETER_0 = 0 } bsp_magnetometer_t;
static inline esp_err_t bsp_magnetometer_init(bsp_magnetometer_t)
{
    return ESP_FAIL;
}
static inline esp_err_t bsp_magnetometer_read(bsp_magnetometer_t, struct bmm150_mag_data *)
{
    return ESP_FAIL;
}
