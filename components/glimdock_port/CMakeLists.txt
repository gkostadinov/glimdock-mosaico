# Glimdock's data layer (network.cpp), built unchanged from the Glimdock
# firmware, under a UI written for the Mosaico panel (src/glim_ui.cpp).
# Glimdock's firmware sources: the copy in third_party/glimdock (see its
# README), or a live Glimdock checkout's firmware/ folder named by the
# GLIMDOCK_FIRMWARE_DIR environment variable.
if(DEFINED ENV{GLIMDOCK_FIRMWARE_DIR})
    set(GLIMDOCK_FIRMWARE_DIR "$ENV{GLIMDOCK_FIRMWARE_DIR}")
elseif(NOT GLIMDOCK_FIRMWARE_DIR)
    set(GLIMDOCK_FIRMWARE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/glimdock/firmware")
endif()
if(NOT EXISTS "${GLIMDOCK_FIRMWARE_DIR}/src/network.cpp")
    message(FATAL_ERROR "Glimdock firmware not found at ${GLIMDOCK_FIRMWARE_DIR}; set the GLIMDOCK_FIRMWARE_DIR environment variable")
endif()

set(firmware_sources "${GLIMDOCK_FIRMWARE_DIR}/src/network.cpp")

idf_component_register(
    SRCS ${firmware_sources}
         "src/glim_ui.cpp" "src/iris_serial.cpp" "src/glimdock_app.cpp" "src/glimdock_summary.cpp" "src/board_mosaico.cpp"
         "src/audio_mosaico.cpp" "src/runtime_health_mosaico.cpp" "src/lv_mem_psram.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${GLIMDOCK_FIRMWARE_DIR}/src" "${GLIMDOCK_FIRMWARE_DIR}/include"
    REQUIRES lvgl__lvgl mosaico_ui
    PRIV_REQUIRES arduino_compat esp_iris esp-mosaico-bsp esp_codec_dev esp_driver_i2c mbedtls esp_timer log)

# Board selection for the shared firmware sources (see firmware/include/config.h).
set(board_definitions
    GLIMDOCK_PUBLIC_FIRMWARE
    HOMELAB_PLATFORM_WIFI
    HOMELAB_NO_FLIPPER
    HOMELAB_ROTATION=3
    HOMELAB_POWER_BUTTON=0
    HOMELAB_SETUP_PORTAL=0
    HOMELAB_NETWORK_STACK=28672
    "HOMELAB_BOARD_NAME=\"ESP-Mosaico (ESP32-S31)\""
    "HOMELAB_BATTERY_NOTE=\"BQ27220 fuel gauge \\xE2\\x80\\xA2 cell voltage\""
    "HOMELAB_BRAND_ASSETS=\"brand_assets_mosaico.h\""
    ARDUINOJSON_ENABLE_ARDUINO_STRING=1
    ARDUINOJSON_ENABLE_ARDUINO_STREAM=0
    ARDUINOJSON_ENABLE_PROGMEM=0)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${board_definitions})
set_source_files_properties(${firmware_sources} PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-function;-Wno-misleading-indentation;-Wno-deprecated-enum-enum-conversion;-Wno-missing-field-initializers;-Wno-format-truncation;-Wno-stringop-truncation")
# LVGL (CONFIG_LV_USE_CUSTOM_MALLOC) links after this component: force the
# PSRAM allocator object in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
# glim_ui.cpp includes the firmware's compact headers (one-line if/return).
set_source_files_properties("src/glim_ui.cpp" "src/glimdock_app.cpp" PROPERTIES COMPILE_OPTIONS "-Wno-misleading-indentation")
