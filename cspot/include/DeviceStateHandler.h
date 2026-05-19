/**
 * TO DO
 * 
 * autoplay doesn't work for episodes
 * 
 */
#pragma once

#include <stdint.h>    // for uint8_t, uint32_t
#include <atomic>
#include <deque>       //for deque..
#include <functional>  //for function
#include <memory>      // for shared_ptr
#include <string>      // for string
#include <utility>     // for pair
#include <variant>     // for variant
#include <vector>      // for vector

#include "PlayerContext.h"  // for PlayerContext::resolveTracklist, jsonToTracklist...
#include "TrackPlayer.h"  // for TrackPlayer
#include "TrackQueue.h"
#include "TrackReference.h"
#include "protobuf/connect.pb.h"  // for PutStateRequest, DeviceState, PlayerState...

namespace cspot {
struct Context;
struct PlayerContext;

void notifyPlayHeadTrackReady();
void notifyPlayFirstAudio();

enum class PutStateMode {
    Full,
    Minimal,
};

class DeviceStateHandler {
    public:
        //Command Callback Structure
        enum CommandType {
            STOP,
            PLAY,
            PAUSE,
            DISC,
            DEPLETED,
            FLUSH,
            PLAYBACK_START,
            PLAYBACK,
            SKIP_NEXT,
            SKIP_PREV,
            SEEK,
            SET_SHUFFLE,
            SET_REPEAT,
            VOLUME,
            TRACK_INFO,
        };

        typedef std::variant<std::shared_ptr<cspot::QueuedTrack>, int32_t, bool>
                CommandData;

        struct Command {
            CommandType commandType;
            CommandData data;
        };

        typedef std::function<void(Command)> StateCallback;
        std::function<void()> onClose;

        DeviceStateHandler(std::shared_ptr<cspot::Context>, std::function<void()>);

        ~DeviceStateHandler();

        void disconnect();

        void putDeviceState(PutStateReason member_type =
        PutStateReason::PutStateReason_PLAYER_STATE_CHANGED);

        void setDeviceState(PutStateReason put_state_reason);

        void putPlayerState(PutStateReason member_type =
                                    PutStateReason::PutStateReason_PLAYER_STATE_CHANGED,
                            PutStateMode mode = PutStateMode::Full);

        void putPlayerState(PutStateReason member_type,
                            bool includeNextTracks,
                            bool refreshRestrictions);

        void handleConnectState();

        void sendCommand(CommandType, CommandData data = {});

        static void reloadTrackList(void *);

        void tryCompleteDeferredReload();

        void tryProcessDeferredContextResolve();

        void tryFlushDeferredPlayerState();

        void notifyHeadTrackReady();

        void notifyPlayFirstAudio();

        void topUpSkipPreload();

        Device device = Device_init_zero;

        std::vector<ProvidedTrack> currentTracks = {};
        StateCallback stateCallback;

        uint64_t started_playing_at = 0;
        uint32_t last_message_id = -1;
        uint8_t offset = 0;
        int64_t offsetFromStartInMillis = 0;

        bool is_active = false;
        bool reloadPreloadedTracks = true;
        bool deferredReloadTrackList = false;
        bool skipNextReloadStartup = false;
        bool skipLikePlayStartup = false;
        bool reloadHeadTrackReady = false;
        bool deferTopUpPreload = false;
        bool needsToBeSkipped = true;
        bool playerStateChanged = false;
        std::atomic<bool> pendingMinimalPlayerState{false};

        std::shared_ptr<cspot::TrackPlayer> trackPlayer;
        std::shared_ptr<cspot::TrackQueue> trackQueue;
        std::shared_ptr<cspot::Context> ctx;
        std::shared_ptr<PlayerContext> playerContext;


    private:

        std::pair<uint8_t *, std::vector<ProvidedTrack> *> queuePacket = {
                &offset, &currentTracks};
        std::vector<std::pair<std::string, std::string>> metadata_map = {};
        std::vector<std::pair<std::string, std::string>> context_metadata_map = {};
        std::mutex playerStateMutex;
        std::string context_uri, context_url;

        void parseCommand(const std::vector<uint8_t> &data);

        void handleActivePlayCommandFast(const std::vector<uint8_t> &rawData,
                                         const std::string &resolvedTrackUri,
                                         const std::string &skipUid);

        void skip(CommandType dir, bool notify);

        void unreference(char **string) {
            if (*string != NULL) {
                free(*string);
                *string = NULL;
            }
        }

        std::atomic<bool> resolvingContext = false;
        std::atomic<bool> deferContextResolveParse = false;
    };
}  // namespace cspot