#include "../include/gemini_api_client.hpp"
#include <vector>
#include <sstream>

extern "C" {
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
}

namespace btspeaker {

GeminiApiClient::GeminiApiClient(const Config& config)
    : config_(config)
    , http_client_(nullptr)
    , initialized_(false)
    , request_cancelled_(false)
{
    ESP_LOGI(TAG, "GeminiApiClient created");
}

GeminiApiClient::~GeminiApiClient() {
    shutdown();
}

esp_err_t GeminiApiClient::init() {
    ESP_LOGI(TAG, "Initializing Gemini API client");
    
    if (config_.api_key.empty()) {
        ESP_LOGE(TAG, "API key is empty");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Build full endpoint URL
    std::string full_url = config_.endpoint + config_.model + ":generateContent";
    
    esp_http_client_config_t http_config = {};
    http_config.url = full_url.c_str();
    http_config.event_handler = http_event_handler;
    http_config.timeout_ms = config_.timeout_ms;
    http_config.disable_auto_redirect = false;
    http_config.max_redirection_count = 3;
    http_config.user_data = this;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.keep_alive_enable = true;
    http_config.skip_cert_common_name_check = false;
    
    http_client_ = esp_http_client_init(&http_config);
    if (!http_client_) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    // Set headers
    esp_http_client_set_method(http_client_, HTTP_METHOD_POST);
    esp_http_client_set_header(http_client_, "Content-Type", "application/json");
    
    // Add API key to URL as query parameter (Gemini API style)
    std::string api_key_param = "?key=" + config_.api_key;
    std::string full_url_with_key = full_url + api_key_param;
    esp_http_client_set_url(http_client_, full_url_with_key.c_str());
    
    initialized_ = true;
    ESP_LOGI(TAG, "Gemini API client initialized successfully");
    return ESP_OK;
}

void GeminiApiClient::shutdown() {
    ESP_LOGI(TAG, "Shutting down Gemini API client");
    
    cancel_request();
    
    if (http_client_) {
        esp_http_client_cleanup(http_client_);
        http_client_ = nullptr;
    }
    
    initialized_ = false;
}

esp_err_t GeminiApiClient::send_audio_request(
    const uint8_t* audio_data, 
    size_t audio_length,
    AudioResponseCallback callback
) {
    if (!initialized_ || !http_client_) {
        ESP_LOGE(TAG, "Client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!audio_data || audio_length == 0) {
        ESP_LOGE(TAG, "Invalid audio data");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Sending audio request to Gemini (length: %zu bytes)", audio_length);
    
    current_callback_ = callback;
    request_cancelled_ = false;
    response_buffer_.clear();
    
    // Encode audio to base64
    std::string base64_audio = encode_audio_to_base64(audio_data, audio_length);
    if (base64_audio.empty()) {
        ESP_LOGE(TAG, "Failed to encode audio to base64");
        return ESP_FAIL;
    }
    
    // Build JSON request
    std::string json_request = build_audio_request_json(base64_audio);
    
    ESP_LOGD(TAG, "JSON request size: %zu bytes", json_request.length());
    
    // Set request data
    esp_err_t ret = esp_http_client_set_post_field(http_client_, 
                                                  json_request.c_str(), 
                                                  json_request.length());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set POST data: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Perform request
    ret = esp_http_client_perform(http_client_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Check response status
    int status_code = esp_http_client_get_status_code(http_client_);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status: %d", status_code);
        ESP_LOGE(TAG, "Response: %s", response_buffer_.c_str());
        return ESP_FAIL;
    }
    
    // Parse and process response
    if (!request_cancelled_) {
        ret = parse_audio_response(response_buffer_, callback);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to parse audio response: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    ESP_LOGI(TAG, "Audio request completed successfully");
    return ESP_OK;
}

void GeminiApiClient::cancel_request() {
    request_cancelled_ = true;
    ESP_LOGI(TAG, "Request cancelled");
}

esp_err_t GeminiApiClient::http_event_handler(esp_http_client_event_t *evt) {
    GeminiApiClient* client = static_cast<GeminiApiClient*>(evt->user_data);
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!client->request_cancelled_) {
                std::string data(static_cast<char*>(evt->data), evt->data_len);
                client->response_buffer_ += data;
                
                // Limit response size to prevent memory issues
                if (client->response_buffer_.length() > client->config_.max_response_size) {
                    ESP_LOGW(client->TAG, "Response too large, truncating");
                    client->response_buffer_.resize(client->config_.max_response_size);
                    client->request_cancelled_ = true;
                }
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(client->TAG, "HTTP request finished, response size: %zu", 
                    client->response_buffer_.length());
            break;
            
        case HTTP_EVENT_ERROR:
            ESP_LOGE(client->TAG, "HTTP error occurred");
            break;
            
        default:
            break;
    }
    
    return ESP_OK;
}

std::string GeminiApiClient::encode_audio_to_base64(const uint8_t* audio_data, size_t length) {
    if (!audio_data || length == 0) {
        return "";
    }
    
    // Calculate required buffer size for base64 encoding
    size_t encoded_len = 0;
    mbedtls_base64_encode(nullptr, 0, &encoded_len, audio_data, length);
    
    if (encoded_len == 0) {
        ESP_LOGE(TAG, "Failed to calculate base64 size");
        return "";
    }
    
    // Allocate buffer and encode
    std::vector<uint8_t> encoded_buffer(encoded_len);
    size_t actual_len = 0;
    
    int ret = mbedtls_base64_encode(encoded_buffer.data(), encoded_len, &actual_len, 
                                   audio_data, length);
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encoding failed: %d", ret);
        return "";
    }
    
    return std::string(reinterpret_cast<char*>(encoded_buffer.data()), actual_len);
}

esp_err_t GeminiApiClient::decode_audio_response(const std::string& base64_audio, std::vector<uint8_t>& output) {
    if (base64_audio.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Calculate required buffer size
    size_t decoded_len = 0;
    mbedtls_base64_decode(nullptr, 0, &decoded_len, 
                         reinterpret_cast<const uint8_t*>(base64_audio.c_str()), 
                         base64_audio.length());
    
    if (decoded_len == 0) {
        ESP_LOGE(TAG, "Failed to calculate decoded size");
        return ESP_FAIL;
    }
    
    // Allocate and decode
    output.resize(decoded_len);
    size_t actual_len = 0;
    
    int ret = mbedtls_base64_decode(output.data(), decoded_len, &actual_len,
                                   reinterpret_cast<const uint8_t*>(base64_audio.c_str()),
                                   base64_audio.length());
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 decoding failed: %d", ret);
        return ESP_FAIL;
    }
    
    output.resize(actual_len);
    return ESP_OK;
}

std::string GeminiApiClient::build_audio_request_json(const std::string& base64_audio) {
    cJSON* root = cJSON_CreateObject();
    cJSON* contents = cJSON_CreateArray();
    cJSON* content = cJSON_CreateObject();
    cJSON* parts = cJSON_CreateArray();
    cJSON* part = cJSON_CreateObject();
    cJSON* inline_data = cJSON_CreateObject();
    
    // Build the JSON structure for Gemini audio-to-audio request
    cJSON_AddStringToObject(inline_data, "mime_type", "audio/wav");
    cJSON_AddStringToObject(inline_data, "data", base64_audio.c_str());
    
    cJSON_AddItemToObject(part, "inline_data", inline_data);
    cJSON_AddItemToArray(parts, part);
    
    cJSON_AddItemToObject(content, "parts", parts);
    cJSON_AddItemToArray(contents, content);
    
    cJSON_AddItemToObject(root, "contents", contents);
    
    // Add generation config for audio output
    cJSON* generation_config = cJSON_CreateObject();
    cJSON* response_modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(response_modalities, cJSON_CreateString("AUDIO"));
    
    cJSON_AddItemToObject(generation_config, "response_modalities", response_modalities);
    cJSON_AddNumberToObject(generation_config, "temperature", 0.7);
    cJSON_AddNumberToObject(generation_config, "max_output_tokens", 1000);
    
    cJSON_AddItemToObject(root, "generationConfig", generation_config);
    
    char* json_string = cJSON_Print(root);
    std::string result(json_string);
    
    free(json_string);
    cJSON_Delete(root);
    
    return result;
}

esp_err_t GeminiApiClient::parse_audio_response(const std::string& response_json, AudioResponseCallback callback) {
    if (response_json.empty() || !callback) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON* root = cJSON_Parse(response_json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }
    
    // Navigate JSON structure: candidates[0].content.parts[0].inline_data.data
    cJSON* candidates = cJSON_GetObjectItem(root, "candidates");
    if (!cJSON_IsArray(candidates) || cJSON_GetArraySize(candidates) == 0) {
        ESP_LOGE(TAG, "No candidates in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    cJSON* first_candidate = cJSON_GetArrayItem(candidates, 0);
    cJSON* content = cJSON_GetObjectItem(first_candidate, "content");
    cJSON* parts = cJSON_GetObjectItem(content, "parts");
    
    if (!cJSON_IsArray(parts) || cJSON_GetArraySize(parts) == 0) {
        ESP_LOGE(TAG, "No parts in response content");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    // Process all audio parts
    int part_count = cJSON_GetArraySize(parts);
    for (int i = 0; i < part_count; i++) {
        cJSON* part = cJSON_GetArrayItem(parts, i);
        cJSON* inline_data = cJSON_GetObjectItem(part, "inline_data");
        
        if (inline_data) {
            cJSON* data_item = cJSON_GetObjectItem(inline_data, "data");
            if (cJSON_IsString(data_item)) {
                std::string base64_audio = cJSON_GetStringValue(data_item);
                
                // Decode audio data
                std::vector<uint8_t> audio_data;
                esp_err_t ret = decode_audio_response(base64_audio, audio_data);
                if (ret == ESP_OK && !audio_data.empty()) {
                    bool is_final = (i == part_count - 1);
                    callback(audio_data.data(), audio_data.size(), is_final);
                    ESP_LOGI(TAG, "Processed audio part %d/%d (%zu bytes)", 
                            i + 1, part_count, audio_data.size());
                } else {
                    ESP_LOGW(TAG, "Failed to decode audio part %d", i);
                }
            }
        }
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

} // namespace btspeaker 