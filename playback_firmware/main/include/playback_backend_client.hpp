#pragma once

#include "aec_ref_config.hpp"
#include "esp_websocket_client.h"
#include "task_affinity.hpp"
#include "uplink_audio_frame.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
}

namespace btspeaker {

class PlaybackBackendClient {
public:
    using AudioDownlinkCallback = std::function<void(const uint8_t *data, size_t length)>;
    using IntentCallback = std::function<void(const char *intent, const char *data)>;
    using StateChangeCallback = std::function<void(const char *state)>;

    PlaybackBackendClient();
    ~PlaybackBackendClient();

    PlaybackBackendClient(const PlaybackBackendClient &) = delete;
    PlaybackBackendClient &operator=(const PlaybackBackendClient &) = delete;

    void setAudioDownlinkCallback(AudioDownlinkCallback callback);
    void setIntentCallback(IntentCallback callback);
    void setStateChangeCallback(StateChangeCallback callback);

    esp_err_t start();
    void stop();

    [[nodiscard]] bool isConnected() const;

    esp_err_t sendPlaybackRef(const int16_t *pcm,
                              uint32_t num_samples,
                              uint32_t sample_rate_hz,
                              uint16_t channel_count,
                              uint32_t sequence,
                              int64_t timestamp_us);

    esp_err_t publishPlaybackState(const char *source,
                                   const char *state,
                                   const char *title,
                                   const char *artist,
                                   const char *album,
                                   int duration_ms,
                                   int position_ms);

private:
    static constexpr const char *TAG = "PLAYBACK_BACKEND";
    static constexpr size_t RX_MAX_PAYLOAD_BYTES = 256 * 1024;
    static constexpr size_t MAX_DOWNLINK_TEXT_BYTES = 2048;
    static constexpr size_t DOWNLINK_QUEUE_DEPTH = 48;
    static constexpr size_t DOWNLINK_TASK_STACK_WORDS = 8192;
    static constexpr TickType_t DOWNLINK_ENQUEUE_TIMEOUT_MS = 500;
    static constexpr int RECONNECT_CONNECT_WAIT_MS = 15000;
    static constexpr int RECONNECT_LOOP_IDLE_MS = 1000;
    static constexpr uint32_t CONNECT_OUTCOME_PENDING = 0;
    static constexpr uint32_t CONNECT_OUTCOME_SUCCESS = 1;
    static constexpr uint32_t CONNECT_OUTCOME_FAILURE = 2;

    enum class DownlinkMessageType : uint8_t {
        Binary = 0,
        Text = 1,
        Shutdown = 2,
    };

    struct DownlinkMessage {
        DownlinkMessageType type = DownlinkMessageType::Binary;
        uint32_t length = 0;
        void *payload = nullptr;
    };
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    static constexpr size_t PLAYBACK_REF_QUEUE_DEPTH = 3;
    static constexpr int PLAYBACK_REF_SEND_TIMEOUT_MS = 250;

    struct PlaybackRefFrame {
        int16_t pcm[kPlaybackRefFrameSamples]{};
        uint32_t num_samples = 0;
        uint32_t sample_rate_hz = 0;
        uint16_t channel_count = 0;
        uint32_t sequence = 0;
        int64_t timestamp_us = 0;
    };
#endif

    static void websocketEventHandler(void *handlerArgs,
                                      esp_event_base_t base,
                                      int32_t eventId,
                                      void *eventData);

    void handleWebsocketEvent(esp_websocket_event_id_t eventId, esp_websocket_event_data_t *eventData);
    void handleDownlinkFragment(esp_websocket_event_data_t *eventData, bool is_binary);
    bool enqueueDownlinkMessage(DownlinkMessage &&message, TickType_t timeout_ms);
    void freeDownlinkPayload(DownlinkMessage &message);
    void startDownlinkWorker();
    void stopDownlinkWorker();
    static void downlinkWorkerTask(void *arg);
    void runDownlinkWorker();
    void processDownlinkMessage(const DownlinkMessage &message);
    void processTextDownlink(const char *data, size_t length);
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
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    esp_err_t startPlaybackRefWorker();
    void stopPlaybackRefWorker();
    static void playbackRefWorkerTask(void *arg);
    void runPlaybackRefWorker();
    esp_err_t sendPlaybackRefNow(const PlaybackRefFrame &frame);
#endif

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
    AudioDownlinkCallback audioDownlinkCallback_;
    IntentCallback intentCallback_;
    StateChangeCallback stateChangeCallback_;
    std::atomic<bool> downlinkAccepting_{false};
    std::atomic<bool> downlinkWorkerShutdown_{false};
    TaskHandle_t downlinkTaskHandle_ = nullptr;
    QueueHandle_t downlinkQueue_ = nullptr;
    std::string binaryBuffer_;
    std::string textBuffer_;
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
    std::atomic<bool> playbackRefWorkerShutdown_{false};
    TaskHandle_t playbackRefTaskHandle_ = nullptr;
    QueueHandle_t playbackRefQueue_ = nullptr;
    QueueHandle_t playbackRefFreeQueue_ = nullptr;
    PlaybackRefFrame playbackRefPool_[PLAYBACK_REF_QUEUE_DEPTH]{};
#endif
};

}  // namespace btspeaker
