#include "../include/audio_source_manager.hpp"
#include "diagnostics_config.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

namespace btspeaker {

    static constexpr const char *TAG = "AUDIO_SOURCE_MGR";

    AudioSourceManager::AudioSourceManager(WiFiManager &wifiManager) :
            wifiManager(wifiManager),
            currentSource(AudioSource::NONE),
            previousSource(AudioSource::NONE),
            volume(100),
            isInitialized(false) {
        btPipeline = std::make_unique<BluetoothPipeline>();
        btPipeline->setConnectedCallback([this]() {
            this->switchToSource(AudioSource::BLUETOOTH);
        });

        if (wifiManager.isConnected()) {
            spotifyPipeline = std::make_unique<SpotifyPipeline>();
            spotifyPipeline->setAuthenticatedCallback([this]() {
                this->switchToSource(AudioSource::SPOTIFY);
            });
            spotifyPipeline->setFailureCallback([this]() {
                ESP_LOGE(TAG, "Spotify authentication failed, switching to Bluetooth");
                this->switchToSource(AudioSource::BLUETOOTH);
            });
            llmPipeline = std::make_unique<LLMPipeline>();
        }

        // Initialize state
        currentSource = AudioSource::NONE;
        previousSource = AudioSource::NONE;
        volume = 100;
        isInitialized = true;
    }

    esp_err_t AudioSourceManager::start() {
        if (!isInitialized) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Starting AudioSourceManager");
        switchToSource(AudioSource::BLUETOOTH);

        ESP_LOGI(TAG, "AudioSourceManager started successfully");
        return ESP_OK;
    }

    size_t AudioSourceManager::getSpotifyPlaybackBufferFillLevel() const {
        if (!spotifyPipeline) {
            return 0;
        }
        return spotifyPipeline->getPlaybackBufferFillLevel();
    }

    size_t AudioSourceManager::getSpotifyPlaybackBufferCapacity() const {
        if (!spotifyPipeline) {
            return 0;
        }
        return spotifyPipeline->getPlaybackBufferCapacity();
    }

