#pragma once

#include "display_backend.hpp"
#include "ui_state.hpp"

namespace btspeaker {

class UiApp {
public:
    explicit UiApp(DisplayBackend &display);

    esp_err_t init();
    void setBooting();
    void setWifiConnecting();
    void setWifiConnected();
    void setBackendConnecting();
    void setIdle();
    void updateNowPlaying(const NowPlayingUiState &state);
    void updateWaveform(const WaveformUiState &state);
    void updateSessionState(const char *state);
    void render();

private:
    enum class BootPhase {
        Booting,
        WifiConnecting,
        WifiConnected,
        BackendConnecting,
        Ready,
    };

    [[nodiscard]] bool shouldShowNowPlaying() const;
    [[nodiscard]] bool isMusicPlaying() const;
    [[nodiscard]] bool isListeningForCommand() const;
    void renderScreen(uint16_t *framebuffer, uint16_t width, uint16_t height);

    DisplayBackend &display_;
    NowPlayingUiState nowPlaying_{};
    WaveformUiState waveform_{};
    char sessionState_[32] = "LISTENING";
    BootPhase bootPhase_ = BootPhase::Booting;
    bool hasMetadata_ = false;
};

}  // namespace btspeaker
