// SPDX-License-Identifier: Apache-2.0
// See iris_ext.h.
#include "iris_ext.h"

#include <stdatomic.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "iris-ext";

typedef struct {
    esp_iris_rpc_handler_t handler;
    void *ctx;
} entry_t;

static entry_t table[256];
static portMUX_TYPE table_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_int state;  // 0 not registered, 1 registering, 2 registered

static esp_err_t dispatch(const esp_iris_rpc_request_t *request, uint8_t *response, size_t capacity, size_t *size,
                          void *unused)
{
    (void)unused;
    *size = 0;
    if (request->payload_size < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t sub = request->payload[0];
    taskENTER_CRITICAL(&table_lock);
    const entry_t e = table[sub];
    taskEXIT_CRITICAL(&table_lock);
    if (!e.handler) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_iris_rpc_request_t inner = *request;  // the same request, minus the sub-command byte
    inner.payload = request->payload + 1;
    inner.payload_size = request->payload_size - 1;
    return e.handler(&inner, response, capacity, size, e.ctx);
}

esp_err_t iris_ext_register(uint8_t sub, esp_iris_rpc_handler_t handler, void *user_ctx)
{
    if (!handler) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&table_lock);
    table[sub].handler = handler;
    table[sub].ctx = user_ctx;
    taskEXIT_CRITICAL(&table_lock);
    int expected = 0;
    if (atomic_compare_exchange_strong(&state, &expected, 1)) {
        const esp_err_t err = esp_iris_rpc_register(IRIS_EXT_SERVICE, IRIS_EXT_METHOD, dispatch, NULL);
        atomic_store(&state, err == ESP_OK ? 2 : 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RPC %d not registered: %s", IRIS_EXT_METHOD, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
