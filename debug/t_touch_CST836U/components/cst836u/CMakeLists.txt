# Compile the reference module directly, without changing its source or headers.
get_filename_component(REFERENCE_CST836U
    "${CMAKE_CURRENT_LIST_DIR}/../../../../refer/read-pico-reader-main/components/cst836u" ABSOLUTE)
idf_component_register(
    SRCS "${REFERENCE_CST836U}/cst836u.c"
    INCLUDE_DIRS "${REFERENCE_CST836U}/include"
    REQUIRES esp_driver_gpio esp_driver_i2c
    PRIV_REQUIRES freertos)
