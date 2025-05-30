//
// Created by Shaked Melman on 29/05/2025.
//

#include "http_client.hpp"
#include <algorithm>
#include <cstring>

extern "C" {
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_err.h"
}

namespace smart_speaker {

// Constructor
HttpClient::HttpClient() 
    : client_handle_(nullptr),
      initialized_(false),
      user_agent_("ESP32-SmartSpeaker/1.0"),
      default_timeout_ms_(10000),
      verify_ssl_default_(true),
      requests_made_(0),
      requests_failed_(0),
      total_bytes_sent_(0),
      total_bytes_received_(0) {
}

// Destructor
HttpClient::~HttpClient() {
    shutdown();
}

// Initialize HTTP client
bool HttpClient::initialize(const std::string& user_agent) {
    if (initialized_.load(std::memory_order_acquire)) {
        return true;
    }
    
    ESP_LOGI(TAG, "Initializing HTTP client with user agent: %s", user_agent.c_str());
    
    user_agent_ = user_agent;
    
    // Initialize networking if not already done
    esp_netif_init();
    
    initialized_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "HTTP client initialized successfully");
    return true;
}

// Shutdown HTTP client
void HttpClient::shutdown() {
    if (!initialized_.load(std::memory_order_acquire)) {
        return;
    }
    
    ESP_LOGI(TAG, "Shutting down HTTP client");
    
    if (client_handle_) {
        esp_http_client_cleanup(client_handle_);
        client_handle_ = nullptr;
    }
    
    initialized_.store(false, std::memory_order_release);
}

// GET request
HttpResponse HttpClient::get(const std::string& url, 
                            const std::unordered_map<std::string, std::string>& headers) {
    HttpRequest request;
    request.method = HttpMethod::GET;
    request.url = url;
    request.headers = headers;
    return execute_request(request);
}

// POST request
HttpResponse HttpClient::post(const std::string& url, 
                             const std::string& body,
                             const std::unordered_map<std::string, std::string>& headers) {
    HttpRequest request;
    request.method = HttpMethod::POST;
    request.url = url;
    request.body = body;
    request.headers = headers;
    return execute_request(request);
}

// PUT request
HttpResponse HttpClient::put(const std::string& url,
                            const std::string& body,
                            const std::unordered_map<std::string, std::string>& headers) {
    HttpRequest request;
    request.method = HttpMethod::PUT;
    request.url = url;
    request.body = body;
    request.headers = headers;
    return execute_request(request);
}

// DELETE request
HttpResponse HttpClient::delete_request(const std::string& url,
                                       const std::unordered_map<std::string, std::string>& headers) {
    HttpRequest request;
    request.method = HttpMethod::DELETE;
    request.url = url;
    request.headers = headers;
    return execute_request(request);
}

// Execute HTTP request
HttpResponse HttpClient::execute_request(const HttpRequest& request) {
    HttpResponse response;
    
    if (!initialized_.load(std::memory_order_acquire)) {
        response.error_message = "HTTP client not initialized";
        handle_request_error(request.url, response.error_message);
        return response;
    }
    
    if (!request.is_valid()) {
        response.error_message = "Invalid request URL";
        handle_request_error(request.url, response.error_message);
        return response;
    }
    
    ESP_LOGI(TAG, "Executing %s request to: %s", method_to_string(request.method), request.url.c_str());
    
    // Setup HTTP client configuration
    esp_http_client_config_t config = {};
    if (!setup_client_config(config, request)) {
        response.error_message = "Failed to setup client configuration";
        return response;
    }
    
    // Create HTTP client
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        response.error_message = "Failed to initialize HTTP client";
        handle_request_error(request.url, response.error_message);
        return response;
    }
    
    // Set HTTP method
    esp_http_client_method_t esp_method = ESP_HTTP_METHOD_GET;
    switch (request.method) {
        case HttpMethod::GET: esp_method = ESP_HTTP_METHOD_GET; break;
        case HttpMethod::POST: esp_method = ESP_HTTP_METHOD_POST; break;
        case HttpMethod::PUT: esp_method = ESP_HTTP_METHOD_PUT; break;
        case HttpMethod::DELETE: esp_method = ESP_HTTP_METHOD_DELETE; break;
        case HttpMethod::PATCH: esp_method = ESP_HTTP_METHOD_PATCH; break;
    }
    esp_http_client_set_method(client, esp_method);
    
    // Set headers
    for (const auto& header : request.headers) {
        esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
    }
    
    // Set User-Agent
    esp_http_client_set_header(client, "User-Agent", user_agent_.c_str());
    
    // Set request body for POST/PUT
    if (!request.body.empty() && (request.method == HttpMethod::POST || request.method == HttpMethod::PUT)) {
        esp_http_client_set_post_field(client, request.body.c_str(), request.body.length());
    }
    
    // Perform request with retries
    bool success = false;
    for (uint32_t retry = 0; retry <= request.max_retries && !success; ++retry) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Retrying request (attempt %lu/%lu)", retry + 1, request.max_retries + 1);
            vTaskDelay(pdMS_TO_TICKS(1000 * retry));  // Exponential backoff
        }
        
        response = internal_request(client);
        success = response.is_success();
        
        if (!success && retry < request.max_retries) {
            ESP_LOGW(TAG, "Request failed (status: %d), retrying...", response.status_code);
        }
    }
    
    // Clean up
    esp_http_client_cleanup(client);
    
    // Update metrics
    requests_made_.fetch_add(1, std::memory_order_relaxed);
    if (!success) {
        requests_failed_.fetch_add(1, std::memory_order_relaxed);
    }
    total_bytes_sent_.fetch_add(request.body.size(), std::memory_order_relaxed);
    total_bytes_received_.fetch_add(response.body.size(), std::memory_order_relaxed);
    
    ESP_LOGI(TAG, "Request completed: %s (status: %d)", 
             success ? "SUCCESS" : "FAILED", response.status_code);
    
    return response;
}

