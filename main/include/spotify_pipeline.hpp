#pragma once

#include <memory>
#include <string>
#include <atomic>
#include "esp_raii_wrapper.hpp"
#include "DeviceStateHandler.h"
#include "esp_err.h"
#include "../include/spotify_config.hpp"
#include "../include/adf_audio_sink.hpp"
#include "spotify_auth_manager.hpp"
#include "EspPlayer.h"
#include "WrappedSemaphore.h"

namespace cspot {
    struct Context;

    class LoginBlob;

    class DeviceStateHandler;
}


namespace btspeaker {

    using AuthenticatedCallback = std::function<void()>;
    using FailureCallback = std::function<void()>;

    class SpotifyPipeline {
    public:
        SpotifyPipeline();

        ~SpotifyPipeline();

        // Initialization and lifecycle
        esp_err_t start();

        esp_err_t stop();

        esp_err_t pause();

        esp_err_t resume();

        esp_err_t suspendForVoiceResponse();

        esp_err_t resumeAfterVoiceResponse();

        // Playback control
        esp_err_t playURI(const std::string &uri);

        void setNextURI(const std::string &uri);

        esp_err_t skipNext();

        esp_err_t skipPrevious();

        void setVolume(uint8_t volume);

        void setAuthenticatedCallback(AuthenticatedCallback cb);

        void setFailureCallback(FailureCallback cb);

        [[nodiscard]] size_t getPlaybackBufferFillLevel() const;

        [[nodiscard]] size_t getPlaybackBufferCapacity() const;

        [[nodiscard]] bool isSinkPlaying() const;

        [[nodiscard]] bool isRebuffering() const;

        [[nodiscard]] bool isPlaybackPaused() const;

        [[nodiscard]] bool isTrackTransitionActive() const;

    private:
        std::string nextUri;
        std::shared_ptr<cspot::Context> context;
        std::shared_ptr<cspot::DeviceStateHandler> deviceStateHandler;
        std::atomic<bool> isPlaying;
        std::atomic<bool> isInitialized;
        std::atomic<bool> isRunning_;
        std::atomic<bool> trackTransitionActive_{false};
        std::atomic<uint8_t> volume;

        // Audio streaming components
        std::shared_ptr<ADFAudioSink> audioSink;
        std::unique_ptr<EspPlayer> espPlayer;

        // FreeRTOS task handle for packet processing
        TaskHandle_t packetHandlerTaskHandle = NULL;
        StackType_t *taskStack = nullptr;
        StaticTask_t *taskBuffer = nullptr;

        // Auth manager
        std::unique_ptr<SpotifyAuthManager> authManager;

        // FreeRTOS task function for main Spotify workflow
        static void spotifyMainTask(void *parameter);

        esp_err_t initCSpotComponents();

        FailureCallback failureCallback;

        void unreference(char **string) {
            if (*string != NULL) {
                free(*string);
                *string = NULL;
            }
        }
    };

} // namespace btspeaker 