idf_component_register(
    SRCS
        "app.c" "model.c" "view.c" "controller.c" "preset_stations.c"
        "subpages/view_stations.c" "subpages/view_playing.c"
    INCLUDE_DIRS "." "subpages"
    REQUIRES lvgl app_manager uilv lang audio_player input
)
