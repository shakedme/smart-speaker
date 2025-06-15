#include "../include/bluetooth_pipeline.hpp"
#include "../include/i2s_audio_service.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "audio_pipeline.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "bluetooth_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace btspeaker {

    static constexpr const char *TAG = "PIPELINE_MGR";

    esp_err_t BluetoothPipeline::setupPipeline() {
        ESP_LOGI(TAG, "Setting up audio pipeline");

        // Initialize event interface with default configuration (like official example)
        audio_event_iface_cfg_t evtCfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();

        audio_event_iface_handle_t evtHandle = audio_event_iface_init(&evtCfg);
        if (!evtHandle) {
            ESP_LOGE(TAG, "Failed to create audio event interface");
            return ESP_FAIL;
        }
        evt_.reset(evtHandle, audioEventIfaceDeleter);

        // Verify event interface is working
        ESP_LOGI(TAG, "Event interface created successfully");

        bluetooth_service_cfg_t btCfg = {
                .device_name = "ESP32_SPEAKER",
                .remote_name = nullptr,
                .mode = BLUETOOTH_A2DP_SINK,
                .user_callback = {}
        };
        esp_err_t ret = bluetooth_service_start(&btCfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Bluetooth service: %s", esp_err_to_name(ret));
            return ret;
        }
        btServiceStarted = true;
        ESP_LOGI(TAG, "Bluetooth A2DP Sink started, device name: %s", btCfg.device_name);

        // Use shared I2S service for consistent configuration
        audio_element_handle_t i2SHandle = I2SAudioService::getInstance().createI2SWriter(
                I2SAudioService::AudioSource::BLUETOOTH,
                44100, // sample_rate
                2,     // channels (stereo)
                16     // bits_per_sample
        );

        if (!i2SHandle) {
            ESP_LOGE(TAG, "Failed to create I2S stream via shared service");
            return ESP_FAIL;
        }
        i2SStreamWriter.reset(i2SHandle, audioElementDeleter);

        // Create Bluetooth stream
        audio_element_handle_t bt_handle = bluetooth_service_create_stream();
        if (!bt_handle) {
            ESP_LOGE(TAG, "Failed to create Bluetooth stream");
            return ESP_FAIL;
        }
        btStreamReader.reset(bt_handle, audioElementDeleter);

        audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
        audio_pipeline_handle_t pipeline_handle = audio_pipeline_init(&pipeline_cfg);
        if (!pipeline_handle) {
            ESP_LOGE(TAG, "Failed to create audio pipeline");
            return ESP_FAIL;
        }
        pipeline_.reset(pipeline_handle, audioPipelineDeleter);

        // Register and link elements
        ret = audio_pipeline_register(pipeline_.get(), btStreamReader.get(), "bt");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register BT stream: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = audio_pipeline_register(pipeline_.get(), i2SStreamWriter.get(), "i2s");
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register I2S stream: %s", esp_err_to_name(ret));
            return ret;
        }

        const char *link_tag[] = {"bt", "i2s"};
        ret = audio_pipeline_link(pipeline_.get(), link_tag, 2);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to link pipeline elements: %s", esp_err_to_name(ret));
            return ret;
        }

        // Set event listener on pipeline
        ret = audio_pipeline_set_listener(pipeline_.get(), evt_.get());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set pipeline listener: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Audio pipeline setup completed successfully");
        return ESP_OK;
    }

    esp_err_t BluetoothPipeline::startPipeline() {
        if (!pipeline_) {
            ESP_LOGE(TAG, "Pipeline not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = audio_pipeline_run(pipeline_.get());
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start audio pipeline: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Audio pipeline started successfully");
        return ESP_OK;
    }

    esp_err_t BluetoothPipeline::stopPipeline() {
        if (!pipeline_) {
            ESP_LOGW(TAG, "Pipeline not available for stop");
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = audio_pipeline_stop(pipeline_.get());
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop pipeline: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    esp_err_t BluetoothPipeline::pausePipeline() {
        if (!pipeline_) {
            ESP_LOGW(TAG, "Pipeline not available for pause");
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = audio_pipeline_pause(pipeline_.get());
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to pause pipeline: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    esp_err_t BluetoothPipeline::resumePipeline() {
        if (!pipeline_) {
            ESP_LOGW(TAG, "Pipeline not available for resume");
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t ret = audio_pipeline_resume(pipeline_.get());
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to resume pipeline: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    void BluetoothPipeline::cleanup() {
        ESP_LOGI(TAG, "Cleaning up audio pipeline");

        // Stop pipeline first, with proper error handling
        if (pipeline_) {
            esp_err_t ret = audio_pipeline_stop(pipeline_.get());
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Pipeline stop returned: %s", esp_err_to_name(ret));
            }

            ret = audio_pipeline_wait_for_stop(pipeline_.get());
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Pipeline wait for stop returned: %s", esp_err_to_name(ret));
            }
        }

        // Unregister elements from pipeline before cleanup to avoid double-free
        if (pipeline_ && btStreamReader) {
            audio_pipeline_unregister(pipeline_.get(), btStreamReader.get());
        }
        if (pipeline_ && i2SStreamWriter) {
            audio_pipeline_unregister(pipeline_.get(), i2SStreamWriter.get());
        }

        // Clean up in reverse order of initialization
        i2SStreamWriter.reset();
        btStreamReader.reset();
        pipeline_.reset();
        evt_.reset();

        // Stop Bluetooth service
        if (btServiceStarted) {
            bluetooth_service_destroy();
            btServiceStarted = false;
            ESP_LOGI(TAG, "Bluetooth service stopped");
        }

        ESP_LOGI(TAG, "Audio pipeline cleanup complete");
    }

} // namespace btspeaker 