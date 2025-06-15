#pragma once

namespace btspeaker {

// Spotify Connect configuration
constexpr int SPOTIFY_CONNECT_PORT = 7864;
constexpr const char* SPOTIFY_DEVICE_NAME = "Rupert's Revenge";
constexpr const char* SPOTIFY_SERVICE_TYPE = "_spotify-connect";
constexpr const char* SPOTIFY_PROTOCOL = "_tcp";

// HTTP server configuration
constexpr int HTTP_MAX_URI_HANDLERS = 8;
constexpr int HTTP_MAX_RESP_HEADERS = 8;
constexpr int HTTP_STACK_SIZE = 8192;

} // namespace btspeaker 