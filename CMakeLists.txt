cmake_minimum_required(VERSION 3.16)

# Set C++ standard before project definition
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ESP32 optimized C++ flags (only for C++ files)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti -Wno-error=unused-function")

# Set ESP-ADF component path
set(EXTRA_COMPONENT_DIRS $ENV{ADF_PATH}/components ${CMAKE_SOURCE_DIR}/components)

# Set custom audio board include path so ESP-ADF components find our board.h
set(AUDIO_BOARD_PATH "${CMAKE_SOURCE_DIR}/components/audio_board/include")
include_directories(${AUDIO_BOARD_PATH})

# Enable ESP32 platform definition
add_definitions(-DESP32_PLATFORM)

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project definition - this must come after the ESP-IDF include
project(bt_speaker)