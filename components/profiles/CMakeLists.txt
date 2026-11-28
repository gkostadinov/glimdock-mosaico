# Profiles: who uses the board and what they may do (profiles.h). The model
# is plain C++ (face enrolment reaches it through profiles_set_face_hooks);
# the shell draws its screens (profiles_ui.cpp) and gates the apps and
# Swifty's tools with it. Host tests: test/.
idf_component_register(
    SRCS "src/profiles.cpp" "src/profiles_platform.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES nvs_flash mbedtls esp_timer esp_hw_support log)
