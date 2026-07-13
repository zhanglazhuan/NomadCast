idf_component_register(
    SRCS "lvgl_port_bsp.cpp"
    REQUIRES lvgl esp_lcd driver touch_bsp i2c_bsp
    INCLUDE_DIRS ".")
