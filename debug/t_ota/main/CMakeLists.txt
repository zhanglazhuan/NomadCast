idf_component_register(SRCS "t_ota.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash ota)
