#include "CdnUrlCache.h"

#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include "PlayPathProfiler.h"
#endif

namespace cspot {
namespace {

constexpr uint32_t kCacheTtlMs = 15 * 60 * 1000;
constexpr size_t kMaxEntries = 8;

struct Entry {
  std::string url;
  uint32_t cachedAtMs = 0;
};

std::mutex cacheMutex;
std::unordered_map<std::string, Entry> cache;

uint32_t nowMs() {
#ifdef ESP_PLATFORM
  return esp_log_timestamp();
#else
  return 0;
#endif
}

void evictExpiredLocked(uint32_t now) {
  for (auto it = cache.begin(); it != cache.end();) {
    if (now - it->second.cachedAtMs > kCacheTtlMs) {
      it = cache.erase(it);
    } else {
      ++it;
    }
  }
}

void trimLocked() {
  while (cache.size() > kMaxEntries) {
    auto oldest = cache.begin();
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (it->second.cachedAtMs < oldest->second.cachedAtMs) {
        oldest = it;
      }
    }
    cache.erase(oldest);
  }
}

}  // namespace

bool CdnUrlCache::lookup(const std::string& fileIdHex, std::string& urlOut) {
  if (fileIdHex.empty()) {
    return false;
  }

  const uint32_t now = nowMs();
  std::scoped_lock lock(cacheMutex);
  evictExpiredLocked(now);

  const auto found = cache.find(fileIdHex);
  if (found == cache.end()) {
    return false;
  }
  if (now - found->second.cachedAtMs > kCacheTtlMs) {
    cache.erase(found);
    return false;
  }

  urlOut = found->second.url;
#ifdef ESP_PLATFORM
  char detail[48];
  snprintf(detail, sizeof(detail), "file=%s", fileIdHex.c_str());
  PlayPathProfiler::mark("track.cdn_cache_hit", detail);
#endif
  return true;
}

void CdnUrlCache::store(const std::string& fileIdHex, const std::string& url) {
  if (fileIdHex.empty() || url.empty()) {
    return;
  }

  const uint32_t now = nowMs();
  std::scoped_lock lock(cacheMutex);
  evictExpiredLocked(now);
  cache[fileIdHex] = Entry{url, now};
  trimLocked();
}

void CdnUrlCache::clear() {
  std::scoped_lock lock(cacheMutex);
  cache.clear();
}

}  // namespace cspot
