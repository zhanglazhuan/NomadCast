idf_component_register(
    SRCS "t_flashdb.c"
    INCLUDE_DIRS "." "../../../boards/esp32s3"
    REQUIRES fatfs esp_driver_sdmmc esp_timer flashdb
)
