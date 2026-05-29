cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -Wno-error=unused-function")

set(SHARED_FIRMWARE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../shared_firmware")
set(EXTRA_COMPONENT_DIRS
        $ENV{ADF_PATH}/components
        ${SHARED_FIRMWARE_ROOT}/components
        ${CMAKE_CURRENT_LIST_DIR}/components
)

set(AUDIO_BOARD_PATH "${CMAKE_CURRENT_LIST_DIR}/components/audio_board/include")
include_directories(${AUDIO_BOARD_PATH})

add_definitions(-DESP32_PLATFORM)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(playback_node)
