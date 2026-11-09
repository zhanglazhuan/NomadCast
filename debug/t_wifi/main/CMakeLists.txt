idf_component_register(SRCS "t_wifi.c"
                       INCLUDE_DIRS "." "../../../boards/esp32s3"
                       REQUIRES esp_wifi esp_http_client esp_netif nvs_flash fatfs driver)
