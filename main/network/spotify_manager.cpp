//
// Created by Shaked Melman on 29/05/2025.
//

#include "spotify_manager.hpp"
#include "http_client.hpp"
#include <algorithm>
#include <sstream>

extern "C" {
#include "esp_timer.h"
#include "mbedtls/base64.h"
}

namespace smart_speaker {

// Constructor
SpotifyManager::SpotifyManager(const SpotifyConfig& config)
    : config_(config),
      initialized_(false),
      connected_(false),
      http_client_(nullptr),
      api_calls_made_(0),
      api_errors_(0),
      tracks_played_(0) {
}

// Destructor
SpotifyManager::~SpotifyManager() {
    shutdown();
}

// Initialize Spotify manager
bool SpotifyManager::initialize() {
    if (initialized_.load(std::memory_order_acquire)) {
        return true;
    }
    
    ESP_LOGI(TAG, "Initializing Spotify manager");
    
    if (!config_.is_valid()) {
        ESP_LOGE(TAG, "Invalid Spotify configuration");
        return false;
    }
    
    // Create HTTP client for API calls
    http_client_ = esp_http_client_init(&(esp_http_client_config_t){
        .url = config_.api_base_url.c_str(),
        .timeout_ms = 15000,
        .skip_cert_common_name_check = false,
    });
    
    if (!http_client_) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }
    
    initialized_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Spotify manager initialized successfully");
    return true;
}

// Connect to Spotify service
bool SpotifyManager::connect() {
    if (!initialized_.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "Spotify manager not initialized");
        return false;
    }
    
    if (connected_.load(std::memory_order_acquire)) {
        return true;
    }
    
    ESP_LOGI(TAG, "Connecting to Spotify service");
    
    // Authenticate with Spotify
    if (!authenticate()) {
        ESP_LOGE(TAG, "Spotify authentication failed");
        return false;
    }
    
    // Register this device for playback
    if (!register_device()) {
        ESP_LOGW(TAG, "Device registration failed, but continuing");
    }
    
    // Start background tasks
    status_update_task_ = TaskFactory::create_network_task("spotify_status", 
        [this]() { status_update_loop(); });
    
    token_refresh_task_ = TaskFactory::create_network_task("spotify_token",
        [this]() { token_refresh_loop(); });
    
    connected_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Connected to Spotify service successfully");
    return true;
}

// Disconnect from Spotify service
void SpotifyManager::disconnect() {
    if (!connected_.load(std::memory_order_acquire)) {
        return;
    }
    
    ESP_LOGI(TAG, "Disconnecting from Spotify service");
    
    // Stop background tasks
    if (status_update_task_) {
        status_update_task_->stop();
        status_update_task_.reset();
    }
    
    if (token_refresh_task_) {
        token_refresh_task_->stop();
        token_refresh_task_.reset();
    }
    
    connected_.store(false, std::memory_order_release);
    ESP_LOGI(TAG, "Disconnected from Spotify service");
}

// Shutdown Spotify manager
void SpotifyManager::shutdown() {
    disconnect();
    
    if (http_client_) {
        esp_http_client_cleanup(http_client_);
        http_client_ = nullptr;
    }
    
    initialized_.store(false, std::memory_order_release);
}

// Authenticate with Spotify
bool SpotifyManager::authenticate() {
    ESP_LOGI(TAG, "Authenticating with Spotify");
    
    if (config_.access_token.empty() && config_.refresh_token.empty()) {
        ESP_LOGE(TAG, "No access token or refresh token available");
        return false;
    }
    
    // If we have a valid access token, use it
    if (!config_.access_token.empty() && !config_.needs_token_refresh()) {
        ESP_LOGI(TAG, "Using existing access token");
        return true;
    }
    
    // Otherwise, refresh the token
    return refresh_access_token();
}

