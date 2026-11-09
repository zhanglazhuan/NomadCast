idf_component_register(
    SRCS "sleep_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES controller lvgl driver input esp_timer esp_lcd
)
