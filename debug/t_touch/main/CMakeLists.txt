idf_component_register(SRCS "t_touch.c" "gt911.c"
                    INCLUDE_DIRS "." "../../../boards/esp32s3"
                    REQUIRES driver esp_timer)
