# Path to cspot repository
set(CSPOT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../cspot")

# Your project sources
set(PROJECT_SRCS
        main.cpp
        src/esp_raii_wrapper.cpp
        src/audio_pipeline_manager.cpp
        src/app_manager.cpp
        src/audio_response_player.cpp
        src/websocket_voice_client.cpp
        src/voice_assistant_manager.cpp
        src/microphone_manager.cpp
        src/wake_word_detector.cpp
        src/voice_command_processor.cpp
        src/wifi_manager.cpp
        src/spotify_pipeline.cpp
        src/audio_source_manager.cpp
)

# Register component with all sources
idf_component_register(
        SRCS
        ${PROJECT_SRCS}

        INCLUDE_DIRS
        "."

        REQUIRES
        # Your existing requirements
        driver
        nvs_flash
        esp_system
        log
        esp_event
        audio_board
        bt
        audio_pipeline
        audio_stream
        bluetooth_service
        esp_wifi
        esp_websocket_client
        esp_adc
        esp-sr
        json

        # Additional for cspot
        esp_http_client
        mbedtls
        pthread
        esp_timer
        esp_netif
        mdns
        lwip
        esp_rom
        freertos
        heap
        soc
)

add_subdirectory(${CSPOT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/cspot)

target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)

# Add compile definitions
target_compile_definitions(${COMPONENT_LIB} PUBLIC
        ESP_PLATFORM
        BELL_ONLY_CJSON
        PB_ENABLE_MALLOC
        PB_FIELD_32BIT
)

# Suppress warnings from external code
target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-deprecated-declarations
        -Wno-missing-field-initializers
        -Wno-sign-compare
        $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
)