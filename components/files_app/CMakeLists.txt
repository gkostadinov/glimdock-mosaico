# Files: the board's NAND (photos and files) and SMB shares, with image and
# text previews.
idf_component_register(
    SRCS "src/files_app.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES mosaico_ui mosaico_storage libsmb2 esp_iris esp_timer lwip nvs_flash log espressif__esp_new_jpeg espressif__libpng
                  bblanchon__arduinojson)
