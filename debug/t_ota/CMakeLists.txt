cmake_minimum_required(VERSION 3.16)

# Pull in the shared OTA module (system/ota)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/ota
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(t_ota)
