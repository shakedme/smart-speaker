//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_SPOTIFY_MANAGER_H
#define BT_SPEAKER_SPOTIFY_MANAGER_H

#pragma once

#include "event_bus.hpp"
#include "task_wrapper.hpp"
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include <unordered_map>

extern "C" {
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
}

namespace smart_speaker {

// Spotify playback states
enum class SpotifyPlaybackState : uint8_t {
    Unknown,
    Playing,
    Paused,
    Stopped,
    Buffering,
    Error
};

// Music genres and moods for LLM integration
enum class MusicGenre : uint8_t {
    Unknown, Pop, Rock, Jazz, Classical, Electronic, HipHop, Country, 
    Reggae, Blues, Folk, Metal, Punk, Ambient, Chill, Workout
};

enum class MusicMood : uint8_t {
    Unknown, Happy, Sad, Energetic, Relaxed, Focused, Romantic, 
    Party, Nostalgic, Angry, Peaceful, Motivational
};

// Spotify track information
struct SpotifyTrack {
    std::string id;
    std::string name;
    std::string artist;
    std::string album;
    uint32_t duration_ms = 0;
    uint32_t popularity = 0;
    std::vector<std::string> genres;
    std::string preview_url;
    
    bool is_valid() const { return !id.empty() && !name.empty(); }
};

// Spotify playlist information
struct SpotifyPlaylist {
    std::string id;
    std::string name;
    std::string description;
    uint32_t track_count = 0;
    std::vector<SpotifyTrack> tracks;
    
    bool is_valid() const { return !id.empty() && !name.empty(); }
};

// Current playback status
struct SpotifyPlaybackStatus {
    SpotifyPlaybackState state = SpotifyPlaybackState::Unknown;
    SpotifyTrack current_track;
    uint32_t progress_ms = 0;
    uint8_t volume = 50;  // 0-100
    bool shuffle = false;
    bool repeat = false;
    
    float progress_percentage() const {
        return current_track.duration_ms > 0 ? 
               (float)progress_ms / current_track.duration_ms * 100.0f : 0.0f;
    }
};

// Music search/request parameters for LLM integration
struct MusicRequest {
    std::string query;                    // Raw search query
    std::vector<MusicGenre> genres;       // Preferred genres
    std::vector<MusicMood> moods;         // Desired moods
    std::string artist;                   // Specific artist
    std::string album;                    // Specific album
    std::string activity;                 // "workout", "study", "party"
    uint32_t energy_level = 50;           // 0-100 (calm to energetic)
    uint32_t danceability = 50;           // 0-100
    uint32_t valence = 50;                // 0-100 (sad to happy)
    bool instrumental_preferred = false;   // Prefer instrumental tracks
    
    bool is_empty() const { 
        return query.empty() && genres.empty() && artist.empty(); 
    }
};

// Spotify API configuration
struct SpotifyConfig {
    std::string client_id;
    std::string client_secret;
    std::string refresh_token;
    std::string access_token;
    uint64_t token_expires_at = 0;
    
    // API endpoints
    std::string api_base_url = "https://api.spotify.com/v1";
    std::string auth_url = "https://accounts.spotify.com/api/token";
    
    // Playback device (ESP32 speaker)
    std::string device_name = "ESP32-Speaker";
    std::string device_id;
    
    bool is_valid() const {
        return !client_id.empty() && !client_secret.empty() && 
               (!access_token.empty() || !refresh_token.empty());
    }
    
    bool needs_token_refresh() const {
        return esp_timer_get_time() / 1000000 >= token_expires_at;
    }
};

// Spotify Web API Manager with LLM Integration
class SpotifyManager {
private:
    SpotifyConfig config_;
    std::atomic<bool> initialized_;
    std::atomic<bool> connected_;
    
    // Current state
    SpotifyPlaybackStatus current_status_;
    std::vector<SpotifyPlaylist> user_playlists_;
    std::vector<SpotifyTrack> recent_tracks_;
    
    // HTTP client for API calls
    esp_http_client_handle_t http_client_;
    
    // Background tasks
    std::unique_ptr<Task> status_update_task_;
    std::unique_ptr<Task> token_refresh_task_;
    
    // Performance tracking
    mutable std::atomic<uint32_t> api_calls_made_;
    mutable std::atomic<uint32_t> api_errors_;
    mutable std::atomic<uint32_t> tracks_played_;
    
    static constexpr const char* TAG = "SPOTIFY_MANAGER";
    
public:
    explicit SpotifyManager(const SpotifyConfig& config);
    ~SpotifyManager();
    