    void AudioSourceManager::updateSpotifyMicUplinkMode() const {
        if (1 == 1) {
            spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Normal;
            micSuspendReason_ = SpotifyMicUplinkSuspendReason::None;
            return;
        }
        if (currentSource != AudioSource::SPOTIFY || !spotifyPipeline) {
            if (spotifyMicUplinkMode_ != SpotifyMicUplinkMode::Normal) {
                ESP_LOGI(TAG, "[SPOTIFY_MIC_UPLINK] mode=normal (not on Spotify)");
            }
            spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Normal;
            micSuspendReason_ = SpotifyMicUplinkSuspendReason::None;
            return;
        }

        const SpotifyMicUplinkMode previous = spotifyMicUplinkMode_;
        const SpotifyMicUplinkSuspendReason previousReason = micSuspendReason_;
        const size_t fill = spotifyPipeline->getPlaybackBufferFillLevel();
        const bool sink_playing = spotifyPipeline->isSinkPlaying();
        const bool spotify_paused = spotifyPipeline->isPlaybackPaused();
        const bool rebuffering = spotifyPipeline->isRebuffering();
        const bool track_transition = spotifyPipeline->isTrackTransitionActive();
        const bool dangerously_low = fill < SPOTIFY_MIC_SUSPEND_FILL_BYTES;

        if (spotify_paused) {
            micSuspendReason_ = SpotifyMicUplinkSuspendReason::None;
            spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Normal;
        } else if (track_transition) {
            micSuspendReason_ = SpotifyMicUplinkSuspendReason::TrackTransition;
            spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Suspended;
        } else {
            switch (spotifyMicUplinkMode_) {
                case SpotifyMicUplinkMode::Suspended:
                    if (micSuspendReason_ == SpotifyMicUplinkSuspendReason::TrackTransition &&
                        sink_playing && !rebuffering) {
                        micSuspendReason_ = SpotifyMicUplinkSuspendReason::None;
                        if (dangerously_low) {
                            micSuspendReason_ = SpotifyMicUplinkSuspendReason::BufferLow;
                            spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Suspended;
                        } else if (fill < SPOTIFY_MIC_STRESS_ON_FILL_BYTES) {
                            spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Stress;
                        } else {
                            spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Normal;
                        }
                    } else if (sink_playing && !rebuffering &&
                               fill >= SPOTIFY_MIC_RESUME_FILL_BYTES) {
                        micSuspendReason_ = SpotifyMicUplinkSuspendReason::None;
                        spotifyMicUplinkMode_ = fill < SPOTIFY_MIC_STRESS_CLEAR_FILL_BYTES
                                                 ? SpotifyMicUplinkMode::Stress
                                                 : SpotifyMicUplinkMode::Normal;
                    } else if (!sink_playing || rebuffering || dangerously_low) {
                        if (micSuspendReason_ != SpotifyMicUplinkSuspendReason::TrackTransition) {
                            micSuspendReason_ = SpotifyMicUplinkSuspendReason::BufferLow;
                        }
                        spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Suspended;
                    }
                    break;
                case SpotifyMicUplinkMode::Stress:
                    if (!sink_playing || rebuffering || dangerously_low) {
                        micSuspendReason_ = SpotifyMicUplinkSuspendReason::BufferLow;
                        spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Suspended;
                    } else if (fill >= SPOTIFY_MIC_STRESS_CLEAR_FILL_BYTES) {
                        spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Normal;
                    }
                    break;
                case SpotifyMicUplinkMode::Normal:
                    if (!sink_playing || rebuffering || dangerously_low) {
                        micSuspendReason_ = SpotifyMicUplinkSuspendReason::BufferLow;
                        spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Suspended;
                    } else if (fill < SPOTIFY_MIC_STRESS_ON_FILL_BYTES) {
                        spotifyMicUplinkMode_ = SpotifyMicUplinkMode::Stress;
                    }
                    break;
            }
        }

        if (previous != spotifyMicUplinkMode_ || previousReason != micSuspendReason_) {
            const char *mode_name = "?";
            switch (spotifyMicUplinkMode_) {
                case SpotifyMicUplinkMode::Normal:
                    mode_name = "normal";
                    break;
                case SpotifyMicUplinkMode::Stress:
                    mode_name = "stress";
                    break;
                case SpotifyMicUplinkMode::Suspended:
                    mode_name = "suspended";
                    break;
            }
            const char *reason_name = "?";
            switch (micSuspendReason_) {
                case SpotifyMicUplinkSuspendReason::None:
                    reason_name = "none";
                    break;
                case SpotifyMicUplinkSuspendReason::TrackTransition:
                    reason_name = "track_change";
                    break;
                case SpotifyMicUplinkSuspendReason::BufferLow:
                    reason_name = "buffer_low";
                    break;
            }
            ESP_LOGI(TAG,
                     "[SPOTIFY_MIC_UPLINK] mode=%s reason=%s fill=%u playing=%d paused=%d rebuffer=%d track_x=%d",
                     mode_name,
                     reason_name,
                     static_cast<unsigned>(fill),
                     sink_playing ? 1 : 0,
                     spotify_paused ? 1 : 0,
                     rebuffering ? 1 : 0,
                     track_transition ? 1 : 0);
        }
    }

    AudioSourceManager::SpotifyMicUplinkPolicy AudioSourceManager::getSpotifyMicUplinkPolicy() {
        updateSpotifyMicUplinkMode();

        switch (spotifyMicUplinkMode_) {
            case SpotifyMicUplinkMode::Stress:
                return SpotifyMicUplinkPolicy::Stress;
            case SpotifyMicUplinkMode::Suspended:
                return SpotifyMicUplinkPolicy::Suspend;
            case SpotifyMicUplinkMode::Normal:
            default:
                return SpotifyMicUplinkPolicy::Allow;
        }
    }

