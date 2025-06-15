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
        std::string server_url;
        int keepalive_interval_s = 30;
        int ping_interval_s = 10;
        size_t chunk_size = 4096;
        int reconnect_timeout_ms = 5000;
        int max_reconnect_attempts = 5;
    };

    explicit WebSocketVoiceClient(const Config& config);
    ~WebSocketVoiceClient();
    
    // Delete copy operations
    WebSocketVoiceClient(const WebSocketVoiceClient&) = delete;
    WebSocketVoiceClient& operator=(const WebSocketVoiceClient&) = delete;
    
    esp_err_t init();
    void shutdown();
    
    // Connection management
    esp_err_t connect();
    void disconnect();
    [[nodiscard]] bool is_connected() const { return state_.load() == ConnectionState::CONNECTED; }
    [[nodiscard]] ConnectionState get_state() const { return state_.load(); }
    
    // Live audio streaming
    esp_err_t send_audio_chunk(const uint8_t* audio_data, size_t length);

    // Callback registration
    void set_audio_response_callback(AudioResponseCallback callback);
    void set_state_change_callback(StateChangeCallback callback);
    
    // Cancel ongoing operations
    void cancel_operations();

private:
    static constexpr const char* TAG = "WS_VOICE_CLIENT";
    
    // WebSocket event handler
    static void websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                      int32_t event_id, void* event_data);
    void handle_websocket_event(esp_websocket_event_id_t event_id, esp_websocket_event_data_t* event_data);
    
    // Connection management
    esp_err_t create_websocket_client();
    void destroy_websocket_client();
    void set_state(ConnectionState new_state);
    
    // Message handling
    void handle_text_message(const char* data, size_t length);
    void handle_binary_message(const uint8_t* data, size_t length);
    
    // Reconnection logic
    static void reconnect_task(void* pvParameters);
    esp_err_t attempt_reconnection();
    
    // Member variables
    Config config_;
    std::atomic<ConnectionState> state_;
    esp_websocket_client_handle_t websocket_client_;
    SemaphoreHandle_t state_mutex_;
    TaskHandle_t reconnect_task_handle_;
    
    // Callbacks
    AudioResponseCallback audio_callback_;
    StateChangeCallback state_callback_;
    
    // State tracking
    bool initialized_;
    int reconnect_attempts_;
};

} // namespace btspeaker 