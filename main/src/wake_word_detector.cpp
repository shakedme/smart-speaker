// wake_word_detector.cpp

#include "../include/wake_word_detector.hpp"
#include "../include/rufus_model_data.hpp"


extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <cstring>
}


namespace btspeaker {

//////////////////////////////////////////////////////////////
//  WakeWordDetector implementation
//////////////////////////////////////////////////////////////

    WakeWordDetector::WakeWordDetector()
            : interpreter_(nullptr),
              input_tensor_(nullptr),
              output_tensor_(nullptr),
              detection_threshold_(0.30f),
              detection_hits_required_(1),
              detection_hit_count_(0) {

        micro_op_resolver.AddConv2D();          // Conv1D → Conv2D
        micro_op_resolver.AddMaxPool2D();       // MaxPooling1D → MaxPool2D
        micro_op_resolver.AddReshape();         // Flatten → Reshape
        micro_op_resolver.AddFullyConnected();  // Dense
        micro_op_resolver.AddLogistic();        // Sigmoid

        // 3) Build the TFLM interpreter
        const tflite::Model *model = tflite::GetModel(rufus_model_quant_tflite);
        interpreter_ = new tflite::MicroInterpreter(
                model,
                micro_op_resolver,
                tensor_arena,
                kTensorArenaSize,
                &micro_error_reporter
        );
        interpreter_->AllocateTensors();
        input_tensor_ = interpreter_->input(0);
        output_tensor_ = interpreter_->output(0);
    }

    WakeWordDetector::~WakeWordDetector() {
        stopProcessingTask();
        delete interpreter_;
    }

    esp_err_t WakeWordDetector::startProcessingTask() {
        if (audio_queue) {
            return ESP_ERR_INVALID_STATE;
        }
        // Create a queue to receive hop-sized PCM chunks
        audio_queue = xQueueCreate(10, sizeof(int16_t) * kHopSize);
        if (!audio_queue) {
            return ESP_ERR_NO_MEM;
        }

        // Launch the processing task
        BaseType_t ok = xTaskCreate(
                processingTaskEntry,
                "wwd_task",
                8192,
                this,
                5,
                &processing_task_handle_
        );
        return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
    }

    esp_err_t WakeWordDetector::feedAudioData(const uint8_t *stereo_data, size_t data_length) {
        if (data_length == kHopSize) {
            xQueueSend(audio_queue, data_length, 0);
        }
    }

    void WakeWordDetector::setEnabled(bool enabled) {
        enabled_ = enabled;
    }

    void WakeWordDetector::stopProcessingTask() {
        if (processingTaskHandle) {
            vTaskDelete(processingTaskHandle);
            processingTaskHandle = nullptr;
        }
        if (audio_queue) {
            vQueueDelete(audio_queue);
            audio_queue = nullptr;
        }
    }

    void WakeWordDetector::setWakeWordCallback(WakeWordCallback callback) {
        wakeWordCallback = std::move(callback);
    }

    void WakeWordDetector::processingTaskEntry(void *arg) {
        auto *self = static_cast<WakeWordDetector *>(arg);

        // Ring buffer for one-second window
        static int16_t ring_buffer[K_WINDOW_SIZE];
        int write_idx = 0;

        // Temp buffers
        int16_t hop_buffer[K_HOP_SIZE];
        int16_t window_buffer[K_WINDOW_SIZE];

        while (self->enabled_) {
            // Wait for next hop of PCM data
            if (xQueueReceive(audio_queue, hop_buffer, portMAX_DELAY) == pdTRUE) {
                // 1) Update ring buffer with new hop
                for (short i: hop_buffer) {
                    ring_buffer[write_idx] = i;
                    write_idx = (write_idx + 1) % K_WINDOW_SIZE;
                }

                // 2) Assemble a contiguous 1s window
                int idx = write_idx;
                for (int i = 0; i < K_WINDOW_SIZE; i++) {
                    window_buffer[i] = ring_buffer[(idx + i) % K_WINDOW_SIZE];
                }

                // 3) Run detection on this window
                processWindow(window_buffer);
            }
        }
    }

//
// processWindow: runs MFCC → inference → FSM → callback
//
    void WakeWordDetector::processWindow(const int16_t *audio_window) {
        // Convert PCM to float in [-1,1]
        static float float_buffer[kWindowSize];
        for (int i = 0; i < kWindowSize; ++i) {
            float_buffer[i] = audio_window[i] / 32768.0f;
        }

        // Feed to microfrontend
        frontend_microphone_read(frontend_state, float_buffer, kWindowSize);

        // Extract MFCC features
        static float mfcc_output[kFeatureSliceSize];
        frontend_features(frontend_state, mfcc_output);

        // Copy features into TFLM input tensor
        for (int i = 0; i < kFeatureSliceSize; ++i) {
            input_tensor_->data.f[i] = mfcc_output[i];
        }

        // Run inference
        interpreter_->Invoke();
        float prob = output_tensor_->data.f[0];

        // Simple threshold + hit-counter FSM
        if (prob >= detection_threshold_) {
            detection_hit_count_++;
            if (detection_hit_count_ >= detection_hits_required_) {
                wakeWordCallback();
                detection_hit_count_ = 0;
            }
        } else {
            detection_hit_count_ = 0;
        }
    }

}  // namespace btspeaker
