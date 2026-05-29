#pragma once

#include <stddef.h>  // for size_t
#include <stdint.h>  // for uint8_t
#include <atomic>    // for atomic
#include <functional>
#include <memory>    // for shared_ptr, unique_ptr
#include <mutex>     // for mutex

#include "AudioSink.h"           // for AudioSink
#include "BellTask.h"            // for Task
#include "DeviceStateHandler.h"  // cspot::DeviceStateHandler (CommandType, etc.)

class EspPlayer : public bell::Task {
public:
    static constexpr size_t kInitialPreBufferBytes = 1024 * 1024;
    static constexpr size_t kRecoveryPreBufferBytes = 1024 * 1024;
    static constexpr size_t kPauseLowWaterBytes = 256 * 1024;

    EspPlayer(std::shared_ptr<AudioSink> sink,
              std::shared_ptr<cspot::DeviceStateHandler> handler);

    ~EspPlayer();

    void disconnect();

    void resetPrebuffer();

    [[nodiscard]] bool isPlaybackPaused() const {
        return isPaused.load(std::memory_order_relaxed);
    }

    using TrackTransitionCallback = std::function<void(bool active)>;
    void setTrackTransitionCallback(TrackTransitionCallback callback);

private:
    static constexpr uint32_t kPositionSyncIntervalMs = 1000;

    void runTask() override;
    size_t feedData(uint8_t *data, size_t len, size_t trackId);
    void tryStartPlayback();
    void monitorPlaybackBuffer();
    void recordFeedDiagnostics(size_t requested,
                               size_t written,
                               uint32_t feedUs,
                               size_t fillBefore,
                               size_t fillAfter);
    void anchorTrackPlayback(size_t trackId);
    void processPendingTrackEnd();
    void notifyPlayerState();
    void sendPlayerStateHeartbeat();
    void notifyTrackTransition(bool active);

    std::shared_ptr<cspot::DeviceStateHandler> handler;
    TrackTransitionCallback trackTransitionCallback_;
    std::atomic<bool> trackTransitionActive_{false};
    std::shared_ptr<AudioSink> audioSink;
    std::deque<std::shared_ptr<cspot::QueuedTrack>> tracks = {};

    std::atomic<bool> isPaused = false;
    std::atomic<bool> playlistEnd = false;
    std::atomic<bool> isRunning = true;
    std::atomic<bool> isPreBuffered = false;
    bool needsInitialPrebuffer_ = true;
    std::atomic<bool> pendingTrackEnd = false;
    bool logFirstAudioPending_ = false;
    size_t lastTrackHash = 0;

    int64_t feedDiagWindowStartUs_ = 0;
    int64_t feedDiagLastCallUs_ = 0;
    uint32_t feedDiagCalls_ = 0;
    uint32_t feedDiagShortWrites_ = 0;
    uint32_t feedDiagZeroWrites_ = 0;
    uint32_t feedDiagMaxGapMs_ = 0;
    uint32_t feedDiagMaxFeedUs_ = 0;
    uint64_t feedDiagRequestedBytes_ = 0;
    uint64_t feedDiagWrittenBytes_ = 0;
    uint64_t feedDiagSumFeedUs_ = 0;
    size_t feedDiagMinFillBefore_ = SIZE_MAX;
    size_t feedDiagMinFillAfter_ = SIZE_MAX;
    size_t feedDiagMaxFillAfter_ = 0;

    std::mutex trackMutex;
};
