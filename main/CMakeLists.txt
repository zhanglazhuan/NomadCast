idf_component_register(SRCS "NomadCast.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl esp_lcd driver esp_timer app_manager launcher settings podcast input uilv flash_store sleep_monitor clock battery_monitor events audio_player ota gt911)

target_include_directories(${COMPONENT_LIB} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../sys/uilv/framework
    ${CMAKE_CURRENT_SOURCE_DIR}/../sys/uilv/widgets
)
