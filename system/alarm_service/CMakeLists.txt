idf_component_register(
    SRCS "alarm_service.c" "alarm_tone.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl flash_store sleep_monitor audio_player driver esp_timer uilv lang
)
