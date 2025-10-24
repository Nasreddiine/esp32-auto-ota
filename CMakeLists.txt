cmake_minimum_required(VERSION 3.5)

set(SUPPORTED_TARGETS esp32 esp32s2 esp32s3 esp32c3 esp32c2 esp32c6 esp32h2)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT IDF_TARGET)
        set(IDF_TARGET "esp32")
    elseif(NOT IDF_TARGET IN_LIST SUPPORTED_TARGETS)
        message(FATAL_ERROR "Invalid IDF_TARGET: ${IDF_TARGET}")
    endif()
endif()

# Set project version
set(PROJECT_VER "1.0.0") 
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_auto_ota)