    bool AudioSourceManager::isSpotifyUplinkStressed() const {
        updateSpotifyMicUplinkMode();
        return spotifyMicUplinkMode_ == SpotifyMicUplinkMode::Stress;
    }

    bool AudioSourceManager::shouldSuspendMicUplinkForSpotify() const {
        updateSpotifyMicUplinkMode();
        return spotifyMicUplinkMode_ == SpotifyMicUplinkMode::Suspended;
    }

    esp_err_t AudioSourceManager::switchToSource(AudioSource source) {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }

        if (source == currentSource) {
            ESP_LOGD(TAG, "Already using source %d", (int) source);
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Switching from source %d to %d", (int) currentSource, (int) source);

        // Store previous source for restoration after voice response
        if (source == AudioSource::VOICE_RESPONSE && currentSource != AudioSource::VOICE_RESPONSE) {
            previousSource = currentSource;
            ESP_LOGD(TAG, "Storing previous source %d for restoration", (int) previousSource);
            if (currentSource == AudioSource::SPOTIFY && spotifyPipeline) {
                spotifyPipeline->suspendForVoiceResponse();
            } else if (currentSource == AudioSource::BLUETOOTH && btPipeline) {
                btPipeline->suspendForVoiceResponse();
            } else {
                ensureI2SDriverStopped();
            }
            // Extra delay for voice response due to sample rate change (44.1kHz -> 24kHz)
            ESP_LOGI(TAG, "Adding extra delay for voice response I2S reconfiguration");
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            ensureI2SDriverStopped();
        }

        startSource(source);
        currentSource = source;
        ESP_LOGI(TAG, "Successfully switched to source %d", (int) source);
        if (diagnostics_enabled) {
            ESP_LOGI(TAG, "[AUDIO_SRC_DIAG] active playback source is now %d (0=NONE 2=BT 3=SPOTIFY 1=VOICE)",
                     (int) source);
        }
        return ESP_OK;
    }

    esp_err_t AudioSourceManager::stopCurrentSource() {
        switch (currentSource) {
            case AudioSource::BLUETOOTH:
                btPipeline->cleanupPipeline();
                btPipeline->disconnectCurrentDevice();
                return ESP_OK;
            case AudioSource::SPOTIFY:
                return spotifyPipeline->stop();
            case AudioSource::VOICE_RESPONSE:
                llmPipeline->cleanupPipeline();
                return ESP_OK;
            case AudioSource::NONE:
                return ESP_OK;
            default:
                ESP_LOGW(TAG, "Unknown source %d", (int) currentSource);
                return ESP_ERR_INVALID_STATE;
        }
    }

    void AudioSourceManager::updatePreviousSource() {
        if (currentSource == AudioSource::VOICE_RESPONSE) {
            return;
        }

        previousSource = currentSource;
    }

