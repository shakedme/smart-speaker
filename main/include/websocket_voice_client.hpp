#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "../include/esp_websocket_client.h"

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
}

namespace btspeaker {

    class VoiceAssistantManager;

    class WebSocketVoiceClient {
    public:
        enum class ConnectionState {
            DISCONNECTED,
            CONNECTING,
            CONNECTED,
            ERROR
        };

        struct Config {
            std::string serverUrl;
            int keepaliveIntervalS = 30;
            int pingIntervalS = 10;
            size_t chunkSize = 4096;
            int reconnectTimeoutMs = 5000;
            int maxReconnectAttempts = 5;
        };

        explicit WebSocketVoiceClient(VoiceAssistantManager &voiceAssistantManager);

        ~WebSocketVoiceClient();

        void init();

        void shutdown();

        esp_err_t connect();

        void disconnect();

        [[nodiscard]] bool isConnected() const { return state.load() == ConnectionState::CONNECTED; }

        // Non-blocking: copies audio and enqueues for mic_uplink worker.
        esp_err_t sendAudioChunk(const uint8_t *audioData, size_t length);

    private:
        struct UplinkChunk {
            bool shutdown;
            uint8_t *payload;
            uint32_t length;
        };

        static constexpr const char *TAG = "WS_VOICE_CLIENT";
        static constexpr size_t UPLINK_QUEUE_DEPTH = 16;
        static constexpr size_t UPLINK_TASK_STACK_WORDS = 4096;
        static constexpr int UPLINK_SEND_TIMEOUT_MS = 250;
        static constexpr int UPLINK_DIAG_INTERVAL_MS = 5000;
        static constexpr uint32_t UPLINK_SLOW_SEND_US = 100000;
        static constexpr UBaseType_t UPLINK_TASK_PRIORITY = 6;

        static void websocketEventHandler(void *handlerArgs, esp_event_base_t base,
                                          int32_t eventId, void *eventData);

        static void uplinkWorkerTask(void *arg);

        void handleWebsocketEvent(esp_websocket_event_id_t eventId, esp_websocket_event_data_t *eventData);

        esp_err_t createWebsocketClient();

        void destroyWebsocketClient();

        void setConnectionState(ConnectionState newState);

        static void reconnectTask(void *pvParameters);

        esp_err_t attemptReconnection();

        esp_err_t startUplinkWorker();

        void shutdownUplinkWorker();

        void runUplinkWorker();

        esp_err_t enqueueUplinkChunk(UplinkChunk chunk);

        bool dropOldestUplinkChunk();

        void freeUplinkChunk(UplinkChunk &chunk);

        void drainUplinkQueue();

        void logUplinkDiagnostics(int64_t now_us,
                                  uint32_t enqueued,
                                  uint32_t sent,
                                  uint32_t dropped_oldest,
                                  uint32_t dropped_disc,
                                  uint32_t send_fail,
                                  uint32_t send_slow,
                                  uint64_t bytes_sent,
                                  uint64_t sum_send_us,
                                  uint32_t max_send_us,
                                  uint32_t queue_depth) const;

        Config config;
        std::atomic<ConnectionState> state;
        esp_websocket_client_handle_t websocketClient;
        SemaphoreHandle_t stateMutex;
        TaskHandle_t reconnectTaskHandle;
        std::atomic_bool isDestroying{false};

        VoiceAssistantManager &voiceAssistantManager;

        bool initialized;
        bool isClientRunning;
        int reconnectAttempts;

        QueueHandle_t uplinkQueue_ = nullptr;
        TaskHandle_t uplinkTaskHandle_ = nullptr;
        SemaphoreHandle_t uplinkStoppedSem_ = nullptr;
        StackType_t *uplinkTaskStack_ = nullptr;
        StaticTask_t *uplinkTaskBuffer_ = nullptr;
        std::atomic<bool> uplinkAccepting_{false};
        std::atomic<uint32_t> uplinkDiagEnqueued_{0};
        std::atomic<uint32_t> uplinkDiagDroppedOldest_{0};
    };

} // namespace btspeaker
