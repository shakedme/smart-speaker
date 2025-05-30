//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_SMART_SPEAKER_HPP
#define BT_SPEAKER_SMART_SPEAKER_HPP


#pragma once

#include "../audio/audio_types.hpp"
#include "../shared/event_bus.hpp"
#include "../shared/task_wrapper.hpp"
#include "../audio/audio_mixer.hpp"
#include "../audio/i2s_driver.hpp"
#include <memory>
#include <atomic>

extern "C" {
#include "nvs_flash.h"
#include "esp_log.h"
}

namespace smart_speaker {

// Main application state management
    enum class AppState : uint8_t {
        Initializing,
        Ready,
        BluetoothActive,
        VoiceListening,
        VoiceProcessing,
        SpotifyStreaming,
        Error,
        Shutdown
    };

// Application configuration
    struct AppConfig {
        // Audio settings
        bool enable_bluetooth = true;
        bool enable_voice = true;
        bool enable_spotify = true;
        bool enable_llm = true;

        // Performance settings
        bool enable_performance_monitoring = true;
        uint32_t monitor_interval_ms = 5000;

        // System settings
        bool auto_start_bluetooth = true;
        float default_volume = 0.7f;

        // Network settings
        std::string wifi_ssid;
        std::string wifi_password;
        std::string spotify_client_id;
        std::string llm_api_key;
    };

    class SmartSpeaker {
    private:
        // Core components
        std::unique_ptr <StandardAudioMixer> audio_mixer_;
        std::unique_ptr <TaskCoordinator> task_coordinator_;

        // Module instances (will be added progressively)
        // std::unique_ptr<BluetoothSink> bluetooth_sink_;
        // std::unique_ptr<VoiceDetector> voice_detector_;
        // std::unique_ptr<SpotifyClient> spotify_client_;
        // std::unique_ptr<LLMClient> llm_client_;

        // Application state
        std::atomic <AppState> current_state_;
        std::atomic<bool> initialized_;
        std::atomic<bool> running_;

        AppConfig config_;

        static constexpr const char *TAG = "SMART_SPEAKER";

    public:
        explicit SmartSpeaker(const AppConfig &config = AppConfig{});

        ~SmartSpeaker();

        // Non-copyable, non-movable (singleton-like behavior)
        SmartSpeaker(const SmartSpeaker &) = delete;

        SmartSpeaker &operator=(const SmartSpeaker &) = delete;

        SmartSpeaker(SmartSpeaker &&) = delete;

        SmartSpeaker &operator=(SmartSpeaker &&) = delete;

        // Application lifecycle
        AudioError initialize();

        AudioError start();

        void stop();

        void run();  // Main application loop

        // State management
        AppState get_state() const { return current_state_.load(std::memory_order_acquire); }

        bool is_running() const { return running_.load(std::memory_order_acquire); }

        bool is_initialized() const { return initialized_.load(std::memory_order_acquire); }

        // Configuration
        const AppConfig &get_config() const { return config_; }

        void update_config(const AppConfig &new_config) { config_ = new_config; }

        // Audio control interface
        bool set_volume(float volume);

        bool set_muted(bool muted);

        float get_volume() const;

        bool is_muted() const;

        // Module control
        bool enable_bluetooth(bool enable);

        bool enable_voice_detection(bool enable);

        bool enable_spotify(bool enable);

        // Performance monitoring
        AudioMetrics get_audio_metrics() const;

        void print_system_status() const;

    private:
        // Initialization phases
        AudioError initialize_system();

        AudioError initialize_audio_system();

        AudioError initialize_modules();

        AudioError initialize_tasks();

        // Task implementations
        void audio_output_task();

        void performance_monitor_task();

        void event_handler_task();

        // Event handlers
        void handle_audio_event(const Event &event);

        void handle_bluetooth_event(const Event &event);

        void handle_voice_event(const Event &event);

        void handle_system_event(const Event &event);

        // State transitions
        void transition_to_state(AppState new_state);

        void handle_state_change(AppState old_state, AppState new_state);

        // System management
        void setup_event_subscriptions();

        void cleanup_resources();

        // Performance monitoring
        void monitor_system_performance();

        void check_audio_health();

        void log_performance_metrics();
    };

} // namespace smart_speaker


#endif //BT_SPEAKER_SMART_SPEAKER_HPP
