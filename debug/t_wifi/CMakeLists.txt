cmake_minimum_required(VERSION 3.16)

# Pull in project-level wifi component
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../system/wifi
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(t_wifi)
