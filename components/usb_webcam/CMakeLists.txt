# USB webcam mode: while it is on, ESP-Iris steps aside and the board's USB
# port is a UVC camera (MJPEG) built on the same TinyUSB/esp_tinyusb stack.
idf_component_register(
    SRCS "src/usb_webcam.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__esp_tinyusb espressif__tinyusb esp_iris esp_timer esp_hw_support log)
# TinyUSB's isochronous endpoint macro ORs two enum types (an error in C++26).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-enum-enum-conversion)

# TinyUSB's video class, in the one TinyUSB build ESP-Iris also uses (its
# esp_tinyusb configuration has no video option; it leaves these undefined).
# PUBLIC: every user of TinyUSB must see the same class layout.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_compile_definitions(${tusb_lib} PUBLIC
    CFG_TUD_VIDEO=1
    CFG_TUD_VIDEO_STREAMING=1
    CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE=1024)
