#pragma once

#include <memory>
#include <string>
#include <functional>
#include <atomic>

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
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

    // Callback for receiving audio response
    using AudioResponseCallback = std::function<void(const uint8_t* audio_data, size_t length, bool is_final, const char* intent_json)>;
    using StateChangeCallback = std::function<void(ConnectionState new_state)>;
    
    struct Config {
        std::string serverUrl;
        int keepaliveIntervalS = 30;
        int pingIntervalS = 10;
        size_t chunkSize = 4096;
        int reconnectTimeoutMs = 5000;
        int maxReconnectAttempts = 5;
    };

    explicit WebSocketVoiceClient();
    ~WebSocketVoiceClient();
    
    // Delete copy operations
    WebSocketVoiceClient(const WebSocketVoiceClient&) = delete;
    WebSocketVoiceClient& operator=(const WebSocketVoiceClient&) = delete;
    
    esp_err_t init();
    void shutdown();
    
    // Connection management
    esp_err_t connect();
    void disconnect();
    [[nodiscard]] bool isConnected() const { return state.load() == ConnectionState::CONNECTED; }

    // Live audio streaming
    esp_err_t sendAudioChunk(const uint8_t* audioData, size_t length);

    // Callback registration
    void setAudioResponseCallback(AudioResponseCallback callback);
    void setStateChangeCallback(StateChangeCallback callback);
    
    // Cancel ongoing operations
    void cancelOperations();

private:
    static constexpr const char* TAG = "WS_VOICE_CLIENT";
    
    // WebSocket event handler
    static void websocketEventHandler(void* handlerArgs, esp_event_base_t base,
                                      int32_t eventId, void* eventData);
    void handleWebsocketEvent(esp_websocket_event_id_t eventId, esp_websocket_event_data_t* eventData);
    
    // Connection management
    esp_err_t createWebsocketClient();
    void destroyWebsocketClient();
    void setState(ConnectionState newState);
    
    // Message handling
    void handleTextMessage(const char* data, size_t length);
    void handleBinaryMessage(const uint8_t* data, size_t length);
    
    // Reconnection logic
    static void reconnectTask(void* pvParameters);
    esp_err_t attemptReconnection();
    
    // Member variables
    Config config;
    std::atomic<ConnectionState> state;
    esp_websocket_client_handle_t websocketClient;
    SemaphoreHandle_t stateMutex;
    TaskHandle_t reconnectTaskHandle;
    
    // Callbacks
    AudioResponseCallback audioCallback;
    StateChangeCallback stateChangeCallback;
    
    // State tracking
    bool initialized;
    int reconnectAttempts;
};

} // namespace btspeaker 