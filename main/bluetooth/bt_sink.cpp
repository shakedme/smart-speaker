//
// Created by Shaked Melman on 29/05/2025.
//

#include "bt_sink.hpp"

namespace smart_speaker {

// Static instance for callbacks
BluetoothSink* BluetoothSink::instance_ = nullptr;

// Constructor
BluetoothSink::BluetoothSink(StandardAudioMixer* mixer, const std::string& device_name)
    : audio_mixer_(mixer),
      audio_buffer_(nullptr),
      state_(BluetoothState::NotInitialized),
      initialized_(false),
      running_(false),
      device_name_(device_name),
      auto_reconnect_(true) {
    
    if (instance_ != nullptr) {
        ESP_LOGE(TAG, "BluetoothSink instance already exists!");
        return;
    }
    
    instance_ = this;
    memset(&connected_device_, 0, sizeof(connected_device_));
    memset(&stats_, 0, sizeof(stats_));
    
    ESP_LOGI(TAG, "BluetoothSink created with device name: %s", device_name_.c_str());
}

// Destructor
BluetoothSink::~BluetoothSink() {
    deinitialize();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

// Initialize Bluetooth system
bool BluetoothSink::initialize() {
    ESP_LOGI(TAG, "Initializing Bluetooth A2DP sink...");
    
    if (initialized_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Bluetooth already initialized");
        return true;
    }
    
    // Get audio buffer from mixer
    if (audio_mixer_) {
        audio_buffer_ = audio_mixer_->get_source_buffer(AudioSource::Bluetooth);
        if (!audio_buffer_) {
            ESP_LOGE(TAG, "Failed to get Bluetooth audio buffer from mixer");
            return false;
        }
    } else {
        ESP_LOGE(TAG, "Audio mixer is null");
        return false;
    }
    
    // Initialize Bluetooth controller
    if (!init_bluetooth_controller()) {
        ESP_LOGE(TAG, "Failed to initialize Bluetooth controller");
        return false;
    }
    
    // Initialize Bluetooth stack
    if (!init_bluetooth_stack()) {
        ESP_LOGE(TAG, "Failed to initialize Bluetooth stack");
        return false;
    }
    
    // Initialize A2DP sink
    if (!init_a2dp_sink()) {
        ESP_LOGE(TAG, "Failed to initialize A2DP sink");
        return false;
    }
    
    // Initialize AVRCP target
    if (!init_avrcp_target()) {
        ESP_LOGE(TAG, "Failed to initialize AVRCP target");
        return false;
    }
    
    initialized_.store(true, std::memory_order_release);
    set_state(BluetoothState::Initialized);
    
    ESP_LOGI(TAG, "Bluetooth A2DP sink initialized successfully");
    return true;
}

// Start Bluetooth service
bool BluetoothSink::start() {
    ESP_LOGI(TAG, "Starting Bluetooth A2DP sink...");
    
    if (!initialized_.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "Bluetooth not initialized");
        return false;
    }
    
    if (running_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Bluetooth already running");
        return true;
    }
    
    // Set device name
    esp_err_t ret = esp_bt_gap_set_device_name(device_name_.c_str());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set device name: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Make device discoverable
    if (!make_discoverable(true)) {
        ESP_LOGE(TAG, "Failed to make device discoverable");
        return false;
    }
    
    running_.store(true, std::memory_order_release);
    set_state(BluetoothState::Discoverable);
    
    ESP_LOGI(TAG, "Bluetooth A2DP sink started - device discoverable as '%s'", device_name_.c_str());
    
    // Publish Bluetooth started event
    PUBLISH_SIMPLE_EVENT(EventType::BluetoothConnected);
    
    return true;
}

// Stop Bluetooth service
void BluetoothSink::stop() {
    ESP_LOGI(TAG, "Stopping Bluetooth A2DP sink...");
    
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    
    // Disconnect current device if connected
    disconnect_current_device();
    
    // Make device non-discoverable
    make_discoverable(false);
    
    running_.store(false, std::memory_order_release);
    set_state(BluetoothState::Initialized);
    
    ESP_LOGI(TAG, "Bluetooth A2DP sink stopped");
    
    // Publish Bluetooth stopped event
    PUBLISH_SIMPLE_EVENT(EventType::BluetoothDisconnected);
}

// Deinitialize Bluetooth system
void BluetoothSink::deinitialize() {
    ESP_LOGI(TAG, "Deinitializing Bluetooth...");
    
    stop();
    
    if (!initialized_.load(std::memory_order_acquire)) {
        return;
    }
    
    // Cleanup A2DP
    esp_a2d_sink_deinit();
    
    // Cleanup AVRCP
    esp_avrc_ct_deinit();
    
    // Disable Bluetooth
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    
    initialized_.store(false, std::memory_order_release);
    set_state(BluetoothState::NotInitialized);
    
    ESP_LOGI(TAG, "Bluetooth deinitialized");
}

// Make device discoverable/non-discoverable
bool BluetoothSink::make_discoverable(bool discoverable) {
    esp_bt_gap_set_scan_mode(discoverable ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                             discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
    
    ESP_LOGI(TAG, "Device is now %s", discoverable ? "discoverable" : "non-discoverable");
    return true;
}

// Disconnect current device
bool BluetoothSink::disconnect_current_device() {
    if (!is_connected()) {
        return true;
    }
    
    ESP_LOGI(TAG, "Disconnecting device: %s", bda_to_string(connected_device_.bda).c_str());
    
    esp_err_t ret = esp_a2d_sink_disconnect(connected_device_.bda);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disconnect A2DP: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

// Status queries
bool BluetoothSink::is_connected() const {
    BluetoothState current_state = get_state();
    return current_state == BluetoothState::Connected || 
           current_state == BluetoothState::AudioStreaming;
}

bool BluetoothSink::is_audio_streaming() const {
    return get_state() == BluetoothState::AudioStreaming;
}

// Set device name
void BluetoothSink::set_device_name(const std::string& name) {
    device_name_ = name;
    if (initialized_.load(std::memory_order_acquire)) {
        esp_bt_gap_set_device_name(device_name_.c_str());
    }
}

// AVRCP control commands
bool BluetoothSink::send_play_command() {
    if (!is_connected()) return false;
    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
    return ret == ESP_OK;
}

bool BluetoothSink::send_pause_command() {
    if (!is_connected()) return false;
    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_PRESSED);
    return ret == ESP_OK;
}

bool BluetoothSink::send_stop_command() {
    if (!is_connected()) return false;
    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_STOP, ESP_AVRC_PT_CMD_STATE_PRESSED);
    return ret == ESP_OK;
}

bool BluetoothSink::send_next_track() {
    if (!is_connected()) return false;
    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
    return ret == ESP_OK;
}

bool BluetoothSink::send_previous_track() {
    if (!is_connected()) return false;
    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
    return ret == ESP_OK;
}

bool BluetoothSink::send_volume_up() {
    if (!is_connected()) return false;
    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_VOL_UP, ESP_AVRC_PT_CMD_STATE_PRESSED);
    return ret == ESP_OK;
}

