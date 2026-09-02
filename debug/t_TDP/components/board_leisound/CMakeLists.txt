idf_component_register(SRCS "board_leisound.c"
                       INCLUDE_DIRS "."
                       REQUIRES es8156 driver freertos vfs sdmmc fatfs)
