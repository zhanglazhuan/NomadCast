# Full podcast component — all subpages, MVC layers
idf_component_register(
    SRCS
        "app.c"
        "model.c"
        "controller.c"
        "view.c"
        "backend.c"
        "http_esp.c"
        "cache.c"
        "local_cache.c"
        "task_store.c"
        "cJSON.c"
        "subpages/view_channel.c"
        "subpages/view_download_task.c"
        "subpages/view_local.c"
        "subpages/view_login.c"
        "subpages/view_network.c"
        "subpages/view_player.c"
        "subpages/view_profile.c"
        "subpages/view_search.c"
        "subpages/view_search_results.c"
        "subpages/view_settings.c"
        "subpages/view_tab_bar.c"
        "lv_channel_card.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl app_manager launcher uilv flash_store events esp_http_client mbedtls audio_player fatfs sleep_monitor
)
