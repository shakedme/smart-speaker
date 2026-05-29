#pragma once

#include <cstdint>

namespace speaker_protocol {

inline constexpr uint32_t kProtocolVersion = 2;

inline constexpr const char *kMessageHello = "hello";
inline constexpr const char *kMessagePlaybackState = "playback.state";
inline constexpr const char *kMessageUiNowPlaying = "ui.now_playing";
inline constexpr const char *kMessageUiWaveform = "ui.waveform";
inline constexpr const char *kMessageStateChange = "state_change";
inline constexpr const char *kMessageIntent = "intent";
inline constexpr const char *kMessageError = "error";

inline constexpr const char *kRolePlaybackNode = "playback_node";
inline constexpr const char *kRoleVoiceUiNode = "voice_ui_node";

inline constexpr const char *kBoardEsp32WroverPlayback = "esp32_wrover_playback";
inline constexpr const char *kBoardEsp32S3Korvo2Lcd = "esp32_s3_korvo2_lcd";
inline constexpr const char *kBoardEsp32S3N32R16Sph0645Lcd = "esp32_s3_n32r16_sph0645_lcd";

inline constexpr const char *kCapabilitySpotify = "spotify";
inline constexpr const char *kCapabilityBluetoothA2dp = "bluetooth_a2dp";
inline constexpr const char *kCapabilitySpeakerOutput = "speaker_output";
inline constexpr const char *kCapabilityLlmAudioDownlink = "llm_audio_downlink";
inline constexpr const char *kCapabilityPlaybackRef = "playback_ref";
inline constexpr const char *kCapabilityPlaybackState = "playback_state";
inline constexpr const char *kCapabilityWaveformSource = "waveform_source";
inline constexpr const char *kCapabilityMicUplink = "mic_uplink";
inline constexpr const char *kCapabilityDisplay = "display";
inline constexpr const char *kCapabilityNowPlayingUi = "now_playing_ui";
inline constexpr const char *kCapabilityWaveformUi = "waveform_ui";

}  // namespace speaker_protocol
