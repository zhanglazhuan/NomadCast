idf_component_register(SRCS "touch_controller.c" "display_controller.c" INCLUDE_DIRS "."
    REQUIRES cst836u lvgl esp_lcd esp_driver_gpio PRIV_REQUIRES esp_timer)
