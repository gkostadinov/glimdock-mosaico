# USB webcam mode: while it is on, ESP-Iris steps aside and the board's USB
# port is a UVC camera (MJPEG) and a USB microphone (UAC2), built on the same
# TinyUSB/esp_tinyusb stack.
idf_component_register(
    SRCS "src/usb_webcam.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__esp_tinyusb espressif__tinyusb esp_iris esp_timer esp_hw_support log board_mic esp-mosaico-bsp
                  espressif__esp_codec_dev)
# TinyUSB's isochronous endpoint macro ORs two enum types (an error in C++26).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-enum-enum-conversion)

# TinyUSB's video and audio classes, in the one TinyUSB build ESP-Iris also
# uses (its esp_tinyusb configuration has neither; it leaves these undefined).
# Audio: one microphone endpoint, 1 ms packets of 48 kHz 16-bit mono with one
# sample over (98 bytes), and a 2 KB buffer (about 21 ms) between the reads
# and the packets.
# PUBLIC: every user of TinyUSB must see the same class layout.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_compile_definitions(${tusb_lib} PUBLIC
    CFG_TUD_VIDEO=1
    CFG_TUD_VIDEO_STREAMING=1
    CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE=1024
    CFG_TUD_AUDIO=1
    CFG_TUD_AUDIO_ENABLE_EP_IN=1
    CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX=98
    CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ=2048)
