idf_component_register(SRCS "t_wakeup.c"
                    INCLUDE_DIRS "." "../../../boards/esp32s3"
                    REQUIRES driver esp_adc esp_lcd esp_hw_support)
