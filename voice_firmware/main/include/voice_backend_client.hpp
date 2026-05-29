#pragma once

#include "esp_websocket_client.h"
#include "ui_state.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

class VoiceBackendClient {
public:
    using NowPlayingCallback = std::function<void(const NowPlayingUiState &)>;
    using WaveformCallback = std::function<void(const WaveformUiState &)>;
    using SessionStateCallback = std::function<void(const char *state)>;

    VoiceBackendClient();
    ~VoiceBackendClient();

    void setNowPlayingCallback(NowPlayingCallback callback);
    void setWaveformCallback(WaveformCallback callback);
    void setSessionStateCallback(SessionStateCallback callback);

    esp_err_t start();
    void stop();
    [[nodiscard]] bool isConnected() const;

    esp_err_t sendMicFrame(const int16_t *samples,
                           uint32_t sample_count,
                           uint32_t sequence,
                           int64_t timestamp_us);

private:
    static constexpr const char *TAG = "VOICE_BACKEND";
    static constexpr size_t RX_MAX_PAYLOAD_BYTES = 16 * 1024;
    static constexpr int RECONNECT_CONNECT_WAIT_MS = 15000;
    static constexpr int RECONNECT_LOOP_IDLE_MS = 1000;
    static constexpr uint32_t CONNECT_OUTCOME_PENDING = 0;
    static constexpr uint32_t CONNECT_OUTCOME_SUCCESS = 1;
    static constexpr uint32_t CONNECT_OUTCOME_FAILURE = 2;

    static void websocketEventHandler(void *handlerArgs,
                                      esp_event_base_t base,
                                      int32_t eventId,
                                      void *eventData);
    void handleWebsocketEvent(esp_websocket_event_id_t eventId, esp_websocket_event_data_t *eventData);
    void handleText(const char *data, size_t length);
    esp_err_t connectOnce();
    esp_err_t createClient();
    void destroyClient();
    void startReconnectLoop();
    void stopReconnectLoop();
    static void reconnectLoopTask(void *arg);
    bool isTransportReady() const;
    uint32_t computeReconnectBackoffMs() const;
    uint32_t beginConnectAttempt();
    void signalConnectOutcome(uint32_t outcome);
    uint32_t waitForConnectOutcome(uint32_t generation, int timeout_ms);
    void recordConnectSuccess();
    void recordConnectFailure();
    esp_err_t sendHello();
    esp_err_t sendText(const char *text);

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> clientRunning_{false};
    std::atomic<bool> reconnectShutdown_{false};
    std::atomic<bool> destroying_{false};
    std::atomic<uint32_t> consecutiveFailures_{0};
    std::atomic<uint32_t> connectGeneration_{0};
    std::atomic<uint32_t> connectOutcome_{CONNECT_OUTCOME_PENDING};
    std::atomic<uint32_t> connectOutcomeGeneration_{0};
    TaskHandle_t reconnectTaskHandle_ = nullptr;
    esp_websocket_client_handle_t websocketClient_ = nullptr;
    NowPlayingCallback nowPlayingCallback_;
    WaveformCallback waveformCallback_;
    SessionStateCallback sessionStateCallback_;
    std::string textBuffer_;
};

}  // namespace btspeaker