// JSON GET request
HttpResponse HttpClient::get_json(const std::string& url,
                                 const std::unordered_map<std::string, std::string>& headers) {
    auto json_headers = headers;
    json_headers["Accept"] = "application/json";
    return get(url, json_headers);
}

// JSON POST request with cJSON
HttpResponse HttpClient::post_json(const std::string& url,
                                  const cJSON* json_data,
                                  const std::unordered_map<std::string, std::string>& headers) {
    if (!json_data) {
        HttpResponse response;
        response.error_message = "Invalid JSON data";
        return response;
    }
    
    char* json_string = cJSON_Print(json_data);
    if (!json_string) {
        HttpResponse response;
        response.error_message = "Failed to serialize JSON";
        return response;
    }
    
    HttpResponse response = post_json(url, std::string(json_string), headers);
    free(json_string);
    return response;
}

// JSON POST request with string
HttpResponse HttpClient::post_json(const std::string& url,
                                  const std::string& json_string,
                                  const std::unordered_map<std::string, std::string>& headers) {
    auto json_headers = headers;
    json_headers["Content-Type"] = "application/json";
    json_headers["Accept"] = "application/json";
    return post(url, json_string, json_headers);
}

// Authenticated GET request
HttpResponse HttpClient::authenticated_get(const std::string& url,
                                          const std::string& bearer_token) {
    std::unordered_map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + bearer_token;
    return get(url, headers);
}

// Authenticated POST request
HttpResponse HttpClient::authenticated_post(const std::string& url,
                                           const std::string& body,
                                           const std::string& bearer_token) {
    std::unordered_map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + bearer_token;
    return post(url, body, headers);
}

// Check internet connectivity
bool HttpClient::check_internet_connectivity() {
    // Check if WiFi is connected
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    
    if (ret != ESP_OK) {
        ESP_LOGD("HTTP_CLIENT", "WiFi not connected");
        return false;
    }
    
    // Simple connectivity test (ping a reliable server)
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/get",
        .timeout_ms = 5000,
        .skip_cert_common_name_check = true,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    bool connected = (err == ESP_OK && status_code == 200);
    ESP_LOGD("HTTP_CLIENT", "Internet connectivity: %s", connected ? "YES" : "NO");
    return connected;
}

// URL encoding
std::string HttpClient::url_encode(const std::string& value) {
    std::string encoded;
    encoded.reserve(value.length() * 3);
    
    for (char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            encoded += '%';
            encoded += "0123456789ABCDEF"[c >> 4];
            encoded += "0123456789ABCDEF"[c & 15];
        }
    }
    
    return encoded;
}

// JSON parsing
cJSON* HttpClient::parse_json_response(const HttpResponse& response) {
    if (!response.is_success() || response.body.empty()) {
        return nullptr;
    }
    
    cJSON* json = cJSON_Parse(response.body.c_str());
    if (!json) {
        ESP_LOGW("HTTP_CLIENT", "Failed to parse JSON response");
    }
    
    return json;
}

// JSON to string
std::string HttpClient::json_to_string(const cJSON* json) {
    if (!json) {
        return "";
    }
    
    char* json_string = cJSON_Print(json);
    if (!json_string) {
        return "";
    }
    
    std::string result(json_string);
    free(json_string);
    return result;
}

// Extract JSON string value
bool HttpClient::extract_json_string(const cJSON* json, const char* key, std::string& value) {
    if (!json || !key) {
        return false;
    }
    
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    
    value = item->valuestring;
    return true;
}

// Extract JSON integer value
bool HttpClient::extract_json_int(const cJSON* json, const char* key, int& value) {
    if (!json || !key) {
        return false;
    }
    
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    
    value = item->valueint;
    return true;
}

