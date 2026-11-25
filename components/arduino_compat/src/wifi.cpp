// SPDX-License-Identifier: Apache-2.0
#include "WiFi.h"

#include <atomic>
#include <mutex>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

WiFiClass WiFi;

namespace {
const char *TAG = "wifi";
std::once_flag init_once;
bool init_ok = false;
esp_netif_t *sta_netif = nullptr, *ap_netif = nullptr;
std::atomic<int> sta_status{WL_IDLE_STATUS};
std::atomic<uint32_t> sta_ip{0};
std::atomic<bool> sta_wanted{false};
std::mutex callbacks_mutex;
std::vector<std::pair<WiFiEvent_t, WiFiEventCb>> callbacks;

void dispatch(WiFiEvent_t event, const WiFiEventInfo_t &info)
{
    std::vector<WiFiEventCb> matching;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex);
        for (auto &entry : callbacks) {
            if (entry.first == event) {
                matching.push_back(entry.second);
            }
        }
    }
    for (auto &callback : matching) {
        callback(event, info);
    }
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    WiFi.handle_event(id, data, base == IP_EVENT);
}
}  // namespace

void WiFiClass::handle_event(int32_t id, void *data, bool ip_event)
{
    WiFiEventInfo_t info{};
    if (ip_event) {
        if (id == IP_EVENT_STA_GOT_IP) {
            auto *got = static_cast<ip_event_got_ip_t *>(data);
            sta_ip = got->ip_info.ip.addr;
            sta_status = WL_CONNECTED;
            ESP_LOGI(TAG, "station has an address");
            dispatch(ARDUINO_EVENT_WIFI_STA_GOT_IP, info);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            sta_ip = 0;
        }
        return;
    }
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (sta_wanted) {
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_CONNECTED:
        dispatch(ARDUINO_EVENT_WIFI_STA_CONNECTED, info);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
        info.wifi_sta_disconnected = *event;
        sta_ip = 0;
        sta_status = event->reason == WIFI_REASON_NO_AP_FOUND ? WL_NO_SSID_AVAIL
                     : sta_status == WL_CONNECTED                ? WL_CONNECTION_LOST
                                                                 : WL_DISCONNECTED;
        ESP_LOGW(TAG, "station disconnected, reason %u", unsigned(event->reason));
        dispatch(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info);
        if (sta_wanted && auto_reconnect_) {
            esp_wifi_connect();
        }
        break;
    }
    default:
        break;
    }
}

bool WiFiClass::init()
{
    std::call_once(init_once, [] {
        if (esp_netif_init() != ESP_OK) {
            return;
        }
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
            return;
        }
        sta_netif = esp_netif_create_default_wifi_sta();
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        if (!sta_netif || esp_wifi_init(&config) != ESP_OK) {
            ESP_LOGE(TAG, "driver initialization failed");
            return;
        }
        // Credentials come from Vibe Mode shared information or the app; never
        // persist a second copy in the driver's own NVS namespace.
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, nullptr);
        esp_wifi_set_mode(WIFI_MODE_STA);
        init_ok = esp_wifi_start() == ESP_OK;
        esp_wifi_set_ps(WIFI_PS_NONE);  // see setSleep()
    });
    return init_ok;
}

wl_status_t WiFiClass::begin(const char *ssid, const char *password)
{
    if (!init() || !ssid || !ssid[0] || strlen(ssid) > 32) {
        return WL_CONNECT_FAILED;
    }
    wifi_config_t config{};
    // Same network as the one already joined: keep the association.
    if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && sta_wanted &&
        !strncmp(reinterpret_cast<const char *>(config.sta.ssid), ssid, sizeof(config.sta.ssid)) &&
        !strncmp(reinterpret_cast<const char *>(config.sta.password), password ? password : "",
                 sizeof(config.sta.password))) {
        return status();
    }
    config = {};
    strlcpy(reinterpret_cast<char *>(config.sta.ssid), ssid, sizeof(config.sta.ssid));
    if (password) {
        strlcpy(reinterpret_cast<char *>(config.sta.password), password, sizeof(config.sta.password));
    }
    config.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    sta_wanted = true;
    esp_wifi_disconnect();
    sta_status = WL_DISCONNECTED;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station config: %s", esp_err_to_name(err));
        return WL_CONNECT_FAILED;
    }
    esp_wifi_connect();
    return status();
}

