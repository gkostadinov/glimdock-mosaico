# Camera app: viewfinder, photos on the NAND (or PSRAM), gallery, Mac download over ESP-Iris.
idf_component_register(
    SRCS "src/camera_app.cpp" "src/gallery.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES mosaico_ui mosaico_module_camera esp-mosaico-bsp esp_driver_ppa esp_driver_jpeg esp_driver_gpio esp_mm esp_iris nvs_flash log mosaico_storage files_app
                  espressif__esp_new_jpeg)
