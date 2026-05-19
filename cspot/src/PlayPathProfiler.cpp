#include "PlayPathProfiler.h"

#include <cstdio>
#include <cstring>

#include "WrappedSemaphore.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif

namespace cspot {

uint32_t PlayPathProfiler::sessionStartMs_ = 0;
uint32_t PlayPathProfiler::lastMarkMs_ = 0;
uint32_t PlayPathProfiler::sessionId_ = 0;
bool PlayPathProfiler::active_ = false;

void PlayPathProfiler::resetSession() {
#ifdef ESP_PLATFORM
  if (active_) {
    const uint32_t elapsed = esp_log_timestamp() - sessionStartMs_;
    ESP_LOGI("PROF", "play.superseded session #%u at +%u ms",
             (unsigned) sessionId_, (unsigned) elapsed);
  }
  sessionStartMs_ = esp_log_timestamp();
  lastMarkMs_ = sessionStartMs_;
  sessionId_++;
  active_ = true;
  ESP_LOGI("PROF", "=== play session #%u start (t=%u ms) ===",
           (unsigned) sessionId_, (unsigned) sessionStartMs_);
#else
  active_ = true;
  sessionId_++;
#endif
}

void PlayPathProfiler::mark(const char* stage, const char* detail) {
  if (!active_ || stage == nullptr) {
    return;
  }

#ifdef ESP_PLATFORM
  const uint32_t now = esp_log_timestamp();
  const uint32_t totalMs = now - sessionStartMs_;
  const uint32_t deltaMs = now - lastMarkMs_;
  lastMarkMs_ = now;

  if (detail != nullptr && detail[0] != '\0') {
    ESP_LOGI("PROF", "[+%4u ms | total %5u ms] %s | %s",
             (unsigned) deltaMs, (unsigned) totalMs, stage, detail);
  } else {
    ESP_LOGI("PROF", "[+%4u ms | total %5u ms] %s",
             (unsigned) deltaMs, (unsigned) totalMs, stage);
  }
#else
  (void) stage;
  (void) detail;
#endif
}

bool PlayPathProfiler::sessionActive() {
  return active_;
}

uint32_t PlayPathProfiler::sessionElapsedMs() {
#ifdef ESP_PLATFORM
  if (!active_) {
    return 0;
  }
  return esp_log_timestamp() - sessionStartMs_;
#else
  return 0;
#endif
}

int PlayPathProfiler::drainPlayableSemaphore(
    const std::shared_ptr<bell::WrappedSemaphore>& semaphore) {
  if (!semaphore) {
    return 0;
  }

  int drained = 0;
  while (semaphore->twait(0) == 0) {
    drained++;
  }

  if (drained > 0 && active_) {
    char detail[32];
    snprintf(detail, sizeof(detail), "count=%d", drained);
    mark("semaphore.drained_stale", detail);
  }

  return drained;
}

const char* PlayPathProfiler::shortTrackRef(const char* uri) {
  if (uri == nullptr || uri[0] == '\0') {
    return "(none)";
  }

  const char* lastColon = strrchr(uri, ':');
  if (lastColon != nullptr && lastColon[1] != '\0') {
    return lastColon + 1;
  }

  return uri;
}

}  // namespace cspot
