idf_component_register(
    SRCS
        "app.c" "model.c" "view.c" "controller.c"
        "subpages/view_files.c" "subpages/view_player.c" "subpages/view_tab_bar.c"
    INCLUDE_DIRS "." "subpages"
    REQUIRES lvgl app_manager uilv lang audio_player
)
