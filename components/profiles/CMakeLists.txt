# Profiles: who uses the board and what they may do (profiles.h). The model
# is plain C++; the shell draws its screens (profiles_ui.cpp) and gates the
# apps and Swifty's tools with it. Host tests: test/.
set(srcs "src/profiles.cpp" "src/profiles_platform.cpp")
set(priv_requires nvs_flash mbedtls esp_timer esp_hw_support log)
# Face sign-in comes from components/face_id; until it is in the build, weak
# stand-ins (no camera) keep this component building on its own.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../face_id/CMakeLists.txt")
    list(APPEND priv_requires face_id)
else()
    list(APPEND srcs "src/face_id_weak.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    # face_id.h hands out LVGL objects (its live preview).
    REQUIRES lvgl
    PRIV_REQUIRES ${priv_requires})
