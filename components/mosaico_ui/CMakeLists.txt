# Mosaico AMOLED UI kit (mui.h): Inter fonts, backdrops, glass, capsules.
idf_component_register(
    SRCS "src/mui.cpp" "src/keyboard.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES heap
    EMBED_FILES "assets/Inter-Regular.ttf" "assets/Inter-SemiBold.ttf")
