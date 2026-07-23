idf_component_register(
    SRCS "log_system.c" "log_capture.c" "log_uploader.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl events clock esp_http_client
)
