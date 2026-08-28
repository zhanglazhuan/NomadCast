idf_component_register(
    SRCS "monitor.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer esp_system heap freertos
)