    esp_err_t AudioSourceManager::restorePreviousSource() {
        if (previousSource == AudioSource::NONE) {
            ESP_LOGD(TAG, "No previous source to restore or auto-restore disabled");
            currentSource = AudioSource::NONE;
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Restoring previous source %d", (int) previousSource);

        AudioSource restore_source = previousSource;
        previousSource = AudioSource::NONE;

        if (currentSource == AudioSource::VOICE_RESPONSE && llmPipeline) {
            llmPipeline->cleanupPipeline();
            currentSource = AudioSource::NONE;
        }

        if (restore_source == AudioSource::SPOTIFY && spotifyPipeline) {
            esp_err_t ret = spotifyPipeline->resumeAfterVoiceResponse();
            if (ret == ESP_OK) {
                currentSource = AudioSource::SPOTIFY;
                ESP_LOGI(TAG, "Successfully restored Spotify after voice response");
                if (diagnostics_enabled) {
                    ESP_LOGI(TAG, "[AUDIO_SRC_DIAG] active playback source is now %d (0=NONE 2=BT 3=SPOTIFY 1=VOICE)",
                             (int) currentSource);
                }
            }
            return ret;
        }

        if (restore_source == AudioSource::BLUETOOTH && btPipeline) {
            esp_err_t ret = btPipeline->resumeAfterVoiceResponse();
            if (ret == ESP_OK) {
                currentSource = AudioSource::BLUETOOTH;
                ESP_LOGI(TAG, "Successfully restored Bluetooth after voice response");
                if (diagnostics_enabled) {
                    ESP_LOGI(TAG, "[AUDIO_SRC_DIAG] active playback source is now %d (0=NONE 2=BT 3=SPOTIFY 1=VOICE)",
                             (int) currentSource);
                }
            }
            return ret;
        }

        return switchToSource(restore_source);
    }

// ===== PLAYBACK CONTROL METHODS =====

    esp_err_t AudioSourceManager::play() {
        if (!isInitialized || currentSource == AudioSource::NONE) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = ESP_OK;
        switch (currentSource) {
            case AudioSource::BLUETOOTH:
                btPipeline->avrcpResume();
                ret = ESP_OK;
                break;
            case AudioSource::SPOTIFY:
                ret = spotifyPipeline->resume();
                break;
            case AudioSource::VOICE_RESPONSE:
                ret = ESP_OK;
                break;
            default:
                ret = ESP_ERR_INVALID_STATE;
        }

        return ret;
    }

    esp_err_t AudioSourceManager::pause() {
        if (!isInitialized || currentSource == AudioSource::NONE) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = ESP_OK;
        switch (currentSource) {
            case AudioSource::BLUETOOTH:
                btPipeline->avrcpPause();
                ret = ESP_OK;
                break;
            case AudioSource::SPOTIFY:
                ret = spotifyPipeline->pause();
                break;
            case AudioSource::VOICE_RESPONSE:
                ret = ESP_OK;
                break;
            default:
                ret = ESP_ERR_INVALID_STATE;
        }

        return ret;
    }

    esp_err_t AudioSourceManager::stopPlayback() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = stopCurrentSource();

        return ret;
    }

