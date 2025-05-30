//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_HTTP_CLIENT_H
#define BT_SPEAKER_HTTP_CLIENT_H

#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <atomic>

extern "C" {
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "cJSON.h"
}

namespace smart_speaker {

// HTTP methods
enum class HttpMethod : uint8_t {
    GET,
    POST,
    PUT,
    DELETE,
    PATCHill
};

// HTTP response structure
struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    size_t content_length = 0;
    bool success = false;
    std::string error_message;
    
    bool is_success() const { 
        return success && status_code >= 200 && status_code < 300; 
    }
    
    bool is_json() const {
        auto it = headers.find("content-type");
        return it != headers.end() && 
               it->second.find("application/json") != std::string::npos;
    }
};

// HTTP request configuration
struct HttpRequest {
    HttpMethod method = HttpMethod::GET;
    std::string url;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    uint32_t timeout_ms = 10000;  // 10 second default timeout
    uint32_t max_retries = 3;
    bool verify_ssl = true;
    
    void set_json_body(const std::string& json_data) {
        body = json_data;
        headers["Content-Type"] = "application/json";
    }
    
    void set_bearer_token(const std::string& token) {
        headers["Authorization"] = "Bearer " + token;
    }
    
    void set_api_key(const std::string& key, const std::string& header_name = "X-API-Key") {
        headers[header_name] = key;
    }
    
    bool is_valid() const {
        return !url.empty() && url.find("http") == 0;
    }
};

// HTTP client event callbacks
using HttpProgressCallback = std::function<void(size_t bytes_sent, size_t total_bytes)>;
using HttpErrorCallback = std::function<void(const std::string& error)>;

// Advanced HTTP Client for Cloud APIs
class HttpClient {
private:
    esp_http_client_handle_t client_handle_;
    std::atomic<bool> initialized_;
    
    // Default configuration
    std::string user_agent_;
    uint32_t default_timeout_ms_;
    bool verify_ssl_default_;
    
    // Callbacks
    HttpProgressCallback progress_callback_;
    HttpErrorCallback error_callback_;
    
    // Performance tracking
    mutable std::atomic<uint32_t> requests_made_;
    mutable std::atomic<uint32_t> requests_failed_;
    mutable std::atomic<uint32_t> total_bytes_sent_;
    mutable std::atomic<uint32_t> total_bytes_received_;
    
    static constexpr const char* TAG = "HTTP_CLIENT";
    
public:
    HttpClient();
    ~HttpClient();
    
    // Non-copyable, non-movable
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    
    // Initialization
    bool initialize(const std::string& user_agent = "ESP32-SmartSpeaker/1.0");
    void shutdown();
    
    // Configuration
    void set_default_timeout(uint32_t timeout_ms) { default_timeout_ms_ = timeout_ms; }
    void set_ssl_verification(bool verify) { verify_ssl_default_ = verify; }
    void set_progress_callback(HttpProgressCallback callback) { progress_callback_ = std::move(callback); }
    void set_error_callback(HttpErrorCallback callback) { error_callback_ = std::move(callback); }
    
    // HTTP request methods
    HttpResponse get(const std::string& url, 
                     const std::unordered_map<std::string, std::string>& headers = {});
    
    HttpResponse post(const std::string& url, 
                      const std::string& body,
                      const std::unordered_map<std::string, std::string>& headers = {});
    
    HttpResponse put(const std::string& url,
                     const std::string& body,
                     const std::unordered_map<std::string, std::string>& headers = {});
    
    HttpResponse delete_request(const std::string& url,
                               const std::unordered_map<std::string, std::string>& headers = {});
    
    // Generic request method
    HttpResponse execute_request(const HttpRequest& request);
    
    // JSON convenience methods
    HttpResponse get_json(const std::string& url,
                         const std::unordered_map<std::string, std::string>& headers = {});
    
    HttpResponse post_json(const std::string& url,
                          const cJSON* json_data,
                          const std::unordered_map<std::string, std::string>& headers = {});
    
    HttpResponse post_json(const std::string& url,
                          const std::string& json_string,
                          const std::unordered_map<std::string, std::string>& headers = {});
    
    // Authentication helpers
    HttpResponse authenticated_get(const std::string& url,
                                  const std::string& bearer_token);
    
    HttpResponse authenticated_post(const std::string& url,
                                   const std::string& body,
                                   const std::string& bearer_token);
    
    // File upload/download
    HttpResponse upload_data(const std::string& url,
                            const uint8_t* data,
                            size_t data_size,
                            const std::string& content_type,
                            const std::unordered_map<std::string, std::string>& headers = {});
    
    HttpResponse download_to_buffer(const std::string& url,
                                   std::vector<uint8_t>& buffer,
                                   const std::unordered_map<std::string, std::string>& headers = {});
    
    // Network utilities
    static bool check_internet_connectivity();
    static std::string url_encode(const std::string& value);
    static std::string url_decode(const std::string& value);
    
    // JSON utilities
    static cJSON* parse_json_response(const HttpResponse& response);
    static std::string json_to_string(const cJSON* json);
    static bool extract_json_string(const cJSON* json, const char* key, std::string& value);
    static bool extract_json_int(const cJSON* json, const char* key, int& value);
    static bool extract_json_double(const cJSON* json, const char* key, double& value);
    
    // Status and monitoring
    bool is_initialized() const { return initialized_.load(std::memory_order_acquire); }
    
    struct HttpMetrics {
        uint32_t requests_made = 0;
        uint32_t requests_failed = 0;
        uint32_t total_bytes_sent = 0;
        uint32_t total_bytes_received = 0;
        float success_rate = 0.0f;
    };
    
    HttpMetrics get_metrics() const;
    void reset_metrics();
    void print_status() const;
    
private:
    // Internal request handling
    HttpResponse internal_request(esp_http_client_handle_t client);
    bool setup_client_config(esp_http_client_config_t& config, const HttpRequest& request);
    
    // Response processing
    void process_response_headers(esp_http_client_handle_t client, HttpResponse& response);
    bool read_response_body(esp_http_client_handle_t client, HttpResponse& response);
    
    // Error handling
    void handle_request_error(const std::string& url, const std::string& error);
    std::string get_esp_error_string(esp_err_t err);
    
    // ESP HTTP client event handlers
    static esp_err_t http_event_handler(esp_http_client_event_t* evt);
    
    // Utility methods
    const char* method_to_string(HttpMethod method) const;
    bool is_secure_url(const std::string& url) const;
};

// Factory for creating HTTP client
namespace HttpClientFactory {
    // Create HTTP client with default configuration
    std::unique_ptr<HttpClient> create_default();
    
    // Create HTTP client optimized for API calls
    std::unique_ptr<HttpClient> create_api_client();
    
    // Create HTTP client for file downloads
    std::unique_ptr<HttpClient> create_download_client();
}

} // namespace smart_speaker

#endif //BT_SPEAKER_HTTP_CLIENT_H 