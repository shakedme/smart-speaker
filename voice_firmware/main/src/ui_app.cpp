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
}  // namespace

UiApp::UiApp(DisplayBackend &display) : display_(display) {}

esp_err_t UiApp::init() {
    ESP_LOGI("UI", "[UI] screen=now_playing initialized");
    ESP_LOGI("UI", "[UI] waveform renderer initialized");
    return display_.init();
}

void UiApp::setConnecting() {
    connecting_ = true;
    render();
}

void UiApp::setIdle() {
    connecting_ = false;
    hasMetadata_ = false;
    std::strncpy(nowPlaying_.playback_state, "idle", sizeof(nowPlaying_.playback_state) - 1);
    render();
}

void UiApp::updateNowPlaying(const NowPlayingUiState &state) {
    connecting_ = false;
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
    std::strncpy(nowPlaying_.playback_state, state, sizeof(nowPlaying_.playback_state) - 1);
    nowPlaying_.playback_state[sizeof(nowPlaying_.playback_state) - 1] = '\0';
    render();
}

void UiApp::render() {
    if (uint16_t *fb = display_.framebuffer()) {
        if (!display_.beginDraw()) {
            return;
        }
        const uint16_t width = display_.framebufferWidth();
        const uint16_t height = display_.framebufferHeight();
        std::memset(fb, 0, static_cast<size_t>(width) * height * sizeof(uint16_t));
        if (waveform_.bucket_count > 0) {
            WaveformView::render(
                    fb, width, height, waveform_.buckets.data(), waveform_.bucket_count, kWaveformColor);
        }
        NowPlayingScreen::renderMetadata(fb, width, height, nowPlaying_, connecting_, hasMetadata_);
        display_.endDraw();
        return;
    }

    display_.clear();
    if (waveform_.bucket_count > 0) {
        display_.drawWaveform(waveform_.buckets.data(), waveform_.bucket_count);
    }
    if (connecting_) {
        display_.drawText(8, 150, "Connecting...");
    } else if (!hasMetadata_) {
        display_.drawText(8, 150, "No active playback");
        display_.drawText(8, 178, nowPlaying_.playback_state);
    } else {
        display_.drawText(8, 150, nowPlaying_.title);
        display_.drawText(8, 178, nowPlaying_.artist);
    }
    display_.flush();
}

}  // namespace btspeaker
