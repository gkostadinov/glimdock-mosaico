# Games: Ember Crypt, Starbore and Skyglide, three retro 3D games drawn in software.
idf_component_register(
    SRCS "src/games_ui.cpp" "src/games_common.cpp" "src/game_crypt.cpp" "src/game_starbore.cpp" "src/game_skyglide.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES lvgl
    PRIV_REQUIRES mosaico_ui nvs_flash log esp_timer)
# The games' drawing is the hot loop: full optimisation, relaxed float rules.
set_source_files_properties(src/games_common.cpp src/game_crypt.cpp src/game_starbore.cpp src/game_skyglide.cpp
    PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
