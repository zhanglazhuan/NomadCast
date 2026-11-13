idf_component_register(SRCS "t_key.c"
                    INCLUDE_DIRS "." "../../../boards/esp32s3"
                    REQUIRES lvgl esp_lcd driver esp_timer esp_adc)
