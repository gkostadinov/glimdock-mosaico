# Games: six retro 3D games drawn in software on one small engine (games_3d.h).
idf_component_register(
    SRCS "src/games_ui.cpp" "src/games_common.cpp" "src/games_3d.cpp" "src/game_crypt.cpp" "src/game_starbore.cpp" "src/game_skyglide.cpp"
         "src/game_jellystorm.cpp" "src/game_pinball.cpp" "src/game_rally.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES lvgl
    PRIV_REQUIRES mosaico_ui nvs_flash log esp_timer)
# The games' drawing is the hot loop: full optimisation, relaxed float rules.
set_source_files_properties(src/games_common.cpp src/games_3d.cpp src/game_crypt.cpp src/game_starbore.cpp src/game_skyglide.cpp
    src/game_jellystorm.cpp src/game_pinball.cpp src/game_rally.cpp
    PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
