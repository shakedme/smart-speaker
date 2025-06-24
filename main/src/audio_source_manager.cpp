#include "../include/audio_source_manager.hpp"
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

    esp_err_t AudioSourceManager::switchToSource(AudioSource source) {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }

        if (source == currentSource) {
            ESP_LOGD(TAG, "Already using source %d", (int) source);
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Switching from source %d to %d", (int) currentSource, (int) source);

        AudioSource old_source = currentSource;

        // Step 1: Ensure clean shutdown of current source
        ensureI2SDriverStopped();

        // Store previous source for restoration after voice response
        if (source == AudioSource::VOICE_RESPONSE && currentSource != AudioSource::VOICE_RESPONSE) {
            previousSource = currentSource;
            ESP_LOGD(TAG, "Storing previous source %d for restoration", (int) previousSource);
            // Extra delay for voice response due to sample rate change (44.1kHz -> 24kHz)
            ESP_LOGI(TAG, "Adding extra delay for voice response I2S reconfiguration");
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        // Step 3: Start new source with proper hardware state tracking
        esp_err_t ret = startSource(source);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start new source %d: %s", (int) source, esp_err_to_name(ret));

            // Attempt recovery - try to restore previous source
            vTaskDelay(pdMS_TO_TICKS(1000));

            ESP_LOGW(TAG, "Attempting to restore previous source %d", (int) old_source);
            esp_err_t recovery_ret = startSource(old_source);
            if (recovery_ret != ESP_OK) {
                ESP_LOGE(TAG, "Recovery failed, system in unstable state");
                currentSource = AudioSource::NONE;
            }
            return ret;
        }

        currentSource = source;
        ESP_LOGI(TAG, "Post switch: Available internal memory: %d",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        ESP_LOGI(TAG, "Successfully switched to source %d", (int) source);
        return ESP_OK;
    }

    esp_err_t AudioSourceManager::stopCurrentSource() {
        switch (currentSource) {
            case AudioSource::BLUETOOTH:
                if (btPipeline->isRunning()) {
                    btPipeline->cleanupPipeline();
                    btPipeline->disconnectCurrentDevice();
                }
                return ESP_OK;
            case AudioSource::SPOTIFY:
                if (spotifyPipeline->isRunning()) {
                    return spotifyPipeline->stop();
                }
            case AudioSource::VOICE_RESPONSE:
                if (llmPipeline->getState() == LLMPipeline::State::PLAYING) {
                    return llmPipeline->stop();
                } else {
                }
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
                btPipeline->resume();
                ret = ESP_OK;
                break;
            case AudioSource::SPOTIFY:
                ret = spotifyPipeline->resume();
                break;
            case AudioSource::VOICE_RESPONSE:
                ret = llmPipeline->resume();
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
                btPipeline->pause();
                ret = ESP_OK;
                break;
            case AudioSource::SPOTIFY:
                ret = spotifyPipeline->pause();
                break;
            case AudioSource::VOICE_RESPONSE:
                ret = llmPipeline->pause();
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
                // Bluetooth doesn't support skip (would need AVRCP implementation)
                ESP_LOGW(TAG, "Skip not supported for Bluetooth source");
                return ESP_ERR_NOT_SUPPORTED;
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
                ESP_LOGW(TAG, "Skip not supported for Bluetooth source");
                return ESP_ERR_NOT_SUPPORTED;
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

        // Apply volume to all sources
        if (btPipeline) {
            // TODO: Implement Bluetooth volume control
            ESP_LOGD(TAG, "Bluetooth volume control not implemented");
        }

        if (spotifyPipeline) {
            spotifyPipeline->setVolume(volume);
        }

        if (llmPipeline) {
            // TODO: Implement voice response volume control if needed
            ESP_LOGD(TAG, "Voice response volume control not implemented");
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

        esp_err_t ret = llmPipeline->queueAudioChunk(audio_data, length);

        return ret;
    }

    esp_err_t AudioSourceManager::stopVoiceResponse() {
        if (currentSource != AudioSource::VOICE_RESPONSE) {
            return ESP_OK;  // Not playing voice response
        }
        // Restore previous source
        esp_err_t ret = restorePreviousSource();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore previous source: %s", esp_err_to_name(ret));
        }

        return ESP_OK;
    }


    esp_err_t AudioSourceManager::spotifyPlay(const std::string &uri) {
        if (!isInitialized || !spotifyPipeline) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Playing Spotify URI: %s", uri.c_str());

        // Switch to Spotify if not already active
        if (currentSource != AudioSource::SPOTIFY) {
            esp_err_t ret = switchToSource(AudioSource::SPOTIFY);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to switch to Spotify: %s", esp_err_to_name(ret));
                return ret;
            }
        }

        esp_err_t ret = spotifyPipeline->playURI(uri);

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

    esp_err_t AudioSourceManager::startSource(AudioSource source) {
        ESP_LOGD(TAG, "Starting source %d with hardware coordination", (int) source);


        esp_err_t ret = ESP_OK;
        // Start the source
        switch (source) {
            case AudioSource::BLUETOOTH:
                btPipeline->setupPipeline();
                ret = ESP_OK;
                break;
            case AudioSource::SPOTIFY:
                ret = spotifyPipeline->start();
                break;
            case AudioSource::VOICE_RESPONSE:
                ret = llmPipeline->setupPipeline();
                break;
            default:
                ret = ESP_OK;
                break;
        }

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start source %d: %s", (int) source, esp_err_to_name(ret));
        }

        return ret;
    }

} // namespace btspeaker 