    esp_err_t AudioSourceManager::skipNext() {
        if (!isInitialized || currentSource == AudioSource::NONE) {
            return ESP_ERR_INVALID_STATE;
        }

        switch (currentSource) {
            case AudioSource::SPOTIFY:
                return spotifyPipeline->skipNext();
            case AudioSource::BLUETOOTH:
                btPipeline->avrcpNext();
                return ESP_OK;
            case AudioSource::VOICE_RESPONSE:
                ESP_LOGW(TAG, "Skip not supported for voice responses");
                return ESP_ERR_NOT_SUPPORTED;
            default:
                return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t AudioSourceManager::skipPrevious() {
        if (!isInitialized || currentSource == AudioSource::NONE) {
            return ESP_ERR_INVALID_STATE;
        }

        switch (currentSource) {
            case AudioSource::SPOTIFY:
                return spotifyPipeline->skipPrevious();
            case AudioSource::BLUETOOTH:
                btPipeline->avrcpPrevious();
                return ESP_OK;
            case AudioSource::VOICE_RESPONSE:
                ESP_LOGW(TAG, "Skip not supported for voice responses");
                return ESP_ERR_NOT_SUPPORTED;
            default:
                return ESP_ERR_INVALID_STATE;
        }
    }

    void AudioSourceManager::setVolume(uint8_t newVolume) {
        volume = newVolume;

        ESP_LOGI(TAG, "Setting volume to %d", volume);

        if (btPipeline) {
            btPipeline->setAvrcpVolume(volume);
        }

        if (spotifyPipeline) {
            spotifyPipeline->setVolume(volume);
        }

    }

    esp_err_t AudioSourceManager::queueVoiceResponseChunk(const uint8_t *audio_data, size_t length) {
        if (!isInitialized || !llmPipeline) {
            return ESP_ERR_INVALID_STATE;
        }

        if (currentSource != AudioSource::VOICE_RESPONSE) {
            updatePreviousSource();
            switchToSource(AudioSource::VOICE_RESPONSE);
        }

        esp_err_t ret = llmPipeline->writeAudioToPipeline(audio_data, length);

        return ret;
    }

    esp_err_t AudioSourceManager::stopVoiceResponse() {
        if (currentSource != AudioSource::VOICE_RESPONSE) {
            return ESP_OK;
        }

        esp_err_t ret = restorePreviousSource();

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore previous source: %s", esp_err_to_name(ret));
        }

        return ESP_OK;
    }

    bool AudioSourceManager::isVoiceResponsePlaybackIdle() const {
        if (currentSource != AudioSource::VOICE_RESPONSE || !llmPipeline) {
            return true;
        }
        return llmPipeline->isPlaybackIdle();
    }

    namespace {
    void copyPlaybackField(char *dst, const size_t dst_len, const char *src) {
        if (!dst || dst_len == 0) {
            return;
        }
        if (!src) {
            dst[0] = '\0';
            return;
        }
        std::strncpy(dst, src, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
    }  // namespace

    bool AudioSourceManager::fillPlaybackUiSnapshot(PlaybackUiSnapshot &out) const {
        copyPlaybackField(out.source, sizeof(out.source), "unknown");
        copyPlaybackField(out.playback_state, sizeof(out.playback_state), "idle");
        out.title[0] = '\0';
        out.artist[0] = '\0';
        out.album[0] = '\0';
        out.duration_ms = -1;
        out.position_ms = -1;

        if (currentSource == AudioSource::SPOTIFY && spotifyPipeline) {
            copyPlaybackField(out.source, sizeof(out.source), "spotify");
            copyPlaybackField(out.playback_state, sizeof(out.playback_state),
                              spotifyPipeline->playbackStateName());
            SpotifyPipeline::TrackMetadata metadata{};
            if (spotifyPipeline->tryGetCurrentTrackMetadata(metadata)) {
                copyPlaybackField(out.title, sizeof(out.title), metadata.title);
                copyPlaybackField(out.artist, sizeof(out.artist), metadata.artist);
                copyPlaybackField(out.album, sizeof(out.album), metadata.album);
                out.duration_ms = static_cast<int>(metadata.duration_ms);
                return true;
            }
            return std::strcmp(out.playback_state, "playing") == 0 ||
                   std::strcmp(out.playback_state, "paused") == 0;
        }

        if (currentSource == AudioSource::VOICE_RESPONSE) {
            if (previousSource == AudioSource::SPOTIFY && spotifyPipeline) {
                copyPlaybackField(out.source, sizeof(out.source), "spotify");
                copyPlaybackField(out.playback_state, sizeof(out.playback_state), "paused");
                SpotifyPipeline::TrackMetadata metadata{};
                if (spotifyPipeline->tryGetCurrentTrackMetadata(metadata)) {
                    copyPlaybackField(out.title, sizeof(out.title), metadata.title);
                    copyPlaybackField(out.artist, sizeof(out.artist), metadata.artist);
                    copyPlaybackField(out.album, sizeof(out.album), metadata.album);
                    out.duration_ms = static_cast<int>(metadata.duration_ms);
                    return true;
                }
            }
            copyPlaybackField(out.source, sizeof(out.source), "llm");
            copyPlaybackField(out.playback_state, sizeof(out.playback_state), "playing");
            copyPlaybackField(out.title, sizeof(out.title), "Rufus");
            return true;
        }

        if (currentSource == AudioSource::BLUETOOTH && btPipeline) {
            copyPlaybackField(out.source, sizeof(out.source), "bluetooth");
            copyPlaybackField(out.playback_state, sizeof(out.playback_state), "playing");
            return true;
        }

        return false;
    }


    esp_err_t AudioSourceManager::spotifyPlay(const std::string &uri) {
        if (!isInitialized || !spotifyPipeline) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Playing Spotify URI: %s", uri.c_str());

        // Switch to Spotify if not already active
        esp_err_t ret;
        if (currentSource != AudioSource::SPOTIFY) {
            spotifyPipeline->setNextURI(uri);
            ret = switchToSource(AudioSource::SPOTIFY);
        } else {
            ret = spotifyPipeline->playURI(uri);
        }

        return ret;
    }

    void AudioSourceManager::onMusicCommand(const char *command) {
        esp_err_t ret = ESP_OK;

        // Check for Spotify URI (spotify:track:xxx, spotify:playlist:xxx, etc.)
        if (strncmp(command, "spotify:", 8) == 0) {
            ESP_LOGI(TAG, "Voice command: Play Spotify URI: %s", command);
            ret = spotifyPlay(std::string(command));
        }
            // Check for basic playback control commands
        else if (strstr(command, "play") || strstr(command, "resume")) {
            ret = play();
            ESP_LOGI(TAG, "Voice command: Resume playback");
        } else if (strstr(command, "pause")) {
            ret = pause();
            ESP_LOGI(TAG, "Voice command: Pause playback");
        } else if (strstr(command, "stop")) {
            ret = stopPlayback();
            ESP_LOGI(TAG, "Voice command: Stop playback");
        } else if (strstr(command, "next") || strstr(command, "skip")) {
            ret = skipNext();
            ESP_LOGI(TAG, "Voice command: Skip next");
        } else if (strstr(command, "previous") || strstr(command, "back")) {
            ret = skipPrevious();
            ESP_LOGI(TAG, "Voice command: Skip previous");
        } else if (strstr(command, "volume up")) {
            auto current_volume = getVolume();
            auto new_volume = std::min(100, static_cast<int>(current_volume) + 10);
            setVolume(new_volume);
            ESP_LOGI(TAG, "Voice command: Volume up to %d", new_volume);
        } else if (strstr(command, "volume down")) {
            auto current_volume = getVolume();
            auto new_volume = std::max(0, static_cast<int>(current_volume) - 10);
            setVolume(new_volume);
            ESP_LOGI(TAG, "Voice command: Volume down to %d", new_volume);
        } else if (strstr(command, "bluetooth")) {
            ret = switchToSource(AudioSource::BLUETOOTH);
            ESP_LOGI(TAG, "Voice command: Switch to Bluetooth");
        } else if (strstr(command, "spotify") && strncmp(command, "spotify:", 8) != 0) {
            ret = switchToSource(AudioSource::SPOTIFY);
            ESP_LOGI(TAG, "Voice command: Switch to Spotify");
        } else {
            ESP_LOGW(TAG, "Unknown music command: %s", command);
            return;
        }

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to execute music command: %s", esp_err_to_name(ret));
        }
    }

// ===== I2S HARDWARE LIFECYCLE MANAGEMENT =====

    void AudioSourceManager::ensureI2SDriverStopped() {
        ESP_LOGI(TAG, "Ensuring I2S driver is cleanly stopped");

        // Stop current source first
        esp_err_t ret = stopCurrentSource();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop current source: %s", esp_err_to_name(ret));
        }

        // Additional delay to ensure all async operations complete
        vTaskDelay(pdMS_TO_TICKS(I2S_SWITCH_DELAY_MS));
    }

    void AudioSourceManager::startSource(AudioSource source) {
        ESP_LOGD(TAG, "Starting source %d with hardware coordination", (int) source);

        switch (source) {
            case AudioSource::BLUETOOTH:
                btPipeline->setupPipeline();
                break;
            case AudioSource::SPOTIFY:
                spotifyPipeline->start();
                break;
            case AudioSource::VOICE_RESPONSE:
                llmPipeline->setupPipeline();
                break;
            default:
                break;
        }

    }

} // namespace btspeaker 