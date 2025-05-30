cmake_minimum_required(VERSION 3.16)

# Set C++ standard before project definition
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ESP32 optimized C++ flags (only for C++ files)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti -Wno-error=unused-function")

# Enable ESP32 platform definition
add_definitions(-DESP32_PLATFORM)

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project definition - this must come after the ESP-IDF include
project(bt_speaker)