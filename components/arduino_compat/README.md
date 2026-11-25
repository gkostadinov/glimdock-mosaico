# arduino_compat

The slice of the Arduino ESP32 core that the Glimdock firmware sources use,
implemented on ESP-IDF so those sources build unchanged for ESP-Mosaico:

| Header | Backed by |
| --- | --- |
| `Arduino.h` (`String`, `Print`/`Stream`, `Serial`, `millis`, `configTime`) | `std::string`, esp_timer, console stdio, esp_netif SNTP |
| `Preferences.h` | NVS (default `nvs` partition), Arduino return conventions |
| `WiFi.h` | esp_wifi/esp_netif; one shared station, `WIFI_STORAGE_RAM` |
| `WiFiClient.h` / `WiFiClientSecure.h` | lwIP sockets / esp-tls with a mandatory pinned CA |
| `HTTPClient.h` | HTTP/1.0–1.1 request writer; body left in the client stream |
| `WebServer.h` / `DNSServer.h` | polled setup portal and captive DNS |

Only calls Glimdock makes exist, so a new dependency fails to compile rather
than misbehaving. Redirects are never followed and TLS never falls back to an
insecure mode. Request buffers that carried bearer tokens or form secrets are
cleared before they are freed.
