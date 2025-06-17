#include "../include/audio_source_manager.hpp"
#include "../include/i2s_audio_service.hpp"
#include "esp_log.h"

namespace btspeaker {

    static constexpr const char *TAG = "AUDIO_SOURCE_MGR";

    AudioSourceManager &AudioSourceManager::getInstance() {
        static AudioSourceManager instance;
        return instance;
    }

    esp_err_t AudioSourceManager::initialize() {
        if (isInitialized) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Initializing AudioSourceManager");

        // Initialize I2S hardware state
        i2SHardwareState = I2SHardwareState::IDLE;

        // Initialize shared I2S service first
        auto &i2s_service = I2SAudioService::getInstance();
        esp_err_t ret = i2s_service.initialize();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2S service: %s", esp_err_to_name(ret));
            return ret;
        }

        // Initialize Bluetooth pipeline
        btPipeline = std::make_unique<BluetoothPipeline>();
        ret = btPipeline->setupPipeline();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to setup Bluetooth pipeline: %s", esp_err_to_name(ret));
            return ret;
        }

        // Initialize Spotify pipeline
        spotifyPipeline = std::make_unique<SpotifyPipeline>();
        ret = spotifyPipeline->initialize();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize Spotify pipeline: %s", esp_err_to_name(ret));
            return ret;
        }

        // Initialize voice response player
        llmPipeline = std::make_unique<LLMPipeline>();
        ret = llmPipeline->init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize voice response player: %s", esp_err_to_name(ret));
            return ret;
        }

        // Initialize state
        currentSource = AudioSource::NONE;
        previousSource = AudioSource::NONE;
        volume = 100;
        autoRestorePreviousSource = true;
        isInitialized = true;

        ESP_LOGI(TAG, "AudioSourceManager initialized successfully");
        return ESP_OK;
    }

    esp_err_t AudioSourceManager::start() {
        if (!isInitialized) {
            ESP_LOGE(TAG, "Not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Starting AudioSourceManager");

        // Start Bluetooth pipeline (always available)
        esp_err_t ret = btPipeline->startPipeline();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Bluetooth pipeline: %s", esp_err_to_name(ret));
            return ret;
        }

        // Start Spotify pipeline (will start when needed)
        ret = spotifyPipeline->start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start Spotify pipeline: %s", esp_err_to_name(ret));
            // Continue - Spotify may not be available
        }

        // Default to Bluetooth source
        ret = switchToSource(AudioSource::BLUETOOTH);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to Bluetooth source: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "AudioSourceManager started successfully");
        return ESP_OK;
    }

    esp_err_t AudioSourceManager::stop() {
        if (!isInitialized) {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Stopping AudioSourceManager");

        // Stop current source
        esp_err_t ret = stopCurrentSource();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop current source: %s", esp_err_to_name(ret));
        }

        // Stop all pipelines
        if (btPipeline) {
            btPipeline->stopPipeline();
        }
        if (spotifyPipeline) {
            spotifyPipeline->stop();
        }
        if (llmPipeline) {
            llmPipeline->stopPlayback();
        }

        currentSource = AudioSource::NONE;

        ESP_LOGI(TAG, "AudioSourceManager stopped");
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
        
        // Set switching state
        i2SHardwareState = I2SHardwareState::SWITCHING;

        AudioSource old_source = currentSource;

        // Step 1: Ensure clean shutdown of current source
        ensureI2SDriverStopped();

        // Store previous source for restoration after voice response
        if (source == AudioSource::VOICE_RESPONSE && currentSource != AudioSource::VOICE_RESPONSE) {
            previousSource = currentSource;
            ESP_LOGD(TAG, "Storing previous source %d for restoration", (int) previousSource);
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
                i2SHardwareState = I2SHardwareState::IDLE;
                currentSource = AudioSource::NONE;
            }
            return ret;
        }

        currentSource = source;

        // Step 4: Notify I2S service of source change (for non-cspot sources)
        if (source != AudioSource::SPOTIFY) {
            auto &i2s_service = I2SAudioService::getInstance();
            I2SAudioService::AudioSource i2s_source;
            switch (source) {
                case AudioSource::BLUETOOTH:
                    i2s_source = I2SAudioService::AudioSource::BLUETOOTH;
                    i2SHardwareState = I2SHardwareState::ESP_ADF_ACTIVE;
                    break;
                case AudioSource::VOICE_RESPONSE:
                    i2s_source = I2SAudioService::AudioSource::VOICE_RESPONSE;
                    i2SHardwareState = I2SHardwareState::ESP_ADF_ACTIVE;
                    break;
                default:
                    i2s_source = I2SAudioService::AudioSource::NONE;
                    i2SHardwareState = I2SHardwareState::IDLE;
                    break;
            }
            i2s_service.setActiveSource(i2s_source);
        } else {
            // Spotify uses cspot's own I2S management
            i2SHardwareState = I2SHardwareState::CSPOT_ACTIVE;
        }

        ESP_LOGI(TAG, "Successfully switched to source %d, I2S state: %d", 
                 (int) source, (int) i2SHardwareState.load());
        return ESP_OK;
    }

    esp_err_t AudioSourceManager::stopCurrentSource() {
        switch (currentSource) {
            case AudioSource::BLUETOOTH:
                return btPipeline->pausePipeline();  // Pause instead of stop to maintain connection
            case AudioSource::SPOTIFY:
                return spotifyPipeline->pause();      // Pause to maintain Spotify session
            case AudioSource::VOICE_RESPONSE:
                return llmPipeline->stopPlayback();
            case AudioSource::NONE:
                return ESP_OK;
            default:
                ESP_LOGW(TAG, "Unknown source %d", (int) currentSource);
                return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t AudioSourceManager::pauseCurrentSourceForVoice() {
        if (currentSource == AudioSource::VOICE_RESPONSE) {
            return ESP_OK;  // Already in voice mode
        }

        previousSource = currentSource;
        ESP_LOGD(TAG, "Pausing source %d for voice response", (int) currentSource);

        return stopCurrentSource();
    }

    esp_err_t AudioSourceManager::restorePreviousSource() {
        if (previousSource == AudioSource::NONE || !autoRestorePreviousSource) {
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
                ret = btPipeline->resumePipeline();
                break;
            case AudioSource::SPOTIFY:
                ret = spotifyPipeline->resume();
                break;
            case AudioSource::VOICE_RESPONSE:
                ret = llmPipeline->resumePlayback();
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
                ret = btPipeline->pausePipeline();
                break;
            case AudioSource::SPOTIFY:
                ret = spotifyPipeline->pause();
                break;
            case AudioSource::VOICE_RESPONSE:
                ret = llmPipeline->pausePlayback();
                break;
            default:
                ret = ESP_ERR_INVALID_STATE;
        }

        return ret;
    }

    esp_err_t AudioSourceManager::resume() {
        return play();
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

// ===== VOICE RESPONSE METHODS =====

    esp_err_t AudioSourceManager::queueVoiceResponseChunk(const uint8_t *audio_data, size_t length, bool is_final) {
        if (!isInitialized || !llmPipeline) {
            return ESP_ERR_INVALID_STATE;
        }

        // If this is the first chunk, set up voice response mode
        if (currentSource != AudioSource::VOICE_RESPONSE) {
            esp_err_t ret = pauseCurrentSourceForVoice();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to pause current source: %s", esp_err_to_name(ret));
            }

            switchToSource(AudioSource::VOICE_RESPONSE);
        }

        esp_err_t ret = llmPipeline->queueAudioChunk(audio_data, length, is_final);

        return ret;
    }

    esp_err_t AudioSourceManager::stopVoiceResponse() {
        if (currentSource != AudioSource::VOICE_RESPONSE) {
            return ESP_OK;  // Not playing voice response
        }

        ESP_LOGI(TAG, "Stopping voice response");

        esp_err_t ret = llmPipeline->stopPlayback();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop voice response: %s", esp_err_to_name(ret));
        }

        // Restore previous source
        ret = restorePreviousSource();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore previous source: %s", esp_err_to_name(ret));
        }

        return ESP_OK;
    }

