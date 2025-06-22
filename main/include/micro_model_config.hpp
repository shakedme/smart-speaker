//
// Created by Shaked Melman on 22/06/2025.
//

#ifndef BT_SPEAKER_MICRO_MODEL_CONFIG_HPP
#define BT_SPEAKER_MICRO_MODEL_CONFIG_HPP

#endif //BT_SPEAKER_MICRO_MODEL_CONFIG_HPP


namespace btspeaker {
    static constexpr int K_MFCC_FEATURE_COUNT = 40;
    static constexpr int K_MFCC_FRAME_COUNT = 49;
    static constexpr int K_FEATURE_SLIZE_SICE = K_MFCC_FRAME_COUNT * K_MFCC_FEATURE_COUNT;  // 1960
    static constexpr int K_TENSOR_ARENA_SIZE = 100 * 1024;
    static constexpr int K_SAMPLE_RATE = 16000;
    static constexpr int K_WINDOW_SIZE_MS = 1000;
    static constexpr int K_HOP_SIZE_MS = 100;
    static constexpr int K_WINDOW_SIZE = (K_SAMPLE_RATE * K_WINDOW_SIZE_MS) / 1000;  // 16000
    static constexpr int K_HOP_SIZE = (K_SAMPLE_RATE * K_HOP_SIZE_MS) / 1000;  // 1600
    static constexpr int K_FEATURE_STRIDE_MS = 20;
    static constexpr int K_FEATURE_DURATION_MS = 30;
}