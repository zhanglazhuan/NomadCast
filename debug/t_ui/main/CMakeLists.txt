idf_component_register(SRCS "t_ui.c" "ui/ui.c"
                       INCLUDE_DIRS "." "ui"
                       REQUIRES lvgl esp_lcd driver esp_timer board_ui)
