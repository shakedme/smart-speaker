idf_component_register(
        SRCS
        # Main application
        main.cpp
        app/app_bluetooth_audio.cpp

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

        # Testing framework
        "unity"           # Unit testing framework

        # Optional components (uncomment as needed when implementing features)
        # "esp_timer"       # High-resolution timers
        # "esp_common"      # Common ESP-IDF utilities
        # "freertos"        # FreeRTOS kernel
        # "heap"            # Memory management
        # "newlib"          # C++ standard library support
        # "pthread"         # POSIX threads (for C++ threading)
        # "spi_flash"       # Flash memory access
        # "json"            # JSON parsing
        # "esp_wifi"        # WiFi stack
        # "esp_netif"       # Network interface
        # "esp_http_client" # HTTP client for APIs
        # "lwip"            # TCP/IP stack
        # "esp_peripherals" # ESP-ADF peripheral management
        # "esp_https_ota"   # OTA updates
        # "mdns"            # mDNS service discovery
        # "esp_tls"         # TLS/SSL support
        # "mbedtls"         # Cryptography
        # "fatfs"           # File system
)