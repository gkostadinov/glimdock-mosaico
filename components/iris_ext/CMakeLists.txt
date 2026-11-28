# One ESP-Iris RPC that carries many (iris_ext.h).
idf_component_register(
    SRCS "src/iris_ext.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_iris
    PRIV_REQUIRES log)
