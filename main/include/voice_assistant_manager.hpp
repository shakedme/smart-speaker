#pragma once

#include "esp_raii_wrapper.hpp"
#include <memory>
#include <atomic>
#include <functional>

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
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

        esp_err_t start();

        void onAudioResponse(const uint8_t *data, size_t length);

        void onTextData(const char *messageJson);

    private:
        static constexpr const char *TAG = "VOICE_ASST";

        void setVoiceAssistantState(VoiceAssistantState new_state);

        void onAudioInput(const uint8_t *data, size_t length);

        // Member variables
        std::atomic<VoiceAssistantState> state;
        AudioSourceManager &audioSourceManager;

        // Sub-components
        std::unique_ptr<MicrophoneManager> microphoneManager;
        std::unique_ptr<WebSocketVoiceClient> websocketClient;

    };

} // namespace btspeaker 