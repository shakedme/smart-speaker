#pragma once

#include <string>
#include <functional>

extern "C" {
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
}

namespace btspeaker {

class WiFiManager {
public:
    enum class Status {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        FAILED
    };
    
    using StatusCallback = std::function<void(Status status)>;
    
    WiFiManager();
    ~WiFiManager();
    
    // Delete copy operations
    WiFiManager(const WiFiManager&) = delete;
    WiFiManager& operator=(const WiFiManager&) = delete;
    
    esp_err_t init();
    esp_err_t connect(const std::string& ssid, const std::string& password);
    esp_err_t disconnect();
    void shutdown();
    
    Status get_status() const { return status_; }
    bool is_connected() const { return status_ == Status::CONNECTED; }
    
    void set_status_callback(StatusCallback callback) { status_callback_ = callback; }

private:
    static constexpr const char* TAG = "WIFI_MGR";
    static constexpr int WIFI_CONNECTED_BIT = BIT0;
    static constexpr int WIFI_FAIL_BIT = BIT1;
    static constexpr int MAX_RETRY = 5;
    
    static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data);
    
    Status status_;
    EventGroupHandle_t wifi_event_group_;
    esp_netif_t* sta_netif_;
    int retry_count_;
    StatusCallback status_callback_;
    bool initialized_;
};

} // namespace btspeaker 