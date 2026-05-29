#pragma once

#include "display_backend.hpp"
#include "ui_state.hpp"

namespace btspeaker {

class UiApp {
public:
    explicit UiApp(DisplayBackend &display);

    esp_err_t init();
    void setConnecting();
    void setIdle();
    void updateNowPlaying(const NowPlayingUiState &state);
    void updateWaveform(const WaveformUiState &state);
    void updateSessionState(const char *state);
    void render();

private:
    DisplayBackend &display_;
    NowPlayingUiState nowPlaying_{};
    WaveformUiState waveform_{};
    bool hasMetadata_ = false;
    bool connecting_ = true;
};

}  // namespace btspeaker
