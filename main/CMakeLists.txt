idf_component_register(
        SRCS

        main.cpp

        src/esp_raii_wrapper.cpp
        src/avrcp_handler.cpp
        src/audio_pipeline_manager.cpp
        src/bluetooth_initializer.cpp
        src/app_manager.cpp
        src/audio_response_player.cpp
        src/websocket_voice_client.cpp
        src/voice_assistant_manager.cpp
        src/microphone_manager.cpp
        src/wake_word_detector.cpp
        src/voice_command_processor.cpp
        src/wifi_manager.cpp

        INCLUDE_DIRS
        # Make headers available to other components
        "."

        REQUIRES
        # Core ESP-IDF components actually used
        "driver"          # GPIO, I2S drivers (used by audio_board component)
        "nvs_flash"       # Non-volatile storage (used for NVS initialization)
        "esp_system"      # System functions (used in main.hpp)
        "log"             # Logging system (used for ESP_LOG macros)
        "esp_event"       # Event handling (used by audio pipeline)
        "audio_board"     # Custom audio board component
        "bt"              # Bluetooth stack (needed by bluetooth_service)

        # ESP-ADF components actually used
        "audio_pipeline"  # Audio pipeline management
        "audio_stream"    # I2S stream functionality  
        "bluetooth_service" # Bluetooth A2DP service
        "esp_wifi"        # WiFi stack
        "esp_websocket_client" # WebSocket client for voice streaming
        "esp_adc"         # ADC support (if using ADC for audio input)
        "esp-sr"          # ESP Speech Recognition for wake word detection
        "json"            # JSON parsing for WebSocket messages

        # Optional components (uncomment as needed when implementing features)
        # "esp_timer"       # High-resolution timers
        # "esp_common"      # Common ESP-IDF utilities
        # "freertos"        # FreeRTOS kernel
        # "heap"            # Memory management
        # "newlib"          # C++ standard library support
        # "pthread"         # POSIX threads (for C++ threading)
        # "spi_flash"       # Flash memory access
        # "esp_netif"       # Network interface
        # "lwip"            # TCP/IP stack
        # "esp_peripherals" # ESP-ADF peripheral management
        # "esp_https_ota"   # OTA updates
        # "mdns"            # mDNS service discovery
        # "esp_tls"         # TLS/SSL support
        # "fatfs"           # File system
)