bool BluetoothSink::send_volume_down() {
    if (!is_connected()) return false;
    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_VOL_DOWN, ESP_AVRC_PT_CMD_STATE_PRESSED);
    return ret == ESP_OK;
}

// Print status information
void BluetoothSink::print_status() const {
    ESP_LOGI(TAG, "=== Bluetooth A2DP Status ===");
    ESP_LOGI(TAG, "State: %s", state_to_string(get_state()));
    ESP_LOGI(TAG, "Device name: %s", device_name_.c_str());
    ESP_LOGI(TAG, "Running: %s", running_.load() ? "Yes" : "No");
    
    if (is_connected()) {
        ESP_LOGI(TAG, "Connected device: %s", connected_device_.name);
        ESP_LOGI(TAG, "Device address: %s", bda_to_string(connected_device_.bda).c_str());
        ESP_LOGI(TAG, "Audio state: %s", audio_state_to_string(connected_device_.audio_state));
    }
    
    ESP_LOGI(TAG, "Statistics:");
    ESP_LOGI(TAG, "  Packets received: %lu", stats_.packets_received);
    ESP_LOGI(TAG, "  Audio frames: %lu", stats_.audio_frames_decoded);
    ESP_LOGI(TAG, "  Bytes received: %lu", stats_.audio_bytes_received);
    ESP_LOGI(TAG, "  Codec errors: %lu", stats_.codec_errors);
    ESP_LOGI(TAG, "  Buffer overruns: %lu", stats_.buffer_overruns);
}

// Reset statistics
void BluetoothSink::reset_statistics() {
    memset(&stats_, 0, sizeof(stats_));
    ESP_LOGI(TAG, "Statistics reset");
}

// Private implementation methods...
bool BluetoothSink::init_bluetooth_controller() {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

bool BluetoothSink::init_bluetooth_stack() {
    esp_err_t ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Register GAP callback
    ret = esp_bt_gap_register_callback(gap_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

bool BluetoothSink::init_a2dp_sink() {
    esp_err_t ret = esp_a2d_register_callback(a2dp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_a2d_sink_register_data_callback(audio_data_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP data callback register failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_a2d_sink_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink init failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

bool BluetoothSink::init_avrcp_target() {
    esp_err_t ret = esp_avrc_ct_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRCP controller init failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_avrc_ct_register_callback(avrcp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRCP callback register failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

// State management
void BluetoothSink::set_state(BluetoothState new_state) {
    BluetoothState old_state = state_.exchange(new_state, std::memory_order_acq_rel);
    if (old_state != new_state) {
        ESP_LOGI(TAG, "State changed: %s -> %s", 
                 state_to_string(old_state), state_to_string(new_state));
    }
}

void BluetoothSink::handle_connection_established(const uint8_t* bda) {
    memcpy(connected_device_.bda, bda, ESP_BD_ADDR_LEN);
    connected_device_.connected = true;
    set_state(BluetoothState::Connected);
    
    ESP_LOGI(TAG, "Device connected: %s", bda_to_string(bda).c_str());
    PUBLISH_SIMPLE_EVENT(EventType::BluetoothConnected);
}

void BluetoothSink::handle_connection_lost() {
    connected_device_.connected = false;
    connected_device_.audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
    set_state(BluetoothState::Discoverable);
    
    ESP_LOGI(TAG, "Device disconnected");
    PUBLISH_SIMPLE_EVENT(EventType::BluetoothDisconnected);
}

void BluetoothSink::handle_audio_state_change(esp_a2d_audio_state_t state) {
    connected_device_.audio_state = state;
    
    if (state == ESP_A2D_AUDIO_STATE_STARTED) {
        set_state(BluetoothState::AudioStreaming);
        PUBLISH_SIMPLE_EVENT(EventType::BluetoothAudioStarted);
    } else {
        set_state(BluetoothState::Connected);
        PUBLISH_SIMPLE_EVENT(EventType::BluetoothAudioStopped);
    }
}

// Audio processing
void BluetoothSink::process_audio_data(const uint8_t* data, uint32_t len) {
    if (!audio_buffer_ || !running_.load(std::memory_order_acquire)) {
        return;
    }
    
    stats_.packets_received++;
    stats_.audio_bytes_received += len;
    
    // Convert raw audio data to 16-bit samples
    // Note: This is a simplified conversion - real implementation would
    // need proper SBC/AAC decoding
    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
    size_t sample_count = len / sizeof(int16_t);
    
    convert_and_buffer_audio(samples, sample_count);
}

void BluetoothSink::convert_and_buffer_audio(const int16_t* samples, size_t sample_count) {
    // Convert samples to audio frames and buffer them
    const size_t frame_size = AudioConfig::FRAME_SIZE;
    
    for (size_t i = 0; i < sample_count; i += frame_size) {
        StandardFrame* frame = audio_buffer_->reserve_write(1);
        if (!frame) {
            stats_.buffer_overruns++;
            continue;
        }
        
        size_t samples_to_copy = std::min(frame_size, sample_count - i);
        std::copy(samples + i, samples + i + samples_to_copy, frame->samples.begin());
        
        // Fill remaining samples with silence if needed
        if (samples_to_copy < frame_size) {
            std::fill(frame->samples.begin() + samples_to_copy, frame->samples.end(), 0);
        }
        
        // Set frame metadata
        frame->timestamp_us = esp_timer_get_time();
        frame->source = AudioSource::Bluetooth;
        frame->channels = AudioConfig::CHANNELS;
        frame->sample_rate = AudioConfig::SAMPLE_RATE;
        
        audio_buffer_->commit_write(1);
        stats_.audio_frames_decoded++;
    }
}

// Utility functions
const char* BluetoothSink::state_to_string(BluetoothState state) const {
    switch (state) {
        case BluetoothState::NotInitialized: return "Not Initialized";
        case BluetoothState::Initialized: return "Initialized";
        case BluetoothState::Discoverable: return "Discoverable";
        case BluetoothState::Connecting: return "Connecting";
        case BluetoothState::Connected: return "Connected";
        case BluetoothState::AudioStreaming: return "Audio Streaming";
        case BluetoothState::Error: return "Error";
        default: return "Unknown";
    }
}

const char* BluetoothSink::audio_state_to_string(esp_a2d_audio_state_t state) const {
    switch (state) {
        case ESP_A2D_AUDIO_STATE_STOPPED: return "Stopped";
        case ESP_A2D_AUDIO_STATE_STARTED: return "Started";
        default: return "Unknown";
    }
}

std::string BluetoothSink::bda_to_string(const uint8_t* bda) const {
    char bda_str[18];
    snprintf(bda_str, sizeof(bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return std::string(bda_str);
}

// Static callback implementations
void BluetoothSink::gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    if (!instance_) return;
    
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(instance_->TAG, "Authentication success: %s", 
                         instance_->bda_to_string(param->auth_cmpl.bda).c_str());
                instance_->connected_device_.authenticated = true;
            } else {
                ESP_LOGE(instance_->TAG, "Authentication failed, status: %d", param->auth_cmpl.stat);
            }
            break;
        default:
            break;
    }
}

void BluetoothSink::a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    if (!instance_) return;
    
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                instance_->handle_connection_established(param->conn_stat.remote_bda);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                instance_->handle_connection_lost();
            }
            break;
            
        case ESP_A2D_AUDIO_STATE_EVT:
            instance_->handle_audio_state_change(param->audio_stat.state);
            break;
            
        case ESP_A2D_AUDIO_CFG_EVT:
            ESP_LOGI(instance_->TAG, "Audio codec configured: %d", param->audio_cfg.mcc.type);
            instance_->stats_.current_codec = param->audio_cfg.mcc.type;
            break;
            
        default:
            break;
    }
}

void BluetoothSink::avrcp_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t* param) {
    if (!instance_) return;
    
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            ESP_LOGI(instance_->TAG, "AVRCP connection state: %d", param->conn_stat.connected);
            break;
        default:
            break;
    }
}

void BluetoothSink::audio_data_callback(const uint8_t* data, uint32_t len) {
    if (!instance_) return;
    instance_->process_audio_data(data, len);
}

} // namespace smart_speaker
