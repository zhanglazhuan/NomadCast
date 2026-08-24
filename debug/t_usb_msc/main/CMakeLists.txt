idf_component_register(
    SRCS "t_usb_msc.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_sdmmc sdmmc fatfs
)