// Get metrics
HttpClient::HttpMetrics HttpClient::get_metrics() const {
    HttpMetrics metrics;
    metrics.requests_made = requests_made_.load(std::memory_order_relaxed);
    metrics.requests_failed = requests_failed_.load(std::memory_order_relaxed);
    metrics.total_bytes_sent = total_bytes_sent_.load(std::memory_order_relaxed);
    metrics.total_bytes_received = total_bytes_received_.load(std::memory_order_relaxed);
    
    if (metrics.requests_made > 0) {
        metrics.success_rate = (float)(metrics.requests_made - metrics.requests_failed) / metrics.requests_made;
    }
    
    return metrics;
}

// Internal request implementation
HttpResponse HttpClient::internal_request(esp_http_client_handle_t client) {
    HttpResponse response;
    
    esp_err_t err = esp_http_client_perform(client);
    
    response.status_code = esp_http_client_get_status_code(client);
    response.content_length = esp_http_client_get_content_length(client);
    
    if (err == ESP_OK) {
        response.success = true;
        
        // Process headers
        process_response_headers(client, response);
        
        // Read response body
        read_response_body(client, response);
        
    } else {
        response.success = false;
        response.error_message = get_esp_error_string(err);
        ESP_LOGE(TAG, "HTTP request failed: %s", response.error_message.c_str());
    }
    
    return response;
}

// Setup client configuration
bool HttpClient::setup_client_config(esp_http_client_config_t& config, const HttpRequest& request) {
    memset(&config, 0, sizeof(config));
    
    config.url = request.url.c_str();
    config.timeout_ms = request.timeout_ms > 0 ? request.timeout_ms : default_timeout_ms_;
    config.skip_cert_common_name_check = !request.verify_ssl;
    config.event_handler = http_event_handler;
    
    return true;
}

// Process response headers
void HttpClient::process_response_headers(esp_http_client_handle_t client, HttpResponse& response) {
    // Get common headers
    int content_length = esp_http_client_get_content_length(client);
    if (content_length >= 0) {
        response.headers["content-length"] = std::to_string(content_length);
    }
    
    // Note: ESP-IDF doesn't provide easy access to all response headers
    // This would need to be implemented using the event handler if needed
}

// Read response body
bool HttpClient::read_response_body(esp_http_client_handle_t client, HttpResponse& response) {
    int content_length = esp_http_client_get_content_length(client);
    
    if (content_length > 0) {
        response.body.reserve(content_length);
        
        char buffer[1024];
        int read_len;
        
        while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
            response.body.append(buffer, read_len);
        }
        
        return read_len >= 0;  // 0 means end of stream, negative means error
    }
    
    return true;
}

// Handle request error
void HttpClient::handle_request_error(const std::string& url, const std::string& error) {
    ESP_LOGE(TAG, "HTTP request error for %s: %s", url.c_str(), error.c_str());
    
    if (error_callback_) {
        error_callback_(error);
    }
}

// Get ESP error string
std::string HttpClient::get_esp_error_string(esp_err_t err) {
    switch (err) {
        case ESP_ERR_HTTP_CONNECT: return "Connection failed";
        case ESP_ERR_HTTP_WRITE_DATA: return "Write data failed";
        case ESP_ERR_HTTP_FETCH_HEADER: return "Fetch header failed";
        case ESP_ERR_HTTP_INVALID_TRANSPORT: return "Invalid transport";
        case ESP_FAIL: return "General failure";
        default: return "Unknown error (" + std::to_string(err) + ")";
    }
}

// HTTP event handler
esp_err_t HttpClient::http_event_handler(esp_http_client_event_t* evt) {
    // Basic event handling - can be extended for progress tracking
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD("HTTP_CLIENT", "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD("HTTP_CLIENT", "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD("HTTP_CLIENT", "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD("HTTP_CLIENT", "HTTP_EVENT_ON_HEADER, key=%s, value=%s", 
                    evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD("HTTP_CLIENT", "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD("HTTP_CLIENT", "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD("HTTP_CLIENT", "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Method to string
const char* HttpClient::method_to_string(HttpMethod method) const {
    switch (method) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE: return "DELETE";
        case HttpMethod::PATCH: return "PATCH";
        default: return "UNKNOWN";
    }
}

// Factory implementations
namespace HttpClientFactory {

std::unique_ptr<HttpClient> create_default() {
    auto client = std::make_unique<HttpClient>();
    if (client->initialize()) {
        return client;
    }
    return nullptr;
}

std::unique_ptr<HttpClient> create_api_client() {
    auto client = std::make_unique<HttpClient>();
    if (client->initialize("ESP32-SmartSpeaker-API/1.0")) {
        client->set_default_timeout(15000);  // 15 seconds for API calls
        return client;
    }
    return nullptr;
}

std::unique_ptr<HttpClient> create_download_client() {
    auto client = std::make_unique<HttpClient>();
    if (client->initialize("ESP32-SmartSpeaker-Download/1.0")) {
        client->set_default_timeout(30000);  // 30 seconds for downloads
        return client;
    }
    return nullptr;
}

} // namespace HttpClientFactory

} // namespace smart_speaker 