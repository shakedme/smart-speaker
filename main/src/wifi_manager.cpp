#include "../include/wifi_manager.hpp"
#include "../include/voice_config.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
}

namespace btspeaker {

WiFiManager::WiFiManager()
    : status_(Status::DISCONNECTED)
    , wifi_event_group_(nullptr)
    , sta_netif_(nullptr)
    , retry_count_(0)
    , initialized_(false)
{
    ESP_LOGI(TAG, "WiFiManager created");
}

WiFiManager::~WiFiManager() {
    shutdown();
}

esp_err_t WiFiManager::init() {
    ESP_LOGI(TAG, "Initializing WiFi manager");
    
    if (initialized_) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return ESP_OK;
    }
    
    // Initialize TCP/IP adapter
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize netif: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create default event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create WiFi event group
    wifi_event_group_ = xEventGroupCreate();
    if (!wifi_event_group_) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return ESP_ERR_NO_MEM;
    }
    
    // Create default WiFi STA
    sta_netif_ = esp_netif_create_default_wifi_sta();
    
    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register event handler
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set WiFi mode
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Start WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "WiFi manager initialized successfully");
    return ESP_OK;
}

esp_err_t WiFiManager::connect(const std::string& ssid, const std::string& password) {
    if (!initialized_) {
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
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }
    
    status_ = Status::CONNECTING;
    retry_count_ = 0;
    
    // Clear event bits
    xEventGroupClearBits(wifi_event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // Start connection
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi connection: %s", esp_err_to_name(ret));
        status_ = Status::FAILED;
        return ret;
    }
    
    // Wait for connection result
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group_,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          pdMS_TO_TICKS(20000)); // 20 second timeout
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi SSID: %s", ssid.c_str());
        status_ = Status::CONNECTED;
        if (status_callback_) {
            status_callback_(status_);
        }
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to WiFi SSID: %s", ssid.c_str());
        status_ = Status::FAILED;
        if (status_callback_) {
            status_callback_(status_);
        }
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
        status_ = Status::FAILED;
        if (status_callback_) {
            status_callback_(status_);
        }
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t WiFiManager::disconnect() {
    ESP_LOGI(TAG, "Disconnecting from WiFi");
    
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(ret));
    }
    
    status_ = Status::DISCONNECTED;
    if (status_callback_) {
        status_callback_(status_);
    }
    
    return ret;
}

void WiFiManager::shutdown() {
    ESP_LOGI(TAG, "Shutting down WiFi manager");
    
    if (!initialized_) {
        return;
    }
    
    disconnect();
    
    // Unregister event handlers
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    
    // Stop and deinit WiFi
    esp_wifi_stop();
    esp_wifi_deinit();
    
    // Clean up netif
    if (sta_netif_) {
        esp_netif_destroy_default_wifi(sta_netif_);
        sta_netif_ = nullptr;
    }
    
    // Clean up event group
    if (wifi_event_group_) {
        vEventGroupDelete(wifi_event_group_);
        wifi_event_group_ = nullptr;
    }
    
    initialized_ = false;
    status_ = Status::DISCONNECTED;
}

void WiFiManager::wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    WiFiManager* self = static_cast<WiFiManager*>(arg);
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(self->TAG, "WiFi started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (self->retry_count_ < MAX_RETRY) {
            esp_wifi_connect();
            self->retry_count_++;
            ESP_LOGI(self->TAG, "Retry to connect to the AP (attempt %d/%d)", 
                    self->retry_count_, MAX_RETRY);
        } else {
            xEventGroupSetBits(self->wifi_event_group_, WIFI_FAIL_BIT);
            ESP_LOGI(self->TAG, "Connect to the AP failed");
            self->status_ = Status::FAILED;
            if (self->status_callback_) {
                self->status_callback_(self->status_);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(self->TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        self->retry_count_ = 0;
        xEventGroupSetBits(self->wifi_event_group_, WIFI_CONNECTED_BIT);
        self->status_ = Status::CONNECTED;
        if (self->status_callback_) {
            self->status_callback_(self->status_);
        }
    }
}

} // namespace btspeaker 