#pragma once

#include "audio_types.hpp"
#include "../include/aec_ref_config.hpp"
#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
#include "../include/playback_reference_tap.hpp"
#endif
#include <array>
#include <memory>
#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
}

namespace btspeaker {

    class MicrophoneManager;

    class WebSocketVoiceClient;

    class AudioSourceManager;

    class VoiceAssistantManager {
    public:
        enum class VoiceAssistantState {
            IDLE,
            LISTENING,
            PROCESSING_INTENT,
            STREAMING_RESPONSE,
            HANDLING_COMMAND,
            WAITING_FOR_USER
        };

        explicit VoiceAssistantManager(AudioSourceManager &audioSourceManager);

        ~VoiceAssistantManager();

        VoiceAssistantManager(const VoiceAssistantManager &) = delete;

        VoiceAssistantManager &operator=(const VoiceAssistantManager &) = delete;

        esp_err_t start();

        void onNetworkLost();

        void onNetworkAvailable();

        // Called from websocket_task: copy payload and return immediately.
        void onAudioResponse(const uint8_t *data, size_t length);

        void onTextData(const uint8_t *data, size_t length);

    private:
        enum class InboundMessageType : uint8_t {
            TEXT_JSON = 0,
            AUDIO_PCM = 1,
            SHUTDOWN = 2,
        };

        struct InboundMessage {
            InboundMessageType type;
            uint32_t length;
            void *payload;
        };

        static constexpr const char *TAG = "VOICE_ASST";
        static constexpr size_t INBOUND_QUEUE_DEPTH = 48;
        static constexpr TickType_t INBOUND_AUDIO_ENQUEUE_TIMEOUT_MS = 500;
        static constexpr size_t MAX_INBOUND_TEXT_BYTES = 2048;
        static constexpr size_t INBOUND_TASK_STACK_WORDS = 8192;
        static constexpr int UPSTREAM_DIAG_INTERVAL_MS = 5000;

        void logUpstreamDiagnostics(int64_t now_us,
                                    uint32_t chunks_received,
                                    uint32_t chunks_sent,
                                    uint32_t chunks_skipped_state,
                                    uint32_t chunks_skipped_disconnected,
                                    uint32_t chunks_skipped_spotify_protect,
                                    uint32_t chunks_send_failed,
                                    uint64_t bytes_sent,
                                    uint64_t sum_send_us,
                                    uint32_t max_send_us) const;

        static const char *voiceStateName(VoiceAssistantState state);

        static const char *audioSourceName(AudioSource source);

        static void inboundWorkerTask(void *arg);

        void runInboundWorker();

        bool enqueueInboundMessage(InboundMessage &&message, TickType_t timeout_ms = 0);

        void freeInboundPayload(InboundMessage &message);

        void shutdownInboundWorker();

        void setVoiceAssistantState(VoiceAssistantState new_state);

        void onAudioInput(const uint8_t *data, size_t length);

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
        esp_err_t sendFramedMicChunks(const uint8_t *data, size_t length, int64_t capture_timestamp_us);

        void updatePlaybackRefActive(bool playback_active, AudioSource playback_source);

        void resetFramedMicAggregator();
#endif

        void processTextMessage(const char *messageJson);

        void processAudioResponse(const uint8_t *data, size_t length);

        std::atomic<VoiceAssistantState> state;
        AudioSourceManager &audioSourceManager;

        std::unique_ptr<MicrophoneManager> microphoneManager;
        std::unique_ptr<WebSocketVoiceClient> websocketClient;

#if CONFIG_BTSPK_BACKEND_AEC_REF_STREAM
        static constexpr uint32_t FRAMED_MIC_AGGREGATE_SAMPLES = 4096;  // 256 ms @ 16 kHz.
        std::unique_ptr<PlaybackReferenceTap> playbackRefTap_;
        uint32_t micSequence_{0};
        std::array<int16_t, FRAMED_MIC_AGGREGATE_SAMPLES> framedMicBuffer_{};
        uint32_t framedMicFill_{0};
        int64_t framedMicTimestampUs_{0};
#endif

        QueueHandle_t inboundQueue_ = nullptr;
        TaskHandle_t inboundTaskHandle_ = nullptr;
        SemaphoreHandle_t inboundStoppedSem_ = nullptr;
        StackType_t *inboundTaskStack_ = nullptr;
        StaticTask_t *inboundTaskBuffer_ = nullptr;
        std::atomic<bool> inboundAccepting_{false};
    };

} // namespace btspeaker
