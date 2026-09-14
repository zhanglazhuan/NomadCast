idf_component_register(
    SRCS
        "app.c" "model.c" "view.c" "controller.c" "weather_api.c"
        "subpages/view_current.c" "subpages/view_city.c"
    INCLUDE_DIRS "." "subpages"
    REQUIRES lvgl app_manager uilv lang flash_store events clock esp_http_client mbedtls json esp_timer zlib
)
