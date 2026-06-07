#include "ui_app.hpp"

#include "now_playing_screen.hpp"
#include "waveform_view.hpp"
#include <cstring>

extern "C" {
#include "esp_log.h"
}

namespace btspeaker {

namespace {
constexpr uint16_t kWaveformColor = 0x07E0;
constexpr uint16_t kColorWhite = 0xFFFF;
constexpr uint16_t kColorGray = 0x8410;
constexpr uint16_t kColorAccent = 0x5D1F;

bool stateEquals(const char *state, const char *expected) {
    return state && expected && std::strcmp(state, expected) == 0;
}
}  // namespace

UiApp::UiApp(DisplayBackend &display) : display_(display) {}

esp_err_t UiApp::init() {
    ESP_LOGI("UI", "[UI] Korvo-2 display UI initialized");
    bootPhase_ = BootPhase::Booting;
    return display_.init();
}

void UiApp::setBooting() {
    bootPhase_ = BootPhase::Booting;
    render();
}

void UiApp::setWifiConnecting() {
    bootPhase_ = BootPhase::WifiConnecting;
    render();
}

void UiApp::setWifiConnected() {
    bootPhase_ = BootPhase::WifiConnected;
    render();
}

void UiApp::setBackendConnecting() {
    bootPhase_ = BootPhase::BackendConnecting;
    render();
}

void UiApp::setIdle() {
    bootPhase_ = BootPhase::Ready;
    render();
}

void UiApp::updateNowPlaying(const NowPlayingUiState &state) {
    bootPhase_ = BootPhase::Ready;
    hasMetadata_ = state.title[0] != '\0' || state.artist[0] != '\0';
    nowPlaying_ = state;
    render();
}

void UiApp::updateWaveform(const WaveformUiState &state) {
    waveform_ = state;
    render();
}

void UiApp::updateSessionState(const char *state) {
    if (!state) {
        return;
    }
    bootPhase_ = BootPhase::Ready;
    std::strncpy(sessionState_, state, sizeof(sessionState_) - 1);
    sessionState_[sizeof(sessionState_) - 1] = '\0';
    render();
}

bool UiApp::shouldShowNowPlaying() const {
    if (!hasMetadata_) {
        return false;
    }
    return stateEquals(nowPlaying_.playback_state, "playing") ||
           stateEquals(nowPlaying_.playback_state, "paused");
}

bool UiApp::isMusicPlaying() const {
    return shouldShowNowPlaying() && stateEquals(nowPlaying_.playback_state, "playing");
}

bool UiApp::isListeningForCommand() const {
    return stateEquals(sessionState_, "LISTENING_FOR_COMMAND") ||
           stateEquals(sessionState_, "PROCESSING_INTENT") ||
           stateEquals(sessionState_, "WAITING_FOR_USER");
}

void UiApp::renderScreen(uint16_t *framebuffer, const uint16_t width, const uint16_t height) {
    const uint16_t center_y = height / 2U;

    if (bootPhase_ == BootPhase::Booting) {
        NowPlayingScreen::renderTextCentered(framebuffer, width, height, center_y,
                                             "Rufus is booting..", kColorWhite);
        return;
    }

    if (bootPhase_ == BootPhase::WifiConnecting) {
        NowPlayingScreen::renderTextCenteredPair(framebuffer, width, height, center_y,
                                                 "Rufus is booting..", "Connecting to WiFi...",
                                                 kColorWhite, kColorGray);
        return;
    }

    if (bootPhase_ == BootPhase::WifiConnected || bootPhase_ == BootPhase::BackendConnecting) {
        if (bootPhase_ == BootPhase::BackendConnecting) {
            NowPlayingScreen::renderTextCenteredPair(framebuffer, width, height, center_y,
                                                     "Connected to wifi!", "Connecting to Rufus...",
                                                     kColorWhite, kColorGray);
        } else {
            NowPlayingScreen::renderTextCentered(framebuffer, width, height, center_y,
                                                   "Connected to wifi!", kColorWhite);
        }
        return;
    }

    if (shouldShowNowPlaying()) {
        if (isMusicPlaying() && waveform_.bucket_count > 0) {
            WaveformView::render(
                    framebuffer, width, height, waveform_.buckets.data(), waveform_.bucket_count, kWaveformColor);
        }
        char title_line[64];
        char artist_line[64];
        NowPlayingScreen::truncateCopy(title_line, sizeof(title_line),
                                       nowPlaying_.title[0] ? nowPlaying_.title : "Unknown track");
        NowPlayingScreen::truncateCopy(artist_line, sizeof(artist_line),
                                       nowPlaying_.artist[0] ? nowPlaying_.artist : "Unknown artist");
        NowPlayingScreen::renderTextCenteredPair(framebuffer, width, height,
                                                 static_cast<uint16_t>(height - 36U),
                                                 title_line, artist_line, kColorWhite, kColorAccent);
        return;
    }

    if (isListeningForCommand() || stateEquals(sessionState_, "LISTENING")) {
        NowPlayingScreen::renderTextCentered(framebuffer, width, height, center_y,
                                             "I'm listening!", kColorAccent);
        return;
    }

    NowPlayingScreen::renderTextCenteredPair(framebuffer, width, height, center_y,
                                             "Waiting for voice", "or music", kColorWhite, kColorGray);
}

void UiApp::render() {
    if (uint16_t *fb = display_.framebuffer()) {
        if (!display_.beginDraw()) {
            return;
        }
        const uint16_t width = display_.framebufferWidth();
        const uint16_t height = display_.framebufferHeight();
        std::memset(fb, 0, static_cast<size_t>(width) * height * sizeof(uint16_t));
        renderScreen(fb, width, height);
        display_.endDraw();
        return;
    }

    display_.clear();
    display_.flush();
}

}  // namespace btspeaker
