// SPDX-License-Identifier: Apache-2.0
// Device platform for Home: esp_http_client with the ESP-IDF CA bundle (the
// hub has a public certificate), NVS preferences and esp_timer.
#include <string.h>

#include <string>
#include <vector>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "home_hub.h"
#include "home_prefs.h"
#include "nvs.h"

namespace home {
namespace {
constexpr size_t MAX_BODY = 192 * 1024;

std::string nvs_string(nvs_handle_t handle, const char *key)
{
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || !length) {
        return std::string();
    }
    std::string value(length, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) {
        return std::string();
    }
    value.resize(length - 1);
    return value;
}
}  // namespace

uint32_t now_ms()
{
    return uint32_t(esp_timer_get_time() / 1000);
}

namespace {
// One connection to the hub, kept open between requests. Opening a new TLS
// session for every poll (every 3 s) took most of a CPU core and made the
// whole UI stutter. Only the home-http worker thread uses it.
struct Connection {
    esp_http_client_handle_t client = nullptr;
    std::string origin;
    int timeout_ms = 0;
    std::vector<std::string> headers;  // names set on the last request
} conn;

std::string origin_of(const std::string &url)
{
    const size_t scheme = url.find("://");
    const size_t path = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    return path == std::string::npos ? url : url.substr(0, path);
}

void drop()
{
    if (conn.client) {
        esp_http_client_close(conn.client);
        esp_http_client_cleanup(conn.client);
    }
    conn.client = nullptr;
    conn.headers.clear();
}

// One try; reused is set when an already-open connection was used, so a
// failure may just be the hub having closed it.
HttpResponse attempt(const HttpRequest &request, bool *reused)
{
    HttpResponse out;
    const std::string origin = origin_of(request.url);
    if (conn.client && (conn.origin != origin || conn.timeout_ms != request.timeout_ms)) {
        drop();
    }
    *reused = conn.client != nullptr;
    if (!conn.client) {
        esp_http_client_config_t config = {};
        config.url = request.url.c_str();
        config.timeout_ms = request.timeout_ms;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.disable_auto_redirect = true;
        config.buffer_size = 2048;
        config.buffer_size_tx = 2048;
        config.keep_alive_enable = true;
        conn.client = esp_http_client_init(&config);
        if (!conn.client) {
            out.error = "client";
            return out;
        }
        conn.origin = origin;
        conn.timeout_ms = request.timeout_ms;
    } else {
        esp_http_client_set_url(conn.client, request.url.c_str());
    }
    esp_http_client_handle_t client = conn.client;
    esp_http_client_set_method(client, request.method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    for (const std::string &name : conn.headers) {
        esp_http_client_delete_header(client, name.c_str());
    }
    conn.headers.clear();
    for (const auto &header : request.headers) {
        esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
        conn.headers.push_back(header.first);
    }
    esp_err_t err = esp_http_client_open(client, int(request.body.size()));
    if (err == ESP_OK && !request.body.empty() &&
            esp_http_client_write(client, request.body.data(), int(request.body.size())) != int(request.body.size())) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        out.error = esp_err_to_name(err);
        drop();
        return out;
    }
    out.code = esp_http_client_get_status_code(client);
    char chunk[1024];
    for (;;) {
        const int got = esp_http_client_read(client, chunk, sizeof(chunk));
        if (got < 0) {
            out.error = "read";
            out.code = 0;
            drop();
            return out;
        }
        if (got == 0) {
            break;
        }
        if (out.body.size() + size_t(got) > MAX_BODY) {
            out.body.clear();
            out.error = "response too large";
            out.code = 0;
            drop();
            return out;
        }
        out.body.append(chunk, size_t(got));
    }
    if (!esp_http_client_is_complete_data_received(client)) {
        drop();    // can't reuse a half-read response
    }
    return out;
}
}  // namespace

HttpResponse http_perform(const HttpRequest &request)
{
    bool reused = false;
    HttpResponse out = attempt(request, &reused);
    if (out.code == 0 && reused) {
        out = attempt(request, &reused);    // the kept connection had gone stale
    }
    return out;
}

void prefs_load(std::string &hub, std::string &token, std::string &name, int &theme, std::string &room)
{
    nvs_handle_t handle;
    if (nvs_open("swifty", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    hub = nvs_string(handle, "hub");
    token = nvs_string(handle, "token");
    name = nvs_string(handle, "name");
    room = nvs_string(handle, "room");
    // "theme2": the AMOLED redesign starts every display in Dark (AMOLED);
    // the earlier "theme" (default Auto) is left behind.
    uint8_t value = 1;
    if (nvs_get_u8(handle, "theme2", &value) == ESP_OK && value <= 2) {
        theme = value;
    }
    nvs_close(handle);
}

void prefs_save(const std::string &hub, const std::string &token, const std::string &name, int theme,
                const std::string &room)
{
    nvs_handle_t handle;
    if (nvs_open("swifty", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_str(handle, "hub", hub.c_str());
    nvs_set_str(handle, "token", token.c_str());
    nvs_set_str(handle, "name", name.c_str());
    nvs_set_str(handle, "room", room.c_str());
    nvs_set_u8(handle, "theme2", uint8_t(theme));
    nvs_commit(handle);
    nvs_close(handle);
}

int prefs_clock_zone()
{
    nvs_handle_t handle;
    uint8_t zone = 0;
    if (nvs_open("display", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "clockZone", &zone);
        nvs_close(handle);
    }
    return zone;
}

}  // namespace home
