#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <functional>
#include <map>
#include "esp_err.h"
#include "esp_http_server.h"
#include "LoginBlob.h"

namespace btspeaker {

    using AuthenticatedCallback = std::function<void()>;

    class SpotifyAuthManager {
    public:
        SpotifyAuthManager();

        ~SpotifyAuthManager();

        // Persistent auth
        esp_err_t loadStoredAuth();

        esp_err_t saveAuthBlob();

        void clearStoredAuth();

        // Zeroconf/Pairing
        esp_err_t startZeroconfDiscovery();

        void stopZeroconfDiscovery();

        // HTTP server
        esp_err_t startHttpServer();

        void stopHttpServer();

        // Authentication
        esp_err_t onSpotifyConnected(const std::shared_ptr<cspot::LoginBlob> &blob);

        // Validation
        bool isStoredAuthValid() const;

        // Debug method to compare auth blobs
        void compareAuthBlobs(const std::shared_ptr<cspot::LoginBlob> &storedBlob, 
                             const std::shared_ptr<cspot::LoginBlob> &incomingBlob) const;
        
        // Debug method to force re-authentication and comparison
        esp_err_t debugCompareWithFreshAuth();

        // State
        [[nodiscard]] bool isAuthenticated() const { return isAuthenticated_; }

        [[nodiscard]] std::shared_ptr<cspot::LoginBlob> getLoginBlob() const { return loginBlob_; }

        [[nodiscard]] std::string getStoredUsername() const { return storedUsername_; }

        // Callback for successful authentication
        void setAuthenticatedCallback(AuthenticatedCallback cb) { authenticatedCallback_ = std::move(cb); }

        // HTTP handlers (for registration)
        static esp_err_t spotifyInfoGetHandler(httpd_req_t *req);

        static esp_err_t spotifyInfoPostHandler(httpd_req_t *req);

        static esp_err_t closeHandler(httpd_req_t *req);

        // Helper
        static SpotifyAuthManager *getInstanceFromRequest(httpd_req_t *req);

        static std::string urlDecode(const std::string &str);

        static constexpr const char *NVS_NAMESPACE = "spotify_auth";
        static constexpr const char *NVS_AUTH_KEY = "auth_blob";

    private:
        std::shared_ptr<cspot::LoginBlob> loginBlob_;
        std::atomic<bool> isAuthenticated_;
        std::string storedUsername_;
        AuthenticatedCallback authenticatedCallback_;
        httpd_handle_t httpServer_ = nullptr;

        // Helper methods for spotifyInfoPostHandler
        static std::map<std::string, std::string> parseFormData(const std::string& content);
        static bool validateAuthParams(const std::map<std::string, std::string>& queryMap);
        static void sendAuthSuccessResponse(httpd_req_t* req);
        static void deferAuthenticationTask(SpotifyAuthManager* mgr);
    };

} // namespace btspeaker 