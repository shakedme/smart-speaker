#include "AudioSink.h"

void AudioSink::pause() {}

void AudioSink::resume() {}

void AudioSink::flush() {}

size_t AudioSink::getBufferFillLevel() {
    return 0;
}

bool AudioSink::isPlaying() {
    return false;
}
