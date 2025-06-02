#pragma once

extern "C" {
#include "esp_err.h"
#include "esp_avrc_api.h"
}

namespace btspeaker {

// Exception-safe initialization helper for Bluetooth components
class BluetoothInitializer {
public:
    BluetoothInitializer();
    ~BluetoothInitializer();
    
    // Delete copy operations
    BluetoothInitializer(const BluetoothInitializer&) = delete;
    BluetoothInitializer& operator=(const BluetoothInitializer&) = delete;
    
    // Move operations
    BluetoothInitializer(BluetoothInitializer&& other) noexcept;
    BluetoothInitializer& operator=(BluetoothInitializer&& other) noexcept;
    
    esp_err_t init_nvs();
    esp_err_t init_avrcp(esp_avrc_tg_cb_t callback);

private:
    bool avrcp_initialized_;
};

} // namespace btspeaker 