#include "../include/bluetooth_audio.hpp"

namespace btspeaker {

// Static member definition - simple and clean
BluetoothAudioApp* BluetoothAudioApp::instance_ = nullptr;

BluetoothAudioApp::BluetoothAudioApp() : state_(State::UNINITIALIZED) {
    // Simple assignment for single-threaded use
    instance_ = this;
    ESP_LOGI(TAG, "BluetoothAudioApp created");
}

BluetoothAudioApp::~BluetoothAudioApp() { 
    shutdown(); 
    // Clear static instance if it points to this object
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

esp_err_t BluetoothAudioApp::init() {
    if (state_.load() != State::UNINITIALIZED) {
        ESP_LOGW(TAG, "Already initialized, state: %d", static_cast<int>(state_.load()));
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Initializing Bluetooth Audio App components");
    
    // Initialize Bluetooth components
    initializer_ = std::make_unique<BluetoothInitializer>();
    esp_err_t ret = initializer_->init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        state_.store(State::ERROR);
        return ret;
    }
    
    esp_log_level_set(TAG, ESP_LOG_INFO);
    
    // Initialize AVRCP handler
    avrcp_handler_ = std::make_unique<AvrcpHandler>();
    ret = initializer_->init_avrcp(AvrcpHandler::avrcp_tg_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize AVRCP: %s", esp_err_to_name(ret));
        state_.store(State::ERROR);
        return ret;
    }
    
    ret = avrcp_handler_->configure_target();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure AVRCP target: %s", esp_err_to_name(ret));
        state_.store(State::ERROR);
        return ret;
    }
    
    // Initialize audio pipeline manager
    pipeline_manager_ = std::make_unique<AudioPipelineManager>();
    
    // Connect AVRCP handler to pipeline manager
    avrcp_handler_->set_pipeline_manager(pipeline_manager_.get());
    
    state_.store(State::INITIALIZED);
    ESP_LOGI(TAG, "BluetoothAudioApp initialized successfully");
    return ESP_OK;
}

esp_err_t BluetoothAudioApp::run() {
    if (state_.load() != State::INITIALIZED) {
        ESP_LOGE(TAG, "Cannot run, invalid state: %d", static_cast<int>(state_.load()));
        return ESP_ERR_INVALID_STATE;
    }
    state_.store(State::RUNNING);
    
    ESP_LOGI(TAG, "Starting Bluetooth Audio App");
    
    // Setup and start audio pipeline
    esp_err_t ret = pipeline_manager_->setup_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup audio pipeline: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = pipeline_manager_->start_pipeline();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio pipeline: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Bluetooth Audio App running, entering event loop");
    
    // Run the main event loop
    return pipeline_manager_->run_event_loop();
}

void BluetoothAudioApp::shutdown() {
    if (state_.load() == State::UNINITIALIZED || state_.load() == State::STOPPED) {
        return; // Already shut down
    }
    
    ESP_LOGI(TAG, "Shutting down BluetoothAudioApp");
    
    // Clean up components in reverse order of initialization
    if (pipeline_manager_) {
        pipeline_manager_->cleanup();
        pipeline_manager_.reset();
    }
    
    avrcp_handler_.reset(); // Will clean up AVRCP static instance
    initializer_.reset();   // Will deinitialize AVRCP
    
    state_.store(State::STOPPED);
    ESP_LOGI(TAG, "BluetoothAudioApp shutdown complete");
}

BluetoothAudioApp::State BluetoothAudioApp::get_state() const {
    // Simple atomic read - no mutex needed
    return state_.load();
}

} // namespace btspeaker 