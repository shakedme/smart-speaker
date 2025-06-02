idf_component_register(
        SRCS
        # Main application
        main.cpp

        INCLUDE_DIRS
        # Make headers available to other components
        "."

        REQUIRES
        # Core ESP-IDF components
        "driver"          # GPIO, I2S drivers
        "esp_timer"       # High-resolution timers
        "nvs_flash"       # Non-volatile storage
        "esp_system"      # System functions
        "esp_common"      # Common ESP-IDF utilities
        "freertos"        # FreeRTOS kernel
        "heap"            # Memory management
        "log"             # Logging system
        "newlib"          # C++ standard library support
        "pthread"         # POSIX threads (for C++ threading)
        "spi_flash"       # Flash memory access
        "json"

        # Bluetooth components (all profiles included in 'bt')
        "bt"              # Complete Bluetooth stack with A2DP, AVRCP, etc.

        # Network components
        "esp_wifi"        # WiFi stack
        "esp_netif"       # Network interface
        "esp_event"       # Event handling
        "esp_http_client" # HTTP client for APIs
        "lwip"            # TCP/IP stack

        # Optional components (uncomment as needed)
        # "esp_https_ota"   # OTA updates
        # "mdns"            # mDNS service discovery
        # "esp_tls"         # TLS/SSL support
        # "mbedtls"         # Cryptography
        # "json"            # JSON parsing
        # "fatfs"           # File system
)