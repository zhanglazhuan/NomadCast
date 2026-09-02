idf_component_register(SRCS "t_tdp.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_adc esp_timer fatfs sdmmc
                                 esp_wifi esp_netif esp_event nvs_flash
                                 esp_http_client mbedtls)
