#pragma once

#include <memory>
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

        static std::unique_ptr<WiFiManager> create();

        WiFiManager();

        ~WiFiManager();

        // Delete copy operations
        WiFiManager(const WiFiManager &) = delete;

        WiFiManager &operator=(const WiFiManager &) = delete;

        esp_err_t connect(const std::string &ssid, const std::string &password);

        esp_err_t disconnect();

        void shutdown();

        [[nodiscard]] bool isConnected() const { return status == Status::CONNECTED; }

        void setStatusCallback(StatusCallback callback);

    private:
        static constexpr const char *TAG = "WIFI_MGR";
        static constexpr int WIFI_MGR_CONNECTED_BIT = BIT0;
        static constexpr int WIFI_MGR_FAIL_BIT = BIT1;
        static constexpr int MAX_RETRY = 5;

        static void wifiEventHandler(void *arg, esp_event_base_t eventBase,
                                     int32_t eventId, void *eventData);

        Status status;
        EventGroupHandle_t wifiEventGroup;
        esp_netif_t *staNetif;
        int retryCount;
        StatusCallback statusCallback;
        bool initialized;
    };

} // namespace btspeaker 