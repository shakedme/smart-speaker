#pragma once

#include <cstdint>
#include <memory>

namespace bell {
class WrappedSemaphore;
}

namespace cspot {

class PlayPathProfiler {
 public:
  static void resetSession();
  static void mark(const char* stage, const char* detail = nullptr);
  static bool sessionActive();
  static uint32_t sessionElapsedMs();
  static int drainPlayableSemaphore(
      const std::shared_ptr<bell::WrappedSemaphore>& semaphore);
  static const char* shortTrackRef(const char* uri);

 private:
  static uint32_t sessionStartMs_;
  static uint32_t lastMarkMs_;
  static uint32_t sessionId_;
  static bool active_;
};

}  // namespace cspot