// ===== SPOTIFY METHODS =====

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

// ===== CALLBACK MANAGEMENT =====

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
        
        // Force stop all pipelines to ensure clean shutdown
        if (btPipeline) {
            btPipeline->stopPipeline();
        }
        if (spotifyPipeline) {
            spotifyPipeline->stop();  // This should clean up cspot's I2S
        }
        if (llmPipeline) {
            llmPipeline->stopPlayback();
        }
        
        // Additional delay to ensure all async operations complete
        vTaskDelay(pdMS_TO_TICKS(I2S_SWITCH_DELAY_MS));
    }

    esp_err_t AudioSourceManager::verifyI2SHardwareState() {
        // This could be expanded to actually verify I2S registers
        // For now, just check our state tracking
        I2SHardwareState current_state = i2SHardwareState.load();
        
        if (current_state == I2SHardwareState::SWITCHING) {
            ESP_LOGW(TAG, "I2S hardware still in switching state - possible stuck transition");
            return ESP_ERR_INVALID_STATE;
        }
        
        return ESP_OK;
    }

    esp_err_t AudioSourceManager::startSource(AudioSource source) {
        ESP_LOGD(TAG, "Starting source %d with hardware coordination", (int) source);
        
        // Verify hardware is ready
        esp_err_t ret = verifyI2SHardwareState();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Hardware verification failed, continuing anyway");
        }
        
        // Start the source
        switch (source) {
            case AudioSource::BLUETOOTH:
                ret = btPipeline->resumePipeline();
                if (ret == ESP_OK) {
                    i2SHardwareState = I2SHardwareState::ESP_ADF_ACTIVE;
                }
                break;
                
            case AudioSource::SPOTIFY:
                ret = spotifyPipeline->resume();
                if (ret == ESP_OK) {
                    i2SHardwareState = I2SHardwareState::CSPOT_ACTIVE;
                }
                break;
                
            case AudioSource::VOICE_RESPONSE:
                ret = ESP_OK;  // Voice response player is started when audio is queued
                i2SHardwareState = I2SHardwareState::ESP_ADF_ACTIVE;
                break;
                
            case AudioSource::NONE:
                ret = ESP_OK;
                i2SHardwareState = I2SHardwareState::IDLE;
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown source %d", (int) source);
                ret = ESP_ERR_INVALID_STATE;
                i2SHardwareState = I2SHardwareState::IDLE;
        }
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start source %d: %s", (int) source, esp_err_to_name(ret));
            i2SHardwareState = I2SHardwareState::IDLE;
        }
        
        return ret;
    }

} // namespace btspeaker 