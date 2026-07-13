# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Extra component paths (sys/ holds system-level modules)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/uilv
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/app_manager
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/launcher
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/input
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/sleep_monitor
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/flash_store
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/clock
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/battery_monitor
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/events
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/ota
    ${CMAKE_CURRENT_SOURCE_DIR}/sys/audio_player
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/settings
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/podcast
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/esp-adf/components
)

# Exclude conflicting ADF components and old C++ BSP
set(EXCLUDE_COMPONENTS display touch_bsp i2c_bsp battery audio codec_board wifi board_leisound audio_board audio_hal display_service esp_peripherals audio_sal)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(NomadCast)
