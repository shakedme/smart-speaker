#include "../include/bluetooth_initializer.hpp"

extern "C" {
#include "esp_log.h"
#include "nvs_flash.h"
}

namespace btspeaker {

static constexpr const char* TAG = "BT_INIT";

BluetoothInitializer::BluetoothInitializer() : avrcp_initialized_(false) {
    ESP_LOGD(TAG, "BluetoothInitializer created");
}

BluetoothInitializer::~BluetoothInitializer() {
    if (avrcp_initialized_) {
        ESP_LOGD(TAG, "Deinitializing AVRCP");
        esp_avrc_tg_deinit();
    }
}

BluetoothInitializer::BluetoothInitializer(BluetoothInitializer&& other) noexcept 
    : avrcp_initialized_(other.avrcp_initialized_) {
    other.avrcp_initialized_ = false;
}

BluetoothInitializer& BluetoothInitializer::operator=(BluetoothInitializer&& other) noexcept {
    if (this != &other) {
        // Clean up current state
        if (avrcp_initialized_) {
            esp_avrc_tg_deinit();
        }
        
        // Move from other
        avrcp_initialized_ = other.avrcp_initialized_;
        other.avrcp_initialized_ = false;
    }
    return *this;
}

esp_err_t BluetoothInitializer::init_nvs() {
    ESP_LOGD(TAG, "Initializing NVS");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase and reinit: %s", esp_err_to_name(ret));
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized successfully");
    } else {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t BluetoothInitializer::init_avrcp(esp_avrc_tg_cb_t callback) {
    ESP_LOGD(TAG, "Initializing AVRCP target");
    
    esp_err_t ret = esp_avrc_tg_register_callback(callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register AVRCP callback: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_avrc_tg_init();
    if (ret == ESP_OK) {
        avrcp_initialized_ = true;
        ESP_LOGI(TAG, "AVRCP target initialized successfully");
    } else {
        ESP_LOGE(TAG, "Failed to initialize AVRCP target: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

} // namespace btspeaker 