# Face ID: on-device face enrollment and recognition with the camera module
# (ESP-DL's face detector and MobileFaceNet). Face data never leaves the board.
# WHOLE_ARCHIVE: the profiles component ships weak stand-ins for this API; every
# object here must be linked so the strong definitions replace them all (an
# archive member is otherwise pulled only for a symbol nothing defines yet).
idf_component_register(
    SRCS "src/face_id.cpp" "src/face_preview.cpp" "src/face_match.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES lvgl
    PRIV_REQUIRES camera_app mosaico_ui esp-mosaico-bsp esp_iris iris_ext nvs_flash esp_timer heap log
                  espressif__human_face_detect espressif__human_face_recognition
    WHOLE_ARCHIVE)

# ESP-DL compiles all of its ~80 operators unless a component lists the ones
# its models use, and the face model packages list none: attention, rotary
# embeddings, LSTMs and the rest cost ~0.5 MB of code and 8 KB of internal RAM.
# The three models' graphs hold only Conv (Relu fused into it), Add, Concat
# and PRelu; the requantize ops and Relu are kept as cheap insurance. Every
# Conv kernel stays (all of kernels.yml): which one a layer picks is decided
# at run time, and a missing one would only show on the board.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("espressif__esp-dl" IN_LIST build_components)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
    include("${espdl_dir}/cmake/compile_register.cmake")
    file(STRINGS "${espdl_dir}/spec/kernels.yml" kernel_lines REGEX "^  - ")
    set(req "kernel_abi: 1\nops:\n")
    foreach(op Conv Add Concat PRelu Relu QuantizeLinear DequantizeLinear RequantizeLinear)
        string(APPEND req "  - ${op}\n")
    endforeach()
    string(APPEND req "kernels:\n")
    foreach(line ${kernel_lines})
        string(APPEND req "${line}\n")
    endforeach()
    set(req_file "${CMAKE_BINARY_DIR}/face_id/face_models.compile_req.yml")
    file(WRITE "${req_file}" "${req}")
    esp_dl_compile_add_yml("${req_file}")
endif()
