//
// Created by Shaked Melman on 22/06/2025.
//

#include "../include/micro_features_generator.hpp"

#include <esp_log.h>
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "include/rufus_model_data.hpp"

namespace btspeaker {


    bool g_is_first_time = true;

    const tflite::Model *model = nullptr;
    tflite::MicroInterpreter *interpreter = nullptr;

    constexpr size_t kArenaSize = 16 * 1024;
    alignas(16) uint8_t g_arena[kArenaSize];

    constexpr int kAudioSampleDurationCount =
            K_FEATURE_DURATION_MS * K_SAMPLE_RATE / 1000;
    constexpr int kAudioSampleStrideCount =
            K_FEATURE_STRIDE_MS * K_SAMPLE_RATE / 1000;
    using AudioPreprocessorOpResolver = tflite::MicroMutableOpResolver<18>;

    TfLiteStatus RegisterOps(AudioPreprocessorOpResolver &op_resolver) {
        TF_LITE_ENSURE_STATUS(op_resolver.AddReshape());
        TF_LITE_ENSURE_STATUS(op_resolver.AddCast());
        TF_LITE_ENSURE_STATUS(op_resolver.AddStridedSlice());
        TF_LITE_ENSURE_STATUS(op_resolver.AddConcatenation());
        TF_LITE_ENSURE_STATUS(op_resolver.AddMul());
        TF_LITE_ENSURE_STATUS(op_resolver.AddAdd());
        TF_LITE_ENSURE_STATUS(op_resolver.AddDiv());
        TF_LITE_ENSURE_STATUS(op_resolver.AddMinimum());
        TF_LITE_ENSURE_STATUS(op_resolver.AddMaximum());
        TF_LITE_ENSURE_STATUS(op_resolver.AddWindow());
        TF_LITE_ENSURE_STATUS(op_resolver.AddFftAutoScale());
        TF_LITE_ENSURE_STATUS(op_resolver.AddRfft());
        TF_LITE_ENSURE_STATUS(op_resolver.AddEnergy());
        TF_LITE_ENSURE_STATUS(op_resolver.AddFilterBank());
        TF_LITE_ENSURE_STATUS(op_resolver.AddFilterBankSquareRoot());
        TF_LITE_ENSURE_STATUS(op_resolver.AddFilterBankSpectralSubtraction());
        TF_LITE_ENSURE_STATUS(op_resolver.AddPCAN());
        TF_LITE_ENSURE_STATUS(op_resolver.AddFilterBankLog());
        return kTfLiteOk;
    }

    TfLiteStatus InitializeMicroFeatures() {
        g_is_first_time = true;

        model = tflite::GetModel(rufus_model_quant_tflite);
        if (model->version() != TFLITE_SCHEMA_VERSION) {
            MicroPrintf("Model provided for Feature generator is schema version %d "
                        "not equal to supported version %d.", model->version(), TFLITE_SCHEMA_VERSION);
            return kTfLiteError;
        }

        static AudioPreprocessorOpResolver op_resolver;
        RegisterOps(op_resolver);

        static tflite::MicroInterpreter static_interpreter(model, op_resolver, g_arena, kArenaSize);
        interpreter = &static_interpreter;

        if (interpreter->AllocateTensors() != kTfLiteOk) {
            MicroPrintf("AllocateTensors failed for Feature provider model. Line %d", __LINE__);
            return kTfLiteError;
        }

        // MicroPrintf("AudioPreprocessor model arena size = %u",
        //             interpreter.arena_used_bytes());

        return kTfLiteOk;
    }

    TfLiteStatus GenerateSingleFeature(const int16_t *audio_data,
                                       const int audio_data_size,
                                       int8_t *feature_output,
                                       tflite::MicroInterpreter *interpreter) {
        TfLiteTensor *input = interpreter->input(0);
        TfLiteTensor *output = interpreter->output(0);
        std::copy_n(audio_data, audio_data_size,
                    tflite::GetTensorData<int16_t>(input));
        if (interpreter->Invoke() != kTfLiteOk) {
            MicroPrintf("Feature generator model invocation failed");
        }

        std::copy_n(tflite::GetTensorData<int8_t>(output), K_FEATURE_SLIZE_SICE,
                    feature_output);

        return kTfLiteOk;
    }

    TfLiteStatus GenerateFeatures(const int16_t *audio_data,
                                  const size_t audio_data_size,
                                  Features *features_output) {
        size_t remaining_samples = audio_data_size;
        size_t feature_index = 0;
        while (remaining_samples >= kAudioSampleDurationCount &&
               feature_index < K_MFCC_FEATURE_COUNT) {
            TF_LITE_ENSURE_STATUS(
                    GenerateSingleFeature(audio_data, kAudioSampleDurationCount,
                                          (*features_output)[feature_index], interpreter));
            feature_index++;
            audio_data += kAudioSampleStrideCount;
            remaining_samples -= kAudioSampleStrideCount;
        }

        return kTfLiteOk;
    }
}