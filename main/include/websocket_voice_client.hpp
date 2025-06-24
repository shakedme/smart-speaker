#pragma once

#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include "../include/voice_assistant_manager.hpp"
#include "../include/esp_websocket_client.h"

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
}

namespace btspeaker {

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

        // Connection management
        esp_err_t connect();

        void disconnect();

        [[nodiscard]] bool isConnected() const { return state.load() == ConnectionState::CONNECTED; }

        // Live audio streaming
        esp_err_t sendAudioChunk(const uint8_t *audioData, size_t length);

        // Cancel ongoing operations
        void cancelOperations();

    private:
        static constexpr const char *TAG = "WS_VOICE_CLIENT";

        int audioChunkCounter{0};

        static void websocketEventHandler(void *handlerArgs, esp_event_base_t base,
                                          int32_t eventId, void *eventData);

        void handleWebsocketEvent(esp_websocket_event_id_t eventId, esp_websocket_event_data_t *eventData);

        // Connection management
        esp_err_t createWebsocketClient();

        void destroyWebsocketClient();

        void setConnectionState(ConnectionState newState);

        // Reconnection logic
        static void reconnectTask(void *pvParameters);

        esp_err_t attemptReconnection();

        // Member variables
        Config config;
        std::atomic<ConnectionState> state;
        esp_websocket_client_handle_t websocketClient;
        SemaphoreHandle_t stateMutex;
        TaskHandle_t reconnectTaskHandle;
        std::atomic_bool isDestroying{false};

        VoiceAssistantManager &voiceAssistantManager;

        // State tracking
        bool initialized;
        bool isClientRunning;
        int reconnectAttempts;

    };

} // namespace btspeaker 