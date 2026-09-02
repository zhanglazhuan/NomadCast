# ESP-IDF project boilerplate — must be in this exact order
cmake_minimum_required(VERSION 3.16)

# Include esp-adf components (needed for AUDIO mode, harmless for others)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/esp-adf/components
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers
)

# Exclude ADF components that pull in unwanted board-specific code
set(EXCLUDE_COMPONENTS audio_board audio_hal display_service esp_peripherals)

# Global include for ADF-compatible board.h (same as root project)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../boards/esp32s3)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(t_tdp)
