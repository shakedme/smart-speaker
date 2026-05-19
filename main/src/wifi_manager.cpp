#include "../include/wifi_manager.hpp"
#include "../include/voice_config.hpp"
#include <stdexcept>

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
}

namespace btspeaker {

    std::unique_ptr<WiFiManager> WiFiManager::create() {
        return std::make_unique<WiFiManager>();
    }

    WiFiManager::WiFiManager()
            : status(Status::DISCONNECTED), wifiEventGroup(nullptr), staNetif(nullptr), retryCount(0),
              initialized(false) {
        ESP_LOGI(TAG, "Initializing WiFi manager");

        // Initialize TCP/IP adapter
        esp_err_t ret = esp_netif_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize netif: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to initialize netif");
        }

        // Create default event loop
        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to create event loop");
        }

        // Create WiFi event group
        wifiEventGroup = xEventGroupCreate();
        if (!wifiEventGroup) {
            ESP_LOGE(TAG, "Failed to create WiFi event group");
            throw std::runtime_error("Failed to create WiFi event group");
        }

        // Create default WiFi STA
        staNetif = esp_netif_create_default_wifi_sta();

        // Initialize WiFi
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to initialize WiFi");
        }

        // Register event handler
        ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to register WiFi event handler");
        }

        ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to register IP event handler");
        }

        // Set WiFi mode
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to set WiFi mode");
        }

        // Start WiFi
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
            throw std::runtime_error("Failed to start WiFi");
        }

        initialized = true;
        ESP_LOGI(TAG, "WiFi manager initialized successfully");
    }

    WiFiManager::~WiFiManager() {
        shutdown();
    }

    esp_err_t WiFiManager::connect(const std::string &ssid, const std::string &password) {
        if (!initialized) {
            ESP_LOGE(TAG, "WiFi manager not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        if (ssid.empty()) {
            ESP_LOGE(TAG, "SSID cannot be empty");
            return ESP_ERR_INVALID_ARG;
        }

        ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid.c_str());

        // Configure WiFi
        wifi_config_t wifi_config = {};
        strncpy((char *) wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *) wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;

        esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
            return ret;
        }

        status = Status::CONNECTING;
        retryCount = 0;

        // Clear event bits
        xEventGroupClearBits(wifiEventGroup, WIFI_MGR_CONNECTED_BIT | WIFI_MGR_FAIL_BIT);

        // Start connection
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WiFi connection: %s", esp_err_to_name(ret));
            status = Status::FAILED;
            return ret;
        }

        // Wait for connection result
        EventBits_t bits = xEventGroupWaitBits(wifiEventGroup,
                                               WIFI_MGR_CONNECTED_BIT | WIFI_MGR_FAIL_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(20000)); // 20 second timeout

        if (bits & WIFI_MGR_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to WiFi SSID: %s", ssid.c_str());
            status = Status::CONNECTED;
            if (statusCallback) {
                statusCallback(status);
            }
            return ESP_OK;
        } else if (bits & WIFI_MGR_FAIL_BIT) {
            ESP_LOGI(TAG, "Failed to connect to WiFi SSID: %s", ssid.c_str());
            status = Status::FAILED;
            if (statusCallback) {
                statusCallback(status);
            }
            return ESP_FAIL;
        } else {
            ESP_LOGE(TAG, "WiFi connection timeout");
            status = Status::FAILED;
            if (statusCallback) {
                statusCallback(status);
            }
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t WiFiManager::disconnect() {
        ESP_LOGI(TAG, "Disconnecting from WiFi");

        if (!initialized) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = esp_wifi_disconnect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(ret));
        }

        status = Status::DISCONNECTED;
        if (statusCallback) {
            statusCallback(status);
        }

        return ret;
    }

    void WiFiManager::shutdown() {
        ESP_LOGI(TAG, "Shutting down WiFi manager");

        if (!initialized) {
            return;
        }

        disconnect();

        // Unregister event handlers
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler);
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler);

        // Stop and deinit WiFi
        esp_wifi_stop();
        esp_wifi_deinit();

        // Clean up netif
        if (staNetif) {
            esp_netif_destroy_default_wifi(staNetif);
            staNetif = nullptr;
        }

        // Clean up event group
        if (wifiEventGroup) {
            vEventGroupDelete(wifiEventGroup);
            wifiEventGroup = nullptr;
        }

        initialized = false;
        status = Status::DISCONNECTED;
    }

    void WiFiManager::wifiEventHandler(void *arg, esp_event_base_t eventBase,
                                       int32_t eventId, void *eventData) {
        auto *self = static_cast<WiFiManager *>(arg);

        if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_START) {
            ESP_LOGI(self->TAG, "WiFi started");
        } else if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED) {
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            if (self->retryCount < MAX_RETRY) {
                esp_wifi_connect();
                self->retryCount++;
                ESP_LOGI(self->TAG, "Retry to connect to the AP (attempt %d/%d)",
                         self->retryCount, MAX_RETRY);
            } else {
                xEventGroupSetBits(self->wifiEventGroup, WIFI_MGR_FAIL_BIT);
                ESP_LOGI(self->TAG, "Connect to the AP failed");
                self->status = Status::FAILED;
                if (self->statusCallback) {
                    self->statusCallback(self->status);
                }
            }
        } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
            auto *event = (ip_event_got_ip_t *) eventData;
            ESP_LOGI(self->TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            // Modem sleep delays inbound TCP (Spotify Connect commands) by seconds.
            const esp_err_t psErr = esp_wifi_set_ps(WIFI_PS_NONE);
            if (psErr != ESP_OK) {
                ESP_LOGW(self->TAG, "Failed to disable WiFi power save: %s",
                         esp_err_to_name(psErr));
            } else {
                ESP_LOGI(self->TAG, "WiFi power save disabled for low-latency streaming");
            }
            self->retryCount = 0;
            xEventGroupSetBits(self->wifiEventGroup, WIFI_MGR_CONNECTED_BIT);
            self->status = Status::CONNECTED;
            if (self->statusCallback) {
                self->statusCallback(self->status);
            }
        }
    }

} // namespace btspeaker 