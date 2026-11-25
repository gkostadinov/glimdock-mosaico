# Tools: torch, spirit level, compass, calculator and the Interaction sub-board.
idf_component_register(
    SRCS "src/tools_app.cpp" "src/interact.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES mosaico_ui esp-mosaico-bsp nvs_flash log esp_timer mosaico_module_interact mosaico_module_mgr)
