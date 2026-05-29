set(CSPOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../cspot")

set(PROJECT_SRCS
        main.cpp
        src/playback_app_manager.cpp
        src/playback_backend_client.cpp
        src/esp_raii_wrapper.cpp
        src/bluetooth_pipeline.cpp
        src/task_affinity.cpp
        src/llm_pipeline.cpp
        src/spotify_pipeline.cpp
        src/audio_pipeline_util.cpp
        src/audio_source_manager.cpp
        src/adf_audio_sink.cpp
        src/audio_sink_base.cpp
        src/base_audio_pipeline.cpp
        src/spotify_auth_manager.cpp
        src/playback_reference_tap.cpp
        "${CSPOT_ROOT}/esp32/main/EspPlayer.cpp"
)

idf_component_register(
        SRCS ${PROJECT_SRCS}
        INCLUDE_DIRS
        "."
        "include"
        "${CSPOT_ROOT}/esp32/main"
        "${CSPOT_ROOT}/include"
        REQUIRES
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
        json
        esp-dsp
        esp_http_client
        esp_http_server
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
        smart_common
        speaker_protocol
)

add_subdirectory(${CSPOT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/cspot)
target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
        ESP_PLATFORM
        BELL_ONLY_CJSON
        PB_ENABLE_MALLOC
        PB_FIELD_32BIT
        CONFIG_BTSPK_BACKEND_AEC_REF_STREAM=1
        DEVICE_NODE_ROLE_PLAYBACK=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-deprecated-declarations
        -Wno-missing-field-initializers
        -Wno-sign-compare
        $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
)
