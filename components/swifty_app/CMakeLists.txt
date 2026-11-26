# Swifty: Swifty Home's bird in 3D, chat with a local model, push-to-talk ASR.
idf_component_register(
    SRCS "src/swifty_figure.cpp" "src/swifty_config.cpp" "src/swifty_asr.cpp" "src/swifty_agent.cpp"
         "src/swifty_md.cpp" "src/swifty_ui.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES lvgl
    PRIV_REQUIRES mosaico_ui camera_app home_app esp-mosaico-bsp esp_iris nvs_flash esp_http_client esp_timer mbedtls log
                  espressif__esp_websocket_client espressif__esp_new_jpeg espressif__libpng bblanchon__arduinojson
                  espressif__esp_codec_dev tcp_transport)
# The rasterizer is the hot loop: full optimisation, relaxed float rules.
set_source_files_properties(src/swifty_figure.cpp PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
# The speech socket's connects run in a helper task (swifty_asr.cpp).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_transport_connect")
