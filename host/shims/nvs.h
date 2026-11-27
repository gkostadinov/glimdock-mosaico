// SPDX-License-Identifier: Apache-2.0
// Host preview: NVS does nothing (Tools keeps its last tab and compass
// calibration in NVS on the board).
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#endif
static inline esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *)
{
    return ESP_FAIL;
}
static inline esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *)
{
    return ESP_FAIL;
}
static inline esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t)
{
    return ESP_FAIL;
}
static inline esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *)
{
    return ESP_FAIL;
}
static inline esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t)
{
    return ESP_FAIL;
}
static inline esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *)
{
    return ESP_FAIL;
}
static inline esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t)
{
    return ESP_FAIL;
}
static inline esp_err_t nvs_commit(nvs_handle_t)
{
    return ESP_FAIL;
}
static inline void nvs_close(nvs_handle_t) {}
