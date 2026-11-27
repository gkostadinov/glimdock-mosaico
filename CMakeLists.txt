cmake_minimum_required(VERSION 3.16)
# The ESP-Mosaico toolkit: this project lives in an esp-mosaico-vibe workspace's
# projects/ folder (see README.md); MOSAICO_UTILS_ROOT in the environment names
# another esp-mosaico-utils checkout.
if(DEFINED ENV{MOSAICO_UTILS_ROOT})
    set(MOSAICO_UTILS_ROOT "$ENV{MOSAICO_UTILS_ROOT}")
else()
    set(MOSAICO_UTILS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-utils")
endif()
get_filename_component(MOSAICO_UTILS_ROOT "${MOSAICO_UTILS_ROOT}" REALPATH)
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.application.defaults")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_app_recovery")
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/mosaico_idf_project.cmake")
# The toolkit's sources sit outside the project, so ESP-IDF's own path mapping
# misses them: keep the build machine's paths out of the firmware's __FILE__.
idf_build_set_property(COMPILE_OPTIONS "-fmacro-prefix-map=${MOSAICO_UTILS_ROOT}=esp-mosaico-utils" APPEND)
# LVGL's assert handler traps (config/lvgl_assert_trap.h, selected by
# CONFIG_LV_ASSERT_HANDLER_INCLUDE) instead of spinning forever.
idf_build_set_property(INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/config" APPEND)
# The CO5300 panel link stays at the BSP's 40 MHz (80 MHz drew stray lines on
# the glass, 60 MHz while scrolling).
project(glimdock VERSION 1.1.0)
# The board keeps its retained bootloader; like the toolkit's game apps, keep the
# (never installed) template bootloader out of the default build.
if(TARGET bootloader)
    set_property(TARGET bootloader PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/system_update.cmake")
