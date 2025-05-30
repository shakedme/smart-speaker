//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_BT_SINK_H
#define BT_SPEAKER_BT_SINK_H

#pragma once

#include "../audio/audio_types.hpp"
#include "../audio/audio_mixer.hpp"
#include "../shared/event_bus.hpp"
#include <memory>
#include <atomic>
#include <string>

extern "C" {
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "esp_system.h"
}

namespace smart_speaker {

// Bluetooth connection states
    enum class BluetoothState : uint8_t {
        NotInitialized,
        Initialized,
        Discoverable,
        Connecting,
        Connected,
        AudioStreaming,
        Error
    };

// Bluetooth device information
    struct BluetoothDeviceInfo {
        uint8_t bda[ESP_BD_ADDR_LEN] = {0};  // Bluetooth device address
        char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
        bool connected = false;
        bool authenticated = false;
        esp_a2d_audio_state_t audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
    };

// A2DP audio statistics
    struct A2DPStats {
        uint32_t packets_received = 0;
        uint32_t audio_frames_decoded = 0;
        uint32_t audio_bytes_received = 0;
        uint32_t codec_errors = 0;
        uint32_t buffer_overruns = 0;
        esp_a2d_mct_t current_codec = ESP_A2D_MCT_SBC;
    };

// ESP32 Bluetooth A2DP Audio Sink
    class BluetoothSink {
    private:
        StandardAudioMixer* audio_mixer_;
        AudioFrameBuffer* audio_buffer_;
        
        std::atomic<BluetoothState> state_;
        std::atomic<bool> initialized_;
        std::atomic<bool> running_;
        
        BluetoothDeviceInfo connected_device_;
        A2DPStats stats_;
        
        // Configuration
        std::string device_name_;
        bool auto_reconnect_;
        
        static constexpr const char* TAG = "BT_SINK";
        
        // Static callbacks (required by ESP32 Bluetooth API)
        static BluetoothSink* instance_;  // For callback access
        
    public:
        explicit BluetoothSink(StandardAudioMixer* mixer, 
                             const std::string& device_name = "ESP32-Speaker");
        ~BluetoothSink();
        
        // Non-copyable, non-movable
        BluetoothSink(const BluetoothSink&) = delete;
        BluetoothSink& operator=(const BluetoothSink&) = delete;
        BluetoothSink(BluetoothSink&&) = delete;
        BluetoothSink& operator=(BluetoothSink&&) = delete;
        
        // Lifecycle management
        bool initialize();
        bool start();
        void stop();
        void deinitialize();
        
        // Connection management
        bool make_discoverable(bool discoverable = true);
        bool disconnect_current_device();
        
        // Status and information
        BluetoothState get_state() const { return state_.load(std::memory_order_acquire); }
        bool is_connected() const;
        bool is_audio_streaming() const;
        BluetoothDeviceInfo get_connected_device() const { return connected_device_; }
        A2DPStats get_statistics() const { return stats_; }
        
        // Configuration
        void set_device_name(const std::string& name);
        void set_auto_reconnect(bool enable) { auto_reconnect_ = enable; }
        bool get_auto_reconnect() const { return auto_reconnect_; }
        
        // Audio control (AVRCP)
        bool send_play_command();
        bool send_pause_command();
        bool send_stop_command();
        bool send_next_track();
        bool send_previous_track();
        bool send_volume_up();
        bool send_volume_down();
        
        // Diagnostics
        void print_status() const;
        void reset_statistics();
        
    private:
        // Initialization helpers
        bool init_bluetooth_controller();
        bool init_bluetooth_stack();
        bool init_a2dp_sink();
        bool init_avrcp_target();
        
        // State management
        void set_state(BluetoothState new_state);
        void handle_connection_established(const uint8_t* bda);
        void handle_connection_lost();
        void handle_audio_state_change(esp_a2d_audio_state_t state);
        
        // Audio processing
        void process_audio_data(const uint8_t* data, uint32_t len);
        void convert_and_buffer_audio(const int16_t* samples, size_t sample_count);
        
        // Utility functions
        const char* state_to_string(BluetoothState state) const;
        const char* audio_state_to_string(esp_a2d_audio_state_t state) const;
        std::string bda_to_string(const uint8_t* bda) const;
        
        // Static ESP32 callback handlers
        static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
        static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
        static void avrcp_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t* param);
        static void audio_data_callback(const uint8_t* data, uint32_t len);
    };

} // namespace smart_speaker

#endif //BT_SPEAKER_BT_SINK_H
