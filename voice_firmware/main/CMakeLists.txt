set(PROJECT_SRCS
        main.cpp
        src/voice_ui_app_manager.cpp
        src/voice_backend_client.cpp
        src/korvo2_board_resources.c
        src/korvo2_mic_capture_backend.cpp
        src/sph0645_dual_mic_capture_backend.cpp
        src/korvo2_lcd_display_backend.cpp
        src/production_spi_lcd_display_backend.cpp
        src/ui_app.cpp
        src/now_playing_screen.cpp
        src/waveform_view.cpp
)

idf_component_register(
        SRCS ${PROJECT_SRCS}
        INCLUDE_DIRS
        "."
        "include"
        REQUIRES
        driver
        nvs_flash
        esp_system
        log
        esp_event
        esp_wifi
        esp_timer
        esp_netif
        json
        freertos
        heap
        smart_common
        speaker_protocol
        audio_board
        audio_pipeline
        audio_stream
        esp_peripherals
        esp_lcd
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
        ESP_PLATFORM
        CONFIG_BTSPK_BACKEND_AEC_REF_STREAM=1
        DEVICE_NODE_ROLE_VOICE_UI=1
        VOICE_UI_BOARD_PROFILE_KORVO2_LCD=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-deprecated-declarations
        -Wno-missing-field-initializers
        -Wno-sign-compare
        $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
)
