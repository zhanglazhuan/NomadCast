# ESP-IDF project boilerplate — must be in this exact order
cmake_minimum_required(VERSION 3.16)

# Include esp-adf components and shared drivers
# Note: board_leisound is copied locally to debug/t_play/components/
#       so we don't need the entire ../../components dir
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/esp-adf/components
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers
)

# Exclude ADF components that pull in unwanted board-specific code
set(EXCLUDE_COMPONENTS audio_board audio_hal display_service esp_peripherals)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(t_play)
