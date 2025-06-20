#pragma once

namespace btspeaker {

// Spotify Connect configuration
constexpr int SPOTIFY_CONNECT_PORT = 7864;
constexpr const char* SPOTIFY_DEVICE_NAME = "ESP32_SPEAKER";
constexpr const char* SPOTIFY_SERVICE_TYPE = "_spotify-connect";
constexpr const char* SPOTIFY_PROTOCOL = "_tcp";

constexpr const int SPOTIFY_SAMPLE_RATE = 44100;
constexpr const int SPOTIFY_CHANNELS = 2;
constexpr const int SPOTIFY_BITS_PER_SAMPLE = 16;

} // namespace btspeaker 