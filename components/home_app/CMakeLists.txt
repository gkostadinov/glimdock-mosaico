# Glimdock's firmware sources: the copy in third_party/glimdock (see its
# README), or a live Glimdock checkout's firmware/ folder named by the
# GLIMDOCK_FIRMWARE_DIR environment variable.
if(DEFINED ENV{GLIMDOCK_FIRMWARE_DIR})
    set(GLIMDOCK_FIRMWARE_DIR "$ENV{GLIMDOCK_FIRMWARE_DIR}")
elseif(NOT GLIMDOCK_FIRMWARE_DIR)
    set(GLIMDOCK_FIRMWARE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/glimdock/firmware")
endif()

idf_component_register(
    SRCS "src/home_ui.cpp" "src/home_hub.cpp" "src/home_model.cpp" "src/home_platform_idf.cpp"
    INCLUDE_DIRS "include"
    # display_clock.h: the clock zone shared with Glimdock.
    PRIV_INCLUDE_DIRS "src" "${GLIMDOCK_FIRMWARE_DIR}/src"
    REQUIRES lvgl
    PRIV_REQUIRES mosaico_ui esp_http_client mbedtls nvs_flash esp_timer pthread log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-enum-enum-conversion)
