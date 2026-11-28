# Glimdock's firmware sources: the copy in third_party/glimdock (see its
# README), or a live Glimdock checkout's firmware/ folder named by the
# GLIMDOCK_FIRMWARE_DIR environment variable.
if(DEFINED ENV{GLIMDOCK_FIRMWARE_DIR})
    set(GLIMDOCK_FIRMWARE_DIR "$ENV{GLIMDOCK_FIRMWARE_DIR}")
elseif(NOT GLIMDOCK_FIRMWARE_DIR)
    set(GLIMDOCK_FIRMWARE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/glimdock/firmware")
endif()

idf_component_register(
    SRCS "src/shell.cpp" "src/launcher_ui.cpp" "src/launcher_apps.cpp" "src/launcher_layout.cpp" "src/launcher_widgets.cpp" "src/weather.cpp" "src/screenshot.cpp" "src/haptics.cpp" "src/boot_stage.c" "src/orientation.cpp" "src/perf.cpp" "src/frame_sync.cpp" "src/time_zone.cpp" "src/web_settings.cpp" "src/reminders.cpp" "src/always_on.cpp" "src/shortcuts.cpp" "src/board_wifi.cpp" "src/wifi_settings.cpp"
         "src/shortcuts_program.cpp" "src/shortcuts_ui.cpp" "src/shortcuts_policy.cpp"
    INCLUDE_DIRS "include"
    # display_clock.h: Glimdock's time-zone rules, shared with the launcher.
    PRIV_INCLUDE_DIRS "src" "${GLIMDOCK_FIRMWARE_DIR}/src"
    # The page of the web settings (web_settings.cpp).
    EMBED_TXTFILES "src/web_settings.html"
    REQUIRES esp_common esp_iris
    PRIV_REQUIRES lvgl mosaico_ui iris_ext camera_app swifty_app files_app tools_app vpn_app games_app mosaico_storage arduino_compat glimdock_port home_app app_config esp-mosaico-bsp esp_iris esp_app_format
        button mbedtls bblanchon__arduinojson log esp_driver_i2c esp_timer esp_mosaico_app_recovery esp_lvgl_adapter nvs_flash esp_wifi esp_event esp_driver_gpio esp_lcd esp_http_client esp_http_server usb_webcam espressif__esp_tinyusb espressif__tinyusb)

# Keep ESP-IDF's shared certificate bundle when one HTTPS connection closes
# (see __wrap_esp_crt_bundle_detach in shell.cpp).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_crt_bundle_detach")
