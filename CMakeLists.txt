# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Extra component paths (system/ holds system-level modules)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/system/uilv
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/system/app_manager
    ${CMAKE_CURRENT_SOURCE_DIR}/system/launcher
    ${CMAKE_CURRENT_SOURCE_DIR}/system/input
    ${CMAKE_CURRENT_SOURCE_DIR}/system/sleep_monitor
    ${CMAKE_CURRENT_SOURCE_DIR}/system/flash_store
    ${CMAKE_CURRENT_SOURCE_DIR}/system/clock
    ${CMAKE_CURRENT_SOURCE_DIR}/system/battery_monitor
    ${CMAKE_CURRENT_SOURCE_DIR}/system/events
    ${CMAKE_CURRENT_SOURCE_DIR}/system/logs
    ${CMAKE_CURRENT_SOURCE_DIR}/system/ota
    ${CMAKE_CURRENT_SOURCE_DIR}/system/audio_player
    ${CMAKE_CURRENT_SOURCE_DIR}/system/wifi
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/settings
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/podcast
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/esp-adf/components
)

# Global include for ADF-compatible board.h (available to all components)
# Board configs live in boards/<target>/ — currently esp32s3
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32s3)

# Exclude conflicting ADF components (vendor/esp-adf)
set(EXCLUDE_COMPONENTS audio_board audio_hal display_service esp_peripherals audio_sal)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(NomadCast)
