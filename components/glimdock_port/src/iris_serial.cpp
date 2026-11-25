// SPDX-License-Identifier: Apache-2.0
#include "iris_serial.h"

#include <mutex>

#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"

namespace {
constexpr uint16_t SERVICE = 0x4744;
constexpr size_t IN_CAPACITY = 14 * 1024, OUT_CAPACITY = 1024;

std::mutex lock;
uint8_t *in = nullptr;  // PSRAM: may hold a provisioning frame with credentials
size_t in_head = 0, in_size = 0;
uint8_t out[OUT_CAPACITY];
size_t out_size = 0;

void wipe(volatile uint8_t *p, size_t n)
{
    while (n--) *p++ = 0;
}

esp_err_t write_input(const esp_iris_rpc_request_t *request, uint8_t *response, size_t capacity, size_t *size, void *)
{
    std::lock_guard<std::mutex> guard(lock);
    if (!in) in = static_cast<uint8_t *>(heap_caps_calloc(1, IN_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!in) return ESP_ERR_NO_MEM;
    size_t accepted = 0;
    for (; accepted < request->payload_size && in_size < IN_CAPACITY; ++accepted, ++in_size)
        in[(in_head + in_size) % IN_CAPACITY] = request->payload[accepted];
    *size = 0;
    if (capacity >= 4) {
        const uint32_t n = uint32_t(accepted);
        memcpy(response, &n, 4);
        *size = 4;
    }
    return ESP_OK;
}

esp_err_t read_output(const esp_iris_rpc_request_t *, uint8_t *response, size_t capacity, size_t *size, void *)
{
    std::lock_guard<std::mutex> guard(lock);
    const size_t n = std::min(capacity, out_size);
    memcpy(response, out, n);
    memmove(out, out + n, out_size - n);
    out_size -= n;
    wipe(out + out_size, n);
    *size = n;
    return ESP_OK;
}
}  // namespace

int IrisSerial::available()
{
    std::lock_guard<std::mutex> guard(lock);
    return int(in_size);
}

int IrisSerial::read()
{
    std::lock_guard<std::mutex> guard(lock);
    if (!in_size) return -1;
    const uint8_t value = in[in_head];
    in[in_head] = 0;
    in_head = (in_head + 1) % IN_CAPACITY;
    --in_size;
    return value;
}

int IrisSerial::peek()
{
    std::lock_guard<std::mutex> guard(lock);
    return in_size ? in[in_head] : -1;
}

size_t IrisSerial::write(uint8_t value)
{
    std::lock_guard<std::mutex> guard(lock);
    if (out_size >= OUT_CAPACITY) return 0;
    out[out_size++] = value;
    return 1;
}

IrisSerial &glim_iris_serial()
{
    static IrisSerial serial;
    return serial;
}

void glim_iris_serial_register()
{
    if (esp_iris_rpc_register(SERVICE, 1, write_input, nullptr) != ESP_OK ||
        esp_iris_rpc_register(SERVICE, 2, read_output, nullptr) != ESP_OK)
        ESP_LOGW("glim_serial", "private reader provisioning over ESP-Iris unavailable");
}
