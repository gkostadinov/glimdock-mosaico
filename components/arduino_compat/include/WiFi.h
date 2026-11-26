// SPDX-License-Identifier: Apache-2.0
// Arduino WiFi facade over esp_wifi/esp_netif. The station is shared by every
// application in this firmware; credentials live only in RAM (WIFI_STORAGE_RAM).
#pragma once

#include <functional>

#include "Arduino.h"
#include "IPAddress.h"
#include "WiFiClient.h"
#include "esp_wifi_types.h"

typedef enum {
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL = 1,
    WL_CONNECTED = 3,
    WL_CONNECT_FAILED = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6,
} wl_status_t;

#define WIFI_OFF WIFI_MODE_NULL
#define WIFI_STA WIFI_MODE_STA
#define WIFI_AP WIFI_MODE_AP
#define WIFI_AP_STA WIFI_MODE_APSTA

typedef enum {
    ARDUINO_EVENT_WIFI_STA_CONNECTED,
    ARDUINO_EVENT_WIFI_STA_DISCONNECTED,
    ARDUINO_EVENT_WIFI_STA_GOT_IP,
} WiFiEvent_t;

typedef union {
    wifi_event_sta_disconnected_t wifi_sta_disconnected;
} WiFiEventInfo_t;

using WiFiEventCb = std::function<void(WiFiEvent_t, WiFiEventInfo_t)>;

class WiFiClass {
public:
    // Brings up the driver once; safe to call repeatedly from any task.
    bool init();
    wl_status_t begin(const char *ssid, const char *password = nullptr);
    wl_status_t status();
    bool mode(wifi_mode_t mode);
    wifi_mode_t getMode();
    bool disconnect(bool wifi_off = false);
    bool reconnect();
    bool persistent(bool)
    {
        return true;    // always RAM-only
    }
    bool setAutoReconnect(bool enabled)
    {
        auto_reconnect_ = enabled;
        return true;
    }
    bool setSleep(bool enabled);
    IPAddress localIP();
    String SSID();
    int8_t RSSI();
    bool softAP(const char *ssid, const char *password = nullptr);
    bool softAPdisconnect(bool wifi_off = false);
    IPAddress softAPIP();
    void onEvent(WiFiEventCb callback, WiFiEvent_t event);

    // Internal: driver event dispatch.
    void handle_event(int32_t id, void *data, bool ip_event);

private:
    bool auto_reconnect_ = true;
};
extern WiFiClass WiFi;