wl_status_t WiFiClass::status()
{
    return wl_status_t(sta_status.load());
}

bool WiFiClass::mode(wifi_mode_t mode)
{
    if (!init()) {
        return false;
    }
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        if (!ap_netif) {
            ap_netif = esp_netif_create_default_wifi_ap();
        }
    }
    // Every application in this firmware relies on the station: keep it.
    if (mode == WIFI_MODE_AP) {
        mode = WIFI_MODE_APSTA;
    }
    if (mode == WIFI_MODE_NULL) {
        mode = WIFI_MODE_STA;
    }
    return esp_wifi_set_mode(mode) == ESP_OK;
}

wifi_mode_t WiFiClass::getMode()
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    return mode;
}

bool WiFiClass::disconnect(bool)
{
    // Glimdock disconnects only to rejoin with new settings (begin() follows).
    if (!init()) {
        return false;
    }
    sta_wanted = false;
    sta_ip = 0;
    sta_status = WL_DISCONNECTED;
    return esp_wifi_disconnect() == ESP_OK;
}

bool WiFiClass::reconnect()
{
    if (!init() || !sta_wanted) {
        return false;
    }
    esp_wifi_disconnect();
    return esp_wifi_connect() == ESP_OK;
}

bool WiFiClass::setSleep(bool)
{
    // Glimdock enables modem sleep for Wi-Fi/BLE coexistence on the ESP32-S3.
    // There is no BLE here, and on the ESP32-S31 (pinned IDF) waking from modem
    // sleep aborts in phy_track_pll_init. Keep power save off.
    return init() && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
}

IPAddress WiFiClass::localIP()
{
    return IPAddress(sta_ip.load());
}

String WiFiClass::SSID()
{
    wifi_ap_record_t record{};
    if (status() != WL_CONNECTED || esp_wifi_sta_get_ap_info(&record) != ESP_OK) {
        return String();
    }
    return String(reinterpret_cast<const char *>(record.ssid));
}

int8_t WiFiClass::RSSI()
{
    wifi_ap_record_t record{};
    return status() == WL_CONNECTED && esp_wifi_sta_get_ap_info(&record) == ESP_OK ? record.rssi : 0;
}

bool WiFiClass::softAP(const char *ssid, const char *password)
{
    if (!ssid || !mode(WIFI_MODE_APSTA)) {
        return false;
    }
    wifi_config_t config{};
    strlcpy(reinterpret_cast<char *>(config.ap.ssid), ssid, sizeof(config.ap.ssid));
    config.ap.ssid_len = uint8_t(strlen(reinterpret_cast<const char *>(config.ap.ssid)));
    config.ap.max_connection = 2;
    if (password && strlen(password) >= 8) {
        strlcpy(reinterpret_cast<char *>(config.ap.password), password, sizeof(config.ap.password));
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        config.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &config);
    memset(&config, 0, sizeof(config));
    return err == ESP_OK;
}

bool WiFiClass::softAPdisconnect(bool)
{
    return init() && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
}

IPAddress WiFiClass::softAPIP()
{
    esp_netif_ip_info_t info{};
    if (!ap_netif || getMode() != WIFI_MODE_APSTA || esp_netif_get_ip_info(ap_netif, &info) != ESP_OK) {
        return IPAddress();
    }
    return IPAddress(info.ip.addr);
}

void WiFiClass::onEvent(WiFiEventCb callback, WiFiEvent_t event)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex);
    callbacks.emplace_back(event, std::move(callback));
}
