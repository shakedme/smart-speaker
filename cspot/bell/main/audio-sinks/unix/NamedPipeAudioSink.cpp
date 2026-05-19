#include "NamedPipeAudioSink.h"

#include <stdio.h>  // for printf

NamedPipeAudioSink::NamedPipeAudioSink() {
  printf("Start\n");
  this->namedPipeFile = std::ofstream("outputFifo", std::ios::binary);
  printf("stop\n");
}

NamedPipeAudioSink::~NamedPipeAudioSink() {
  this->namedPipeFile.close();
}

size_t NamedPipeAudioSink::feedPCMFrames(const uint8_t* buffer, size_t bytes) {
  this->namedPipeFile.write((char*)buffer, (long)bytes);
  this->namedPipeFile.flush();
  return bytes;
}
