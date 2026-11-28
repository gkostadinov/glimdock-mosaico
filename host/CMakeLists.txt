# Host preview: renders the launcher, Glimdock's actual firmware UI (scaled by
# glim_scale.h) and Swifty Home at 480x480 through LVGL's software renderer,
# with the device's own LVGL Kconfig. See render.py.
cmake_minimum_required(VERSION 3.20)
project(mosaico_preview C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

set(IDEA "${CMAKE_CURRENT_LIST_DIR}/..")
set(LVGL "${IDEA}/managed_components/lvgl__lvgl")
# Glimdock's firmware sources (the vendored copy unless GLIMDOCK_FIRMWARE_DIR
# names a live checkout's firmware/) and a Glimdock checkout for its native
# preview shims (GLIMDOCK_DIR: https://github.com/gkostadinov/glimdock).
if(DEFINED ENV{GLIMDOCK_FIRMWARE_DIR})
    set(GLIMDOCK_FIRMWARE_DIR "$ENV{GLIMDOCK_FIRMWARE_DIR}")
elseif(NOT GLIMDOCK_FIRMWARE_DIR)
    set(GLIMDOCK_FIRMWARE_DIR "${IDEA}/third_party/glimdock/firmware")
endif()
if(NOT GLIMDOCK_DIR AND DEFINED ENV{GLIMDOCK_DIR})
    set(GLIMDOCK_DIR "$ENV{GLIMDOCK_DIR}")
endif()
if(NOT EXISTS "${GLIMDOCK_DIR}/tools/native-preview/shims")
    message(FATAL_ERROR "Set GLIMDOCK_DIR to a Glimdock checkout (https://github.com/gkostadinov/glimdock)")
endif()
set(NATIVE "${GLIMDOCK_DIR}/tools/native-preview")
set(ARDUINOJSON "${IDEA}/managed_components/bblanchon__arduinojson/src" CACHE PATH "")
if(NOT EXISTS "${IDEA}/build/config/sdkconfig.h")
    message(FATAL_ERROR "Build the device project once (idf.py app) to generate build/config/sdkconfig.h")
endif()

file(GLOB_RECURSE lvgl_sources "${LVGL}/src/*.c")
add_library(lvgl STATIC ${lvgl_sources})
target_include_directories(lvgl PUBLIC "${LVGL}" "${LVGL}/src" "${CMAKE_CURRENT_LIST_DIR}" "${IDEA}/build/config" "${IDEA}/config")
target_compile_definitions(lvgl PUBLIC "LV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"host_kconfig.h\"")
target_compile_options(lvgl PRIVATE -w)

# TTF files as C arrays (the device embeds them with EMBED_FILES).
function(embed_font path symbol)
    file(READ "${path}" hex HEX)
    string(LENGTH "${hex}" length)
    math(EXPR bytes "${length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    file(WRITE "${CMAKE_BINARY_DIR}/${symbol}.c"
        "#include <stddef.h>\n#include <stdint.h>\nconst uint8_t ${symbol}[] = {${hex}};\nconst size_t ${symbol}_size = ${bytes};\n")
endfunction()
set(font_sources "")
embed_font("${IDEA}/components/mosaico_ui/assets/Inter-Regular.ttf" inter_regular_ttf)
embed_font("${IDEA}/components/mosaico_ui/assets/Inter-SemiBold.ttf" inter_semibold_ttf)
list(APPEND font_sources "${CMAKE_BINARY_DIR}/inter_regular_ttf.c" "${CMAKE_BINARY_DIR}/inter_semibold_ttf.c")

set(HOME_SOURCES "${IDEA}/components/home_app/src/home_ui.cpp" "${IDEA}/components/home_app/src/home_hub.cpp"
    "${IDEA}/components/home_app/src/home_model.cpp" "${CMAKE_CURRENT_LIST_DIR}/home_platform_host.cpp")
add_executable(mosaico-preview
    preview_main.cpp
    "${IDEA}/components/glimdock_port/src/glim_ui.cpp"
    "${CMAKE_BINARY_DIR}/demo_snapshot.cpp"
    "${IDEA}/components/mosaico_shell/src/launcher_ui.cpp"
    "${IDEA}/components/mosaico_shell/src/launcher_apps.cpp"
    "${IDEA}/components/mosaico_shell/src/launcher_layout.cpp"
    "${IDEA}/components/mosaico_shell/src/launcher_widgets.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/weather_host.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/reminders_host.cpp"
    "${IDEA}/components/mosaico_ui/src/mui.cpp"
    "${IDEA}/components/mosaico_ui/src/keyboard.cpp"
    "${IDEA}/components/glimdock_port/src/glimdock_summary.cpp"
    "${IDEA}/components/swifty_app/src/swifty_figure.cpp"
    "${IDEA}/components/swifty_app/src/swifty_paseo_ui.cpp"
    "${IDEA}/components/tools_app/src/tools_app.cpp"
    "${IDEA}/components/games_app/src/games_ui.cpp"
    "${IDEA}/components/games_app/src/games_common.cpp"
    "${IDEA}/components/games_app/src/games_3d.cpp"
    "${IDEA}/components/games_app/src/game_crypt.cpp"
    "${IDEA}/components/games_app/src/game_starbore.cpp"
    "${IDEA}/components/games_app/src/game_skyglide.cpp"
    "${IDEA}/components/games_app/src/game_jellystorm.cpp"
    "${IDEA}/components/games_app/src/game_pinball.cpp"
    "${IDEA}/components/games_app/src/game_rally.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/interact_host.cpp"
    "${IDEA}/components/vpn_app/src/vpn_ui.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/vpn_host.cpp"
    "${IDEA}/components/mosaico_shell/src/wifi_settings.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/board_wifi_host.cpp"
    "${IDEA}/components/face_id/src/face_preview.cpp"
    "${IDEA}/components/face_id/host/face_id_host.cpp"
    ${font_sources}
    ${HOME_SOURCES})
target_include_directories(mosaico-preview PRIVATE
    "${NATIVE}/shims" "${NATIVE}" "${GLIMDOCK_FIRMWARE_DIR}/src" "${GLIMDOCK_FIRMWARE_DIR}/include"
    "${IDEA}/components/glimdock_port/include" "${IDEA}/components/glimdock_port/src"
    "${IDEA}/components/mosaico_shell/src"
    "${IDEA}/components/home_app/include" "${IDEA}/components/home_app/src" "${IDEA}/components/camera_app/include"
    "${IDEA}/components/swifty_app/include" "${IDEA}/components/swifty_app/src"
    "${IDEA}/components/mosaico_ui/include"
    "${IDEA}/components/tools_app/include" "${IDEA}/components/files_app/include"
    "${IDEA}/components/vpn_app/include"
    "${IDEA}/components/face_id/include" "${IDEA}/components/face_id/src"
    "${IDEA}/components/games_app/include" "${IDEA}/components/games_app/src"
    "${CMAKE_CURRENT_LIST_DIR}/shims"
    "${ARDUINOJSON}" "${CMAKE_BINARY_DIR}")
target_compile_definitions(mosaico-preview PRIVATE
    HOMELAB_NATIVE_PREVIEW=1 HOMELAB_ROTATION=3 GLIMDOCK_PUBLIC_FIRMWARE HOMELAB_NO_FLIPPER HOMELAB_POWER_BUTTON=0 HOMELAB_SETUP_PORTAL=0
    "HOMELAB_BOARD_NAME=\"ESP-Mosaico (ESP32-S31)\""
    "HOMELAB_BATTERY_NOTE=\"BQ27220 fuel gauge \\xE2\\x80\\xA2 cell voltage\""
    "HOMELAB_BRAND_ASSETS=\"brand_assets_mosaico.h\""
    ARDUINOJSON_ENABLE_ARDUINO_STRING=0 ARDUINOJSON_ENABLE_ARDUINO_STREAM=0)
find_package(Threads REQUIRED)
target_link_libraries(mosaico-preview PRIVATE lvgl Threads::Threads)

# Host tests for the home screen's layout: its text, the grid's packing and
# the migration from before widgets were items (run build-host/launcher-layout-test).
add_executable(launcher-layout-test launcher_layout_test.cpp "${IDEA}/components/mosaico_shell/src/launcher_layout.cpp")
target_include_directories(launcher-layout-test PRIVATE "${IDEA}/components/mosaico_shell/src")

# The shortcuts' screens over a stand-in engine, and the host tests of their
# programs (run build-host/shortcuts-test).
target_sources(mosaico-preview PRIVATE
    "${IDEA}/components/mosaico_shell/src/shortcuts_ui.cpp"
    "${IDEA}/components/mosaico_shell/src/shortcuts_program.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shortcuts_host.cpp")
add_executable(shortcuts-test shortcuts_test.cpp "${IDEA}/components/mosaico_shell/src/shortcuts_program.cpp")
target_include_directories(shortcuts-test PRIVATE "${IDEA}/components/mosaico_shell/src" "${ARDUINOJSON}")
target_compile_definitions(shortcuts-test PRIVATE ARDUINOJSON_ENABLE_ARDUINO_STRING=0 ARDUINOJSON_ENABLE_ARDUINO_STREAM=0)
