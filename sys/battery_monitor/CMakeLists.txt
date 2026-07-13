idf_component_register(
    SRCS "battery.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl events driver esp_adc
)
