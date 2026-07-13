idf_component_register(
    SRCS
        "app.c" "model.c" "view.c" "controller.c" "hal_esp.c" "wifi_cred.c"
        "subpages/view_main.c" "subpages/view_general.c"
        "subpages/view_wifi.c" "subpages/view_wifi_connect.c"
        "subpages/view_storage.c" "subpages/view_update.c"
        "subpages/view_about.c"
    INCLUDE_DIRS "." "subpages"
    REQUIRES lvgl uilv app_manager sleep_monitor flash_store clock ota esp_wifi esp_netif esp_event nvs_flash events fatfs
)
