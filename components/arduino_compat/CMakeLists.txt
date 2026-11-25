idf_component_register(
    SRCS "src/arduino_core.cpp" "src/preferences.cpp" "src/wifi.cpp"
         "src/wifi_client.cpp" "src/http_client.cpp" "src/web_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp-tls nvs_flash lwip esp_timer freertos esp_hw_support
    PRIV_REQUIRES log esp_iris)