// Refresh access token using refresh token
bool SpotifyManager::refresh_access_token() {
    if (config_.refresh_token.empty()) {
        ESP_LOGE(TAG, "No refresh token available");
        return false;
    }
    
    ESP_LOGI(TAG, "Refreshing Spotify access token");
    
    // Prepare authentication header
    std::string auth_string = config_.client_id + ":" + config_.client_secret;
    size_t encoded_len;
    mbedtls_base64_encode(nullptr, 0, &encoded_len, (unsigned char*)auth_string.c_str(), auth_string.length());
    
    std::vector<char> encoded_auth(encoded_len + 1);
    mbedtls_base64_decode((unsigned char*)encoded_auth.data(), encoded_len + 1, &encoded_len,
                     (unsigned char*)auth_string.c_str(), auth_string.length());
    encoded_auth[encoded_len] = '\0';
    
    // Prepare request body
    std::string body = "grant_type=refresh_token&refresh_token=" + config_.refresh_token;
    
    // Setup HTTP client for token refresh
    esp_http_client_config_t auth_config = {
        .url = config_.auth_url.c_str(),
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t auth_client = esp_http_client_init(&auth_config);
    if (!auth_client) {
        ESP_LOGE(TAG, "Failed to create auth client");
        return false;
    }
    
    // Set headers
    esp_http_client_set_header(auth_client, "Authorization", 
                              ("Basic " + std::string(encoded_auth.data())).c_str());
    esp_http_client_set_header(auth_client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(auth_client, body.c_str(), body.length());
    
    // Perform request
    esp_err_t err = esp_http_client_perform(auth_client);
    int status_code = esp_http_client_get_status_code(auth_client);
    
    bool success = false;
    if (err == ESP_OK && status_code == 200) {
        // Read response
        int content_length = esp_http_client_get_content_length(auth_client);
        if (content_length > 0) {
            std::vector<char> response_data(content_length + 1);
            int read_len = esp_http_client_read(auth_client, response_data.data(), content_length);
            
            if (read_len > 0) {
                response_data[read_len] = '\0';
                
                // Parse JSON response
                cJSON* json = cJSON_Parse(response_data.data());
                if (json) {
                    // Extract access token
                    cJSON* access_token_item = cJSON_GetObjectItem(json, "access_token");
                    cJSON* expires_in_item = cJSON_GetObjectItem(json, "expires_in");
                    
                    if (cJSON_IsString(access_token_item) && cJSON_IsNumber(expires_in_item)) {
                        config_.access_token = access_token_item->valuestring;
                        
                        // Calculate expiration time
                        uint64_t current_time = esp_timer_get_time() / 1000000;  // Convert to seconds
                        config_.token_expires_at = current_time + expires_in_item->valueint - 300; // 5 min buffer
                        
                        ESP_LOGI(TAG, "Access token refreshed successfully");
                        success = true;
                    }
                    
                    cJSON_Delete(json);
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "Token refresh failed: status=%d, err=%d", status_code, err);
    }
    
    esp_http_client_cleanup(auth_client);
    return success;
}

// Play music based on request
bool SpotifyManager::play_music(const MusicRequest& request) {
    if (!connected_.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "Not connected to Spotify");
        return false;
    }
    
    ESP_LOGI(TAG, "Processing music request: %s", request.query.c_str());
    
    // First, search for tracks based on the request
    std::vector<SpotifyTrack> tracks = search_tracks(build_search_query(request));
    
    if (tracks.empty()) {
        ESP_LOGW(TAG, "No tracks found for request");
        return false;
    }
    
    // Play the first matching track
    return play_track(tracks[0].id);
}

// Play specific track
bool SpotifyManager::play_track(const std::string& track_id) {
    if (!connected_.load(std::memory_order_acquire)) {
        return false;
    }
    
    ESP_LOGI(TAG, "Playing track: %s", track_id.c_str());
    
    // Create JSON request body
    cJSON* json = cJSON_CreateObject();
    cJSON* uris = cJSON_CreateArray();
    cJSON_AddItemToArray(uris, cJSON_CreateString(("spotify:track:" + track_id).c_str()));
    cJSON_AddItemToObject(json, "uris", uris);
    
    // Make API request
    std::string response;
    bool success = make_authenticated_request("/me/player/play", "PUT", 
                                            HttpClient::json_to_string(json), response);
    
    cJSON_Delete(json);
    
    if (success) {
        tracks_played_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGI(TAG, "Track playback started successfully");
    }
    
    return success;
}

// Pause playback
bool SpotifyManager::pause() {
    if (!connected_.load(std::memory_order_acquire)) {
        return false;
    }
    
    ESP_LOGI(TAG, "Pausing playback");
    
    std::string response;
    return make_authenticated_request("/me/player/pause", "PUT", "", response);
}

// Resume playback
bool SpotifyManager::play() {
    if (!connected_.load(std::memory_order_acquire)) {
        return false;
    }
    
    ESP_LOGI(TAG, "Resuming playback");
    
    std::string response;
    return make_authenticated_request("/me/player/play", "PUT", "", response);
}

// Next track
bool SpotifyManager::next_track() {
    if (!connected_.load(std::memory_order_acquire)) {
        return false;
    }
    
    ESP_LOGI(TAG, "Skipping to next track");
    
    std::string response;
    return make_authenticated_request("/me/player/next", "POST", "", response);
}

// Previous track
bool SpotifyManager::previous_track() {
    if (!connected_.load(std::memory_order_acquire)) {
        return false;
    }
    
    ESP_LOGI(TAG, "Going to previous track");
    
    std::string response;
    return make_authenticated_request("/me/player/previous", "POST", "", response);
}

// Set volume
bool SpotifyManager::set_volume(uint8_t volume) {
    if (!connected_.load(std::memory_order_acquire)) {
        return false;
    }
    
    volume = std::min(volume, (uint8_t)100);  // Clamp to 0-100
    ESP_LOGI(TAG, "Setting volume to %d", volume);
    
    std::string endpoint = "/me/player/volume?volume_percent=" + std::to_string(volume);
    std::string response;
    return make_authenticated_request(endpoint, "PUT", "", response);
}

// Search for tracks
std::vector<SpotifyTrack> SpotifyManager::search_tracks(const std::string& query, uint32_t limit) {
    std::vector<SpotifyTrack> tracks;
    
    if (!connected_.load(std::memory_order_acquire) || query.empty()) {
        return tracks;
    }
    
    ESP_LOGI(TAG, "Searching for tracks: %s", query.c_str());
    
    // Build search endpoint
    std::string encoded_query = HttpClient::url_encode(query);
    std::string endpoint = "/search?q=" + encoded_query + 
                          "&type=track&limit=" + std::to_string(limit);
    
    std::string response;
    if (make_authenticated_request(endpoint, "GET", "", response)) {
        parse_search_results(response, tracks);
    }
    
    ESP_LOGI(TAG, "Found %zu tracks", tracks.size());
    return tracks;
}

// Get current playback status
SpotifyPlaybackStatus SpotifyManager::get_current_status() {
    SpotifyPlaybackStatus status;
    
    if (!connected_.load(std::memory_order_acquire)) {
        return status;
    }
    
    std::string response;
    if (make_authenticated_request("/me/player", "GET", "", response)) {
        parse_playback_status(response, status);
    }
    
    return status;
}

// Make authenticated API request
bool SpotifyManager::make_authenticated_request(const std::string& endpoint, 
                                               const std::string& method,
                                               const std::string& data, 
                                               std::string& response) {
    if (config_.access_token.empty()) {
        ESP_LOGE(TAG, "No access token available");
        return false;
    }
    
    // Check if token needs refresh
    if (config_.needs_token_refresh()) {
        if (!refresh_access_token()) {
            ESP_LOGE(TAG, "Failed to refresh access token");
            return false;
        }
    }
    
    // Setup request
    std::string full_url = config_.api_base_url + endpoint;
    esp_http_client_set_url(http_client_, full_url.c_str());
    
    // Set method
    esp_http_client_method_t esp_method = HTTP_METHOD_GET;
    if (method == "POST") esp_method = HTTP_METHOD_POST;
    else if (method == "PUT") esp_method = HTTP_METHOD_PUT;
    else if (method == "DELETE") esp_method = HTTP_METHOD_DELETE;
    
    esp_http_client_set_method(http_client_, esp_method);
    
    // Set headers
    std::string auth_header = "Bearer " + config_.access_token;
    esp_http_client_set_header(http_client_, "Authorization", auth_header.c_str());
    esp_http_client_set_header(http_client_, "Content-Type", "application/json");
    
    // Set body if provided
    if (!data.empty()) {
        esp_http_client_set_post_field(http_client_, data.c_str(), data.length());
    } else {
        esp_http_client_set_post_field(http_client_, nullptr, 0);
    }
    
    // Perform request
    esp_err_t err = esp_http_client_perform(http_client_);
    int status_code = esp_http_client_get_status_code(http_client_);
    
    api_calls_made_.fetch_add(1, std::memory_order_relaxed);
    
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        // Read response body
        int content_length = esp_http_client_get_content_length(http_client_);
        if (content_length > 0) {
            response.resize(content_length);
            esp_http_client_read(http_client_, &response[0], content_length);
        }
        return true;
    } else {
        api_errors_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGE(TAG, "API request failed: %s, status=%d", endpoint.c_str(), status_code);
        return false;
    }
}

// Build search query from music request
std::string SpotifyManager::build_search_query(const MusicRequest& request) {
    std::stringstream query;
    
    if (!request.query.empty()) {
        query << request.query;
    }
    
    if (!request.artist.empty()) {
        if (query.tellp() > 0) query << " ";
        query << "artist:" << request.artist;
    }
    
    if (!request.album.empty()) {
        if (query.tellp() > 0) query << " ";
        query << "album:" << request.album;
    }
    
    // Add genre filters
    for (const auto& genre : request.genres) {
        if (query.tellp() > 0) query << " ";
        query << "genre:" << genre_to_string(genre);
    }
    
    return query.str();
}

// Parse search results
bool SpotifyManager::parse_search_results(const std::string& json_response, 
                                         std::vector<SpotifyTrack>& tracks) {
    cJSON* json = cJSON_Parse(json_response.c_str());
    if (!json) {
        return false;
    }
    
    cJSON* tracks_obj = cJSON_GetObjectItem(json, "tracks");
    if (!tracks_obj) {
        cJSON_Delete(json);
        return false;
    }
    
    cJSON* items = cJSON_GetObjectItem(tracks_obj, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(json);
        return false;
    }
    
    int array_size = cJSON_GetArraySize(items);
    for (int i = 0; i < array_size; ++i) {
        cJSON* item = cJSON_GetArrayItem(items, i);
        
        SpotifyTrack track;
        if (parse_track_info(HttpClient::json_to_string(item), track)) {
            tracks.push_back(track);
        }
    }
    
    cJSON_Delete(json);
    return true;
}

// Parse track information
bool SpotifyManager::parse_track_info(const std::string& json_response, SpotifyTrack& track) {
    cJSON* json = cJSON_Parse(json_response.c_str());
    if (!json) {
        return false;
    }
    
    // Extract basic track info
    HttpClient::extract_json_string(json, "id", track.id);
    HttpClient::extract_json_string(json, "name", track.name);
    
    int duration_ms;
    if (HttpClient::extract_json_int(json, "duration_ms", duration_ms)) {
        track.duration_ms = duration_ms;
    }
    
    int popularity;
    if (HttpClient::extract_json_int(json, "popularity", popularity)) {
        track.popularity = popularity;
    }
    
    // Extract artist info
    cJSON* artists = cJSON_GetObjectItem(json, "artists");
    if (cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
        cJSON* first_artist = cJSON_GetArrayItem(artists, 0);
        std::string artist_name;
        if (HttpClient::extract_json_string(first_artist, "name", artist_name)) {
            track.artist = artist_name;
        }
    }
    
    // Extract album info
    cJSON* album = cJSON_GetObjectItem(json, "album");
    if (album) {
        HttpClient::extract_json_string(album, "name", track.album);
    }
    
    cJSON_Delete(json);
    return track.is_valid();
}

// Parse playback status
bool SpotifyManager::parse_playback_status(const std::string& json_response, 
                                          SpotifyPlaybackStatus& status) {
    cJSON* json = cJSON_Parse(json_response.c_str());
    if (!json) {
        return false;
    }
    
    // Parse playback state
    cJSON* is_playing = cJSON_GetObjectItem(json, "is_playing");
    if (cJSON_IsBool(is_playing)) {
        status.state = cJSON_IsTrue(is_playing) ? SpotifyPlaybackState::Playing : SpotifyPlaybackState::Paused;
    }
    
    // Parse progress
    int progress_ms;
    if (HttpClient::extract_json_int(json, "progress_ms", progress_ms)) {
        status.progress_ms = progress_ms;
    }
    
    // Parse current track
    cJSON* item = cJSON_GetObjectItem(json, "item");
    if (item) {
        parse_track_info(HttpClient::json_to_string(item), status.current_track);
    }
    
    cJSON_Delete(json);
    return true;
}

// Register device for playback
bool SpotifyManager::register_device() {
    // Note: ESP32 cannot act as a Spotify Connect device directly
    // This is a placeholder for potential future implementation
    ESP_LOGI(TAG, "Device registration not implemented for ESP32");
    return true;
}

// Status update loop
void SpotifyManager::status_update_loop() {
    ESP_LOGI(TAG, "Status update task started");
    
    while (!status_update_task_->should_stop()) {
        // Update current playback status every 5 seconds
        current_status_ = get_current_status();
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    ESP_LOGI(TAG, "Status update task stopped");
}

// Token refresh loop
void SpotifyManager::token_refresh_loop() {
    ESP_LOGI(TAG, "Token refresh task started");
    
    while (!token_refresh_task_->should_stop()) {
        // Check if token needs refresh every minute
        if (config_.needs_token_refresh()) {
            ESP_LOGI(TAG, "Access token expired, refreshing...");
            refresh_access_token();
        }
        
        vTaskDelay(pdMS_TO_TICKS(60000));  // Check every minute
    }
    
    ESP_LOGI(TAG, "Token refresh task stopped");
}

// Convert genre enum to string
const char* SpotifyManager::genre_to_string(MusicGenre genre) const {
    switch (genre) {
        case MusicGenre::Pop: return "pop";
        case MusicGenre::Rock: return "rock";
        case MusicGenre::Jazz: return "jazz";
        case MusicGenre::Classical: return "classical";
        case MusicGenre::Electronic: return "electronic";
        case MusicGenre::HipHop: return "hip-hop";
        case MusicGenre::Country: return "country";
        case MusicGenre::Blues: return "blues";
        case MusicGenre::Folk: return "folk";
        case MusicGenre::Metal: return "metal";
        default: return "unknown";
    }
}

// Get metrics
SpotifyManager::SpotifyMetrics SpotifyManager::get_metrics() const {
    SpotifyMetrics metrics;
    metrics.api_calls_made = api_calls_made_.load(std::memory_order_relaxed);
    metrics.api_errors = api_errors_.load(std::memory_order_relaxed);
    metrics.tracks_played = tracks_played_.load(std::memory_order_relaxed);
    
    if (metrics.api_calls_made > 0) {
        metrics.api_success_rate = (float)(metrics.api_calls_made - metrics.api_errors) / metrics.api_calls_made;
    }
    
    return metrics;
}

// Factory implementations
namespace SpotifyManagerFactory {

std::unique_ptr<SpotifyManager> create_with_credentials(
    const std::string& client_id, 
    const std::string& client_secret,
    const std::string& refresh_token) {
    
    SpotifyConfig config;
    config.client_id = client_id;
    config.client_secret = client_secret;
    config.refresh_token = refresh_token;
    
    auto manager = std::make_unique<SpotifyManager>(config);
    if (manager->initialize()) {
        return manager;
    }
    return nullptr;
}

std::unique_ptr<SpotifyManager> create_from_config(const SpotifyConfig& config) {
    auto manager = std::make_unique<SpotifyManager>(config);
    if (manager->initialize()) {
        return manager;
    }
    return nullptr;
}

} // namespace SpotifyManagerFactory

} // namespace smart_speaker 