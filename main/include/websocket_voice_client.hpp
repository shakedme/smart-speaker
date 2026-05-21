#pragma once

#include <string>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "../include/esp_websocket_client.h"
#include "../include/ima_adpcm.hpp"

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
            int reconnectBackoffInitialMs = 1000;
            int reconnectBackoffMaxMs = 10000;
        };

        explicit WebSocketVoiceClient(VoiceAssistantManager &voiceAssistantManager);

        ~WebSocketVoiceClient();

        void init();

        void shutdown();

        esp_err_t connect();

        void disconnect();

        [[nodiscard]] bool isConnected() const { return state.load() == ConnectionState::CONNECTED; }

        [[nodiscard]] bool isTransportReady() const;

        // Non-blocking: copies audio and enqueues for mic_uplink worker.
        esp_err_t sendAudioChunk(const uint8_t *audioData, size_t length);

        void setPlaybackActive(bool active);

        void setPlaybackStress(bool stressed);

        void setPlaybackMicSuspended(bool suspended);

        void onNetworkLost();

        void onNetworkAvailable();

    private:
        struct UplinkChunk {
            bool shutdown;
            uint8_t *payload;
            uint32_t length;
        };

        static constexpr const char *TAG = "WS_VOICE_CLIENT";
        static constexpr size_t UPLINK_QUEUE_DEPTH = 16;
        static constexpr size_t UPLINK_MAX_CHUNK_BYTES = 16 * 1024;
        static constexpr size_t UPLINK_TASK_STACK_WORDS = 4096;
        static constexpr int UPLINK_SEND_TIMEOUT_MS = 250;
        static constexpr int UPLINK_PLAYBACK_SEND_TIMEOUT_MS = 250;
        static constexpr int UPLINK_DIAG_INTERVAL_MS = 5000;
        static constexpr uint32_t UPLINK_SLOW_SEND_US = 100000;
        // During playback, batch four 16 kHz/16-bit 1024-sample frames per WS message.
        // The backend re-frames PCM for Porcupine/Cobra, so this only adds bounded latency.
        static constexpr uint32_t UPLINK_PLAYBACK_AGGREGATE_BYTES = 8192;
        static constexpr uint32_t UPLINK_STRESS_AGGREGATE_BYTES = 16 * 1024;
        static constexpr int UPLINK_PLAYBACK_AGGREGATE_WAIT_MS = 80;
        static constexpr uint32_t UPLINK_PLAYBACK_QUEUE_MAX = 2;
        static constexpr int RECONNECT_CONNECT_WAIT_MS = 15000;
        static constexpr int RECONNECT_LOOP_IDLE_MS = 1000;
        static constexpr uint32_t CONNECT_OUTCOME_PENDING = 0;
        static constexpr uint32_t CONNECT_OUTCOME_SUCCESS = 1;
        static constexpr uint32_t CONNECT_OUTCOME_FAILURE = 2;
        static constexpr size_t DOWNLINK_RX_BUFFER_SIZE = 8192;
        static constexpr size_t DOWNLINK_MAX_PAYLOAD_BYTES = 256 * 1024;

        static void websocketEventHandler(void *handlerArgs, esp_event_base_t base,
                                          int32_t eventId, void *eventData);

        static void uplinkWorkerTask(void *arg);

        static void reconnectLoopTask(void *pvParameters);

        void handleWebsocketEvent(esp_websocket_event_id_t eventId, esp_websocket_event_data_t *eventData);

        void handleDownlinkFragment(esp_websocket_event_data_t *eventData, bool is_binary);

        esp_err_t createWebsocketClient();

        void destroyWebsocketClient();

        void setConnectionState(ConnectionState newState);

        void startReconnectLoop();

        void stopReconnectLoop();

        uint32_t beginConnectAttempt();

        void signalConnectOutcome(uint32_t outcome);

        uint32_t waitForConnectOutcome(uint32_t generation, int timeout_ms);

        uint32_t computeReconnectBackoffMs() const;

        void recordConnectSuccess();

        void recordConnectFailure();

        void markTransportDisconnected(const char *reason);

        esp_err_t startUplinkWorker();

        void shutdownUplinkWorker();

        void runUplinkWorker();

        esp_err_t enqueueUplinkChunk(UplinkChunk chunk);

        bool dropOldestUplinkChunk();

        void freeUplinkChunk(UplinkChunk &chunk);

        uint8_t *acquireUplinkBuffer();

        void releaseUplinkBuffer(uint8_t *payload);

        void destroyUplinkBufferPool();

        void drainUplinkQueue();

        void logUplinkDiagnostics(int64_t now_us,
                                  uint32_t enqueued,
                                  uint32_t sent,
                                  uint32_t dropped_oldest,
                                  uint32_t dropped_playback,
                                  uint32_t dropped_disc,
                                  uint32_t send_fail,
                                  uint32_t send_slow,
                                  uint64_t bytes_sent,
                                  uint64_t sum_send_us,
                                  uint32_t max_send_us,
                                  uint32_t queue_depth) const;

        bool shouldDropForPlaybackCongestion(uint32_t queue_depth) const;

        esp_err_t buildUplinkPayload(const uint8_t *audioData,
                                     size_t length,
                                     uint8_t **out_payload,
                                     uint32_t *out_length);

        Config config;
        std::atomic<ConnectionState> state;
        esp_websocket_client_handle_t websocketClient;
        SemaphoreHandle_t stateMutex;
        TaskHandle_t reconnectLoopTaskHandle_;
        std::atomic_bool isDestroying{false};
        std::atomic_bool reconnectShutdown_{true};
        std::atomic_bool networkAvailable_{true};

        VoiceAssistantManager &voiceAssistantManager;

        bool initialized;
        bool isClientRunning;
        std::atomic<uint32_t> consecutiveFailures_{0};
        std::atomic<uint32_t> connectGeneration_{0};
        std::atomic<uint32_t> connectOutcome_{CONNECT_OUTCOME_PENDING};
        std::atomic<uint32_t> connectOutcomeGeneration_{0};

        QueueHandle_t uplinkQueue_ = nullptr;
        QueueHandle_t uplinkFreeQueue_ = nullptr;
        TaskHandle_t uplinkTaskHandle_ = nullptr;
        SemaphoreHandle_t uplinkStoppedSem_ = nullptr;
        StackType_t *uplinkTaskStack_ = nullptr;
        StaticTask_t *uplinkTaskBuffer_ = nullptr;
        std::atomic<bool> uplinkAccepting_{false};
        std::atomic<uint32_t> uplinkDiagEnqueued_{0};
        std::atomic<uint32_t> uplinkDiagDroppedOldest_{0};
        std::atomic<uint32_t> uplinkDiagDroppedPlayback_{0};
        std::atomic<bool> playbackActive_{false};
        std::atomic<bool> playbackStress_{false};
        std::atomic<bool> playbackMicSuspended_{false};
        ImaAdpcmState adpcmState_{};

        // Reassemble downlink WS payloads larger than the transport rx buffer (default 1 KiB).
        std::vector<uint8_t> downlinkBinaryBuffer_;
        std::vector<uint8_t> downlinkTextBuffer_;
    };

} // namespace btspeaker
