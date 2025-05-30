//
// Created by Shaked Melman on 29/05/2025.
//

#include "wifi_manager.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
}

namespace smart_speaker {

    static const char *TAG = "WIFI_MANAGER";

// WiFi event bits
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

// Static variables for event handling
    static EventGroupHandle_t s_wifi_event_group;
    static int s_retry_num = 0;

// Event handler for WiFi and IP events
    static void event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
        WifiManager *manager = static_cast<WifiManager *>(arg);

        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "WiFi started, connecting...");
            esp_wifi_connect();
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry_num < WifiManager::MAX_RETRY_COUNT) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Retry connecting to WiFi (%d/%d)", s_retry_num, WifiManager::MAX_RETRY_COUNT);
            } else {
                ESP_LOGE(TAG, "Failed to connect to WiFi after %d attempts", WifiManager::MAX_RETRY_COUNT);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
            ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_retry_num = 0;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

            if (manager) {
                manager->on_connected();
            }
        }
    }

// Constructor
    WifiManager::WifiManager()
            : initialized_(false),
              connected_(false),
              reconnect_enabled_(true) {
        ESP_LOGI(TAG, "WifiManager constructor");
    }

// Destructor
    WifiManager::~WifiManager() {
        ESP_LOGI(TAG, "WifiManager destructor");
        disconnect();
        deinitialize();
    }

// Initialize WiFi stack
    bool WifiManager::initialize() {
        ESP_LOGI(TAG, "Initializing WiFi...");

        if (initialized_) {
            ESP_LOGW(TAG, "WiFi already initialized");
            return true;
        }

        // Initialize NVS (required for WiFi)
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        // Initialize TCP/IP stack
        ESP_ERROR_CHECK(esp_netif_init());

        // Create default event loop
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        // Create default WiFi STA interface
        esp_netif_create_default_wifi_sta();

        // Initialize WiFi with default config
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        // Create event group for synchronization
        s_wifi_event_group = xEventGroupCreate();

        // Register event handlers
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &event_handler,
                                                            this,
                                                            NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &event_handler,
                                                            this,
                                                            NULL));

        // Set WiFi mode to station
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        initialized_ = true;
        ESP_LOGI(TAG, "WiFi initialized successfully");
        return true;
    }

// Connect to hardcoded home network
    bool WifiManager::connect() {
        ESP_LOGI(TAG, "Connecting to WiFi...");

        if (!initialized_) {
            ESP_LOGE(TAG, "WiFi not initialized");
            return false;
        }

        if (connected_) {
            ESP_LOGW(TAG, "Already connected to WiFi");
            return true;
        }

        // Configure WiFi with hardcoded credentials
        wifi_config_t wifi_config = {};

        // Hardcoded home network credentials
        strcpy((char *) wifi_config.sta.ssid, HOME_WIFI_SSID);
        strcpy((char *) wifi_config.sta.password, HOME_WIFI_PASSWORD);

        // Security and connection settings
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Connecting to SSID: %s", HOME_WIFI_SSID);

        // Wait for connection or failure
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Successfully connected to WiFi");
            connected_ = true;

            // Publish WiFi connected event
            PUBLISH_SIMPLE_EVENT(EventType::WiFiConnected);

            return true;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGE(TAG, "Failed to connect to WiFi");
            connected_ = false;
            return false;
        } else {
            ESP_LOGE(TAG, "WiFi connection timeout");
            connected_ = false;
            return false;
        }
    }

// Disconnect from WiFi
    void WifiManager::disconnect() {
        ESP_LOGI(TAG, "Disconnecting from WiFi...");

        if (!initialized_ || !connected_) {
            return;
        }

        reconnect_enabled_ = false;
        esp_wifi_disconnect();
        esp_wifi_stop();

        connected_ = false;
        ESP_LOGI(TAG, "Disconnected from WiFi");

        // Publish WiFi disconnected event
        PUBLISH_SIMPLE_EVENT(EventType::WiFiDisconnected);
    }

// Check if connected
    bool WifiManager::is_connected() const {
        if (!initialized_ || !connected_) {
            return false;
        }

        // Double-check with actual WiFi status
        wifi_ap_record_t ap_info;
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
        return (ret == ESP_OK);
    }

// Get connection info
    WifiStatus WifiManager::get_status() const {
        WifiStatus status = {};

        if (!initialized_) {
            status.state = WifiState::NotInitialized;
            return status;
        }

        if (!connected_) {
            status.state = WifiState::Disconnected;
            return status;
        }

        status.state = WifiState::Connected;
        strcpy(status.ssid, HOME_WIFI_SSID);

        // Get IP address
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                status.ip_address = ip_info.ip;
            }
        }

        // Get signal strength
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            status.rssi = ap_info.rssi;
        }

        return status;
    }

// Print status information
    void WifiManager::print_status() const {
        WifiStatus status = get_status();

        ESP_LOGI(TAG, "=== WiFi Status ===");
        ESP_LOGI(TAG, "State: %s", wifi_state_to_string(status.state));

        if (status.state == WifiState::Connected) {
            ESP_LOGI(TAG, "SSID: %s", status.ssid);
            ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&status.ip_address));
            ESP_LOGI(TAG, "RSSI: %d dBm", status.rssi);
        }

        ESP_LOGI(TAG, "Free heap: %zu bytes", esp_get_free_heap_size());
    }

// Get signal strength percentage (0-100)
    int WifiManager::get_signal_strength() const {
        WifiStatus status = get_status();

        if (status.state != WifiState::Connected) {
            return 0;
        }

        // Convert RSSI to percentage (approximate)
        // RSSI ranges: -30 (excellent) to -90 (poor)
        int rssi = status.rssi;
        if (rssi >= -30) return 100;
        if (rssi <= -90) return 0;

        return (rssi + 90) * 100 / 60;  // Linear mapping
    }

// Enable/disable automatic reconnection
    void WifiManager::set_auto_reconnect(bool enable) {
        reconnect_enabled_ = enable;
        ESP_LOGI(TAG, "Auto-reconnect: %s", enable ? "enabled" : "disabled");
    }

// Deinitialize WiFi stack
    void WifiManager::deinitialize() {
        if (!initialized_) {
            return;
        }

        ESP_LOGI(TAG, "Deinitializing WiFi...");

        disconnect();

        if (s_wifi_event_group) {
            vEventGroupDelete(s_wifi_event_group);
            s_wifi_event_group = nullptr;
        }

        esp_wifi_deinit();
        initialized_ = false;

        ESP_LOGI(TAG, "WiFi deinitialized");
    }

// Event callback for successful connection
    void WifiManager::on_connected() {
        ESP_LOGI(TAG, "WiFi connection established");
        connected_ = true;
    }

// Convert WiFi state to string
    const char *WifiManager::wifi_state_to_string(WifiState state) const {
        switch (state) {
            case WifiState::NotInitialized:
                return "Not Initialized";
            case WifiState::Disconnected:
                return "Disconnected";
            case WifiState::Connecting:
                return "Connecting";
            case WifiState::Connected:
                return "Connected";
            case WifiState::Error:
                return "Error";
            default:
                return "Unknown";
        }
    }

} // namespace smart_speaker