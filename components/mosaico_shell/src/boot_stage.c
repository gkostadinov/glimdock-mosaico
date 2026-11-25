// SPDX-License-Identifier: Apache-2.0
#include "boot_stage.h"

#include <string.h>

#include "esp_iris.h"
#include "esp_timer.h"

static const char *volatile s_stage = "app_main";
static volatile int64_t s_at_us = 0;

static esp_err_t read_stage(const esp_iris_rpc_request_t *request, uint8_t *response, size_t capacity,
                            size_t *size, void *ctx)
{
    (void)request;
    (void)ctx;
    char text[96];
    const int n = snprintf(text, sizeof(text), "%s @%lldms (now %lldms)", s_stage, (long long)(s_at_us / 1000),
                           (long long)(esp_timer_get_time() / 1000));
    const size_t len = n < 0 ? 0 : (size_t)n < capacity ? (size_t)n : capacity;
    memcpy(response, text, len);
    *size = len;
    return ESP_OK;
}

void boot_stage_register(void) { esp_iris_rpc_register(0x4744, 9, read_stage, NULL); }

void boot_stage(const char *stage)
{
    s_stage = stage;
    s_at_us = esp_timer_get_time();
}
