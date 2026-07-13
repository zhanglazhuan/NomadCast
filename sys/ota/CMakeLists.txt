idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_https_ota esp_http_client json nvs_flash app_update esp_event
)
