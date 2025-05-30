//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_WIFI_MANAGER_H
#define BT_SPEAKER_WIFI_MANAGER_H

#pragma once

#include "event_bus.hpp"
#include <atomic>
#include <string>

extern "C" {
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_event.h"
#include "esp_log.h"
}

namespace smart_speaker {

// WiFi manager configuration
// TODO: Replace with your actual home network credentials
#define HOME_WIFI_SSID     "YourNetworkName"
#define HOME_WIFI_PASSWORD "YourNetworkPassword"

// WiFi connection states
    enum class WifiState : uint8_t {
        NotInitialized,
        Disconnected,
        Connecting,
        Connected,
        Error
    };

// WiFi status information
    struct WifiStatus {
        WifiState state = WifiState::NotInitialized;
        char ssid[33] = {0};        // WiFi SSID (max 32 chars + null)
        esp_ip4_addr_t ip_address = {};    // IP address structure
        int8_t rssi = 0;            // Signal strength in dBm
        uint32_t uptime_ms = 0;     // Connection uptime
    };

// Simple WiFi manager for home network connection
    class WifiManager {
    public:
        // Configuration constants
        static constexpr int MAX_RETRY_COUNT = 5;
        static constexpr int CONNECT_TIMEOUT_MS = 15000;  // 15 seconds
        static constexpr int RECONNECT_DELAY_MS = 5000;   // 5 seconds

    private:
        std::atomic<bool> initialized_;
        std::atomic<bool> connected_;
        std::atomic<bool> reconnect_enabled_;

        static constexpr const char* TAG = "WIFI_MANAGER";

    public:
        WifiManager();
        ~WifiManager();

        // Non-copyable, non-movable (singleton-like behavior)
        WifiManager(const WifiManager&) = delete;
        WifiManager& operator=(const WifiManager&) = delete;
        WifiManager(WifiManager&&) = delete;
        WifiManager& operator=(WifiManager&&) = delete;

        // Lifecycle management
        bool initialize();
        void deinitialize();

        // Connection management
        bool connect();                    // Connect to hardcoded home network
        void disconnect();
        bool is_connected() const;

        // Status and monitoring
        WifiStatus get_status() const;
        void print_status() const;
        int get_signal_strength() const;   // Returns 0-100%

        // Configuration
        void set_auto_reconnect(bool enable);
        bool is_auto_reconnect_enabled() const {
            return reconnect_enabled_.load(std::memory_order_acquire);
        }

        // Event callback (called by WiFi event handler)
        void on_connected();

    private:
        // Helper methods
        const char* wifi_state_to_string(WifiState state) const;

        // Static event handler (friend function declared in .cpp)
        static void event_handler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data);
    };

// Factory for creating WiFi manager instance
    namespace WifiManagerFactory {
        inline std::unique_ptr<WifiManager> create_home_wifi_manager() {
            return std::make_unique<WifiManager>();
        }
    }

} // namespace smart_speaker

#endif //BT_SPEAKER_WIFI_MANAGER_H