    // Non-copyable, non-movable
    SpotifyManager(const SpotifyManager&) = delete;
    SpotifyManager& operator=(const SpotifyManager&) = delete;
    
    // Lifecycle management
    bool initialize();
    bool connect();
    void disconnect();
    void shutdown();
    
    // Authentication
    bool authenticate();
    bool refresh_access_token();
    void set_access_token(const std::string& token, uint32_t expires_in_seconds);
    
    // Playback control (LLM-driven)
    bool play_music(const MusicRequest& request);
    bool play_track(const std::string& track_id);
    bool play_playlist(const std::string& playlist_id);
    bool play_artist(const std::string& artist_name);
    bool play_album(const std::string& album_name);
    
    // Transport controls
    bool play();
    bool pause();
    bool next_track();
    bool previous_track();
    bool seek_to_position(uint32_t position_ms);
    
    // Volume control
    bool set_volume(uint8_t volume);  // 0-100
    bool volume_up(uint8_t step = 10);
    bool volume_down(uint8_t step = 10);
    
    // Playlist and library management
    bool get_user_playlists();
    bool get_recently_played();
    bool add_to_queue(const std::string& track_id);
    bool create_playlist(const std::string& name, const std::vector<std::string>& track_ids);
    
    // Smart search (LLM integration)
    std::vector<SpotifyTrack> search_tracks(const std::string& query, uint32_t limit = 20);
    std::vector<SpotifyTrack> get_recommendations(const MusicRequest& request);
    std::vector<SpotifyTrack> find_similar_music(const std::string& track_id);
    
    // Status and information
    SpotifyPlaybackStatus get_current_status();
    bool is_connected() const { return connected_.load(std::memory_order_acquire); }
    bool is_playing() const { return current_status_.state == SpotifyPlaybackState::Playing; }
    const SpotifyTrack& get_current_track() const { return current_status_.current_track; }
    
    // User queries (for LLM responses)
    std::string get_current_track_info() const;
    std::string get_artist_info(const std::string& artist_name);
    std::vector<std::string> get_track_lyrics(const std::string& track_id);  // If available
    
    // Configuration
    void set_config(const SpotifyConfig& config) { config_ = config; }
    SpotifyConfig get_config() const { return config_; }
    
    // Performance metrics
    struct SpotifyMetrics {
        uint32_t api_calls_made = 0;
        uint32_t api_errors = 0;
        uint32_t tracks_played = 0;
        uint32_t playlists_loaded = 0;
        float api_success_rate = 0.0f;
    };
    
    SpotifyMetrics get_metrics() const;
    void reset_metrics();
    void print_status() const;
    
private:
    // Background tasks
    void status_update_loop();
    void token_refresh_loop();
    
    // HTTP API helpers
    bool make_api_request(const std::string& endpoint, const std::string& method, 
                         const std::string& data, std::string& response);
    bool make_authenticated_request(const std::string& endpoint, const std::string& method,
                                   const std::string& data, std::string& response);
    
    // Response parsing
    bool parse_track_info(const std::string& json_response, SpotifyTrack& track);
    bool parse_playlist_info(const std::string& json_response, SpotifyPlaylist& playlist);
    bool parse_playback_status(const std::string& json_response, SpotifyPlaybackStatus& status);
    bool parse_search_results(const std::string& json_response, std::vector<SpotifyTrack>& tracks);
    
    // Music intelligence (LLM integration helpers)
    std::string build_search_query(const MusicRequest& request);
    std::string build_recommendation_params(const MusicRequest& request);
    MusicGenre classify_genre(const std::string& genre_string);
    MusicMood classify_mood(const std::string& mood_string);
    
    // Device management
    bool register_device();
    bool set_active_device();
    
    // Error handling
    void handle_api_error(const std::string& endpoint, const std::string& error);
    
    // Utility methods
    std::string get_auth_header() const;
    std::string url_encode(const std::string& value);
    const char* playback_state_to_string(SpotifyPlaybackState state) const;
    const char* genre_to_string(MusicGenre genre) const;
    const char* mood_to_string(MusicMood mood) const;
};

// Factory for creating Spotify manager
namespace SpotifyManagerFactory {
    // Create Spotify manager with user credentials
    std::unique_ptr<SpotifyManager> create_with_credentials(
        const std::string& client_id, 
        const std::string& client_secret,
        const std::string& refresh_token = ""
    );
    
    // Create Spotify manager from stored config
    std::unique_ptr<SpotifyManager> create_from_config(const SpotifyConfig& config);
}

} // namespace smart_speaker

#endif //BT_SPEAKER_SPOTIFY_MANAGER_H 