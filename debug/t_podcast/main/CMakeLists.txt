idf_component_register(
    SRCS "t_podcast.c"
         "../../../apps/podcast/backend.c"
         "../../../apps/podcast/http_esp.c"
         "../../../apps/podcast/cJSON.c"
    INCLUDE_DIRS "."
                 "../../../apps/podcast"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_client mbedtls
)
