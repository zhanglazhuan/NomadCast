idf_component_register(
    SRCS "sleep_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl driver input esp_timer esp_lcd
)
