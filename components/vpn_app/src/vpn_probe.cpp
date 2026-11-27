// SPDX-License-Identifier: Apache-2.0
// ESP-Iris RPC 0x4744/52, for testing where the board's traffic goes: payload
// a URL (http:// or https://) starts a GET of it; an empty payload returns the
// last result: the HTTP status, time, the route and DNS servers in use, and
// the start of the body. A what's-my-IP address shows the public address the
// board leaves from, with the VPN on and off.
#include <string.h>

#include <mutex>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vpn_tunnel.h"

namespace {
const char *TAG = "vpn";
std::mutex lock;
std::string url, result = "no probe yet";
bool busy = false;

void probe(void *)
{
    std::string target;
    {
        std::lock_guard<std::mutex> g(lock);
        target = url;
    }
    char route[160];
    vpn::tunnel_route(route, sizeof(route));
    esp_http_client_config_t c = {};
    c.url = target.c_str();
    c.timeout_ms = 10000;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.buffer_size = 1024;
    std::string body, text;
    const int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_err_t err = h ? esp_http_client_open(h, 0) : ESP_ERR_NO_MEM;
    int status = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        char buf[200];
        const int n = esp_http_client_read(h, buf, sizeof(buf) - 1);
        if (n > 0) {
            body.assign(buf, size_t(n));
        }
    }
    if (h) {
        esp_http_client_cleanup(h);
    }
    const int ms = int((esp_timer_get_time() - t0) / 1000);
    if (err == ESP_OK) {
        text = "HTTP " + std::to_string(status) + " in " + std::to_string(ms) + " ms; " + route + "; body: " + body;
    } else {
        text = std::string("failed (") + esp_err_to_name(err) + ") after " + std::to_string(ms) + " ms; " + route;
    }
    ESP_LOGI(TAG, "probe: %s", err == ESP_OK ? "done" : esp_err_to_name(err));
    {
        std::lock_guard<std::mutex> g(lock);
        result = text;
        busy = false;
    }
    vTaskDeleteWithCaps(nullptr);
}

esp_err_t rpc(const esp_iris_rpc_request_t *request, uint8_t *out, size_t capacity, size_t *size, void *)
{
    std::string reply;
    if (request->payload_size) {
        std::lock_guard<std::mutex> g(lock);
        if (busy) {
            reply = "busy";
        } else {
            url.assign(reinterpret_cast<const char *>(request->payload), request->payload_size);
            if (url.rfind("http://", 0) && url.rfind("https://", 0)) {
                reply = "expected an http:// or https:// URL";
            } else {
                // TLS wants a roomy stack; it never writes flash, so PSRAM.
                busy = xTaskCreatePinnedToCoreWithCaps(probe, "vpn-probe", 8192, nullptr, 2, nullptr, tskNO_AFFINITY,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
                reply = busy ? "started" : "no memory";
            }
        }
    } else {
        std::lock_guard<std::mutex> g(lock);
        reply = busy ? "running" : result;
    }
    *size = std::min(reply.size(), capacity);
    memcpy(out, reply.data(), *size);
    return ESP_OK;
}
}  // namespace

namespace vpn {
void probe_register()
{
    esp_iris_rpc_register(0x4744, 52, rpc, nullptr);
}
}  // namespace vpn
