idf_component_register(
    SRCS
        "app.c" "model.c" "view.c" "controller.c" "radio_api.c"
        "subpages/view_browse.c" "subpages/view_favorites.c"
        "subpages/view_stations.c" "subpages/view_playing.c"
        "subpages/view_tab_bar.c"
    INCLUDE_DIRS "." "subpages"
    REQUIRES lvgl app_manager uilv lang audio_player input flash_store events esp_http_client mbedtls json
)
