#pragma once

#include <memory>
#include <string>
#include <functional>

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "cJSON.h"
}

namespace btspeaker {

class GeminiApiClient {
public:
    // Callback for receiving audio response
    using AudioResponseCallback = std::function<void(const uint8_t* audio_data, size_t length, bool is_final)>;
    
    struct Config {
        std::string api_key;
        std::string model = "gemini-2.0-flash-exp";  // Latest audio-capable model
        std::string endpoint = "https://generativelanguage.googleapis.com/v1beta/models/";
        int timeout_ms = 30000;
        size_t max_response_size = 512 * 1024; // 512KB max response
    };

    explicit GeminiApiClient(const Config& config);
    ~GeminiApiClient();
    
    // Delete copy operations
    GeminiApiClient(const GeminiApiClient&) = delete;
    GeminiApiClient& operator=(const GeminiApiClient&) = delete;
    
    esp_err_t init();
    void shutdown();
    
    // Send audio to Gemini and receive audio response
    esp_err_t send_audio_request(
        const uint8_t* audio_data, 
        size_t audio_length,
        AudioResponseCallback callback
    );
    
    // Cancel ongoing request
    void cancel_request();
    
    bool is_ready() const { return initialized_; }

private:
    static constexpr const char* TAG = "GEMINI_API";
    
    // HTTP response handler
    static esp_err_t http_event_handler(esp_http_client_event_t *evt);
    
    // Audio encoding/decoding
    std::string encode_audio_to_base64(const uint8_t* audio_data, size_t length);
    esp_err_t decode_audio_response(const std::string& base64_audio, std::vector<uint8_t>& output);
    
    // JSON request building
    std::string build_audio_request_json(const std::string& base64_audio);
    esp_err_t parse_audio_response(const std::string& response_json, AudioResponseCallback callback);
    
    // Member variables
    Config config_;
    esp_http_client_handle_t http_client_;
    std::string response_buffer_;
    AudioResponseCallback current_callback_;
    bool initialized_;
    bool request_cancelled_;
    
    // Audio format constants
    static constexpr int INPUT_SAMPLE_RATE = 16000;
    static constexpr int INPUT_CHANNELS = 1;
    static constexpr int INPUT_BITS = 16;
    static constexpr const char* INPUT_ENCODING = "linear16";
};

} // namespace btspeaker 