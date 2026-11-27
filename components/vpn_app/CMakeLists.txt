# VPN: a WireGuard tunnel (esphome/wireguard) that carries all of the board's
# internet traffic while it is on, with its screen and settings.
idf_component_register(
    SRCS "src/vpn_app.cpp" "src/vpn_config.cpp" "src/vpn_tunnel.cpp" "src/vpn_ui.cpp" "src/vpn_probe.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES mosaico_ui esp_netif esp_event esp_wifi esp_timer lwip nvs_flash log bblanchon__arduinojson
                  esphome__wireguard esphome__libsodium esp_iris esp_http_client mbedtls)

# The WireGuard library keeps one allowed network per peer unless told more:
# the board's own address plus up to six AllowedIPs (vpn_config.h MAX_ALLOWED).
# PUBLIC: this component includes wireguard.h too, and the peer struct's size
# depends on it (read with the default, every field after it was off).
idf_component_get_property(wireguard_lib esphome__wireguard COMPONENT_LIB)
if(TARGET ${wireguard_lib})
    target_compile_definitions(${wireguard_lib} PUBLIC CONFIG_WIREGUARD_MAX_SRC_IPS=8)
else()
    message(FATAL_ERROR "vpn_app: esphome__wireguard is not built yet; set its options from the project instead")
endif()
