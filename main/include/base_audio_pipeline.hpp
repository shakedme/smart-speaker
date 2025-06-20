//
// Created by Shaked Melman on 18/06/2025.
//

#ifndef BT_SPEAKER_BASEAUDIOPIPELINE_HPP
#define BT_SPEAKER_BASEAUDIOPIPELINE_HPP


#include <memory>
#include "audio_types.hpp"


namespace btspeaker {

    class BaseAudioPipeline {
    public:
        explicit BaseAudioPipeline(const char* tag);
        virtual ~BaseAudioPipeline() = default;

        // Must be implemented by derived classes
        virtual void setupPipeline() = 0;
        virtual void cleanupPipeline() = 0;

        // Optionally used by derived classes
        void pause();
        void resume();
        void start();
        void stop();

        [[nodiscard]] bool isRunning() const { return running; }

    protected:
        std::unique_ptr<I2SAudioPipeline> audioPipeline;
        bool running = false;
        const char* TAG;
    };

}


#endif //BT_SPEAKER_BASEAUDIOPIPELINE_HPP
