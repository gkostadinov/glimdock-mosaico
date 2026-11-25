# The board's NAND: the LittleFS volume Vibe Mode keeps (Ideas, downloads),
# mounted at /nand for photos and files. Found and checked read-only first;
# never formatted.
idf_component_register(
    SRCS "src/mosaico_storage.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp-mosaico-bsp espressif__spi_nand_flash joltwallet__littlefs esp_blockdev esp_driver_spi
                  esp_driver_gpio log)
