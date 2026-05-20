#include "TrackQueue.h"
#include "cspot_task_config.h"
#include <pb_decode.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>

#include "AccessKeyFetcher.h"
#include "BellTask.h"
#include "BellUtils.h"  // for BELL_SLEEP_MS
#include "CDNAudioFile.h"
#include "CdnUrlCache.h"
#include "CSpotContext.h"
#include "DeviceStateHandler.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "PlayPathProfiler.h"
#include "Utils.h"
#include "WrappedSemaphore.h"
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif
#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif
#include "protobuf/metadata.pb.h"

using namespace cspot;

namespace {
std::string responsePreview(std::string_view body) {
  constexpr size_t kMaxPreviewLen = 240;
  return std::string(body.substr(0, std::min(body.size(), kMaxPreviewLen)));
}

std::string extractUrlFromResponseText(std::string_view body) {
  size_t pos = body.find("https://");
  if (pos == std::string_view::npos) {
    pos = body.find("http://");
  }

  if (pos == std::string_view::npos) {
    return {};
  }

  size_t end = pos;
  while (end < body.size()) {
    const char c = body[end];
    if (c == '"' || c == '\\' || c == ',' || c == '}' || c == ']') {
      break;
    }
    end++;
  }

  return std::string(body.substr(pos, end - pos));
}

#ifdef BELL_ONLY_CJSON
std::string extractUrlFromJsonValue(cJSON* item) {
  if (item == nullptr) {
    return {};
  }

  if (cJSON_IsString(item) && item->valuestring != nullptr &&
      strncmp(item->valuestring, "http", 4) == 0) {
    return std::string(item->valuestring);
  }

  if (cJSON_IsArray(item)) {
    const int size = cJSON_GetArraySize(item);
    for (int i = 0; i < size; i++) {
      auto url = extractUrlFromJsonValue(cJSON_GetArrayItem(item, i));
      if (!url.empty()) {
        return url;
      }
    }
    return {};
  }

  if (cJSON_IsObject(item)) {
    static const char* kPreferredKeys[] = {"cdnurl", "cdnurls", "url", "urls"};
    for (const char* key : kPreferredKeys) {
      auto url = extractUrlFromJsonValue(cJSON_GetObjectItem(item, key));
      if (!url.empty()) {
        return url;
      }
    }

    for (cJSON* child = item->child; child != nullptr; child = child->next) {
      auto url = extractUrlFromJsonValue(child);
      if (!url.empty()) {
        return url;
      }
    }
  }

  return {};
}
#else
std::string extractUrlFromJsonValue(const nlohmann::json& item) {
  if (item.is_string()) {
    auto value = item.get<std::string>();
    if (value.rfind("http", 0) == 0) {
      return value;
    }
    return {};
  }

  if (item.is_array()) {
    for (const auto& child : item) {
      auto url = extractUrlFromJsonValue(child);
      if (!url.empty()) {
        return url;
      }
    }
    return {};
  }

  if (item.is_object()) {
    static const char* kPreferredKeys[] = {"cdnurl", "cdnurls", "url", "urls"};
    for (const char* key : kPreferredKeys) {
      auto found = item.find(key);
      if (found != item.end()) {
        auto url = extractUrlFromJsonValue(*found);
        if (!url.empty()) {
          return url;
        }
      }
    }

    for (const auto& child : item.items()) {
      auto url = extractUrlFromJsonValue(child.value());
      if (!url.empty()) {
        return url;
      }
    }
  }

  return {};
}
#endif
}  // namespace

namespace TrackDataUtils {
bool countryListContains(char* countryList, const char* country) {
  uint16_t countryList_length = strlen(countryList);
  for (int x = 0; x < countryList_length; x += 2) {
    if (countryList[x] == country[0] && countryList[x + 1] == country[1]) {
      return true;
    }
  }
  return false;
}

bool doRestrictionsApply(Restriction* restrictions, int count,
                         const char* country) {
  for (int x = 0; x < count; x++) {
    if (restrictions[x].countries_allowed != nullptr) {
      return !countryListContains(restrictions[x].countries_allowed, country);
    }

    if (restrictions[x].countries_forbidden != nullptr) {
      return countryListContains(restrictions[x].countries_forbidden, country);
    }
  }

  return false;
}

bool canPlayTrack(Track& trackInfo, int altIndex, const char* country) {
  if (altIndex < 0) {

  } else {
    for (int x = 0; x < trackInfo.alternative[altIndex].restriction_count;
         x++) {
      if (trackInfo.alternative[altIndex].restriction[x].countries_allowed !=
          nullptr) {
        return countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_allowed,
            country);
      }

      if (trackInfo.alternative[altIndex].restriction[x].countries_forbidden !=
          nullptr) {
        return !countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_forbidden,
            country);
      }
    }
  }
  return true;
}
}  // namespace TrackDataUtils

void TrackInfo::loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = std::string(pbTrack->name);

  if (pbTrack->artist_count > 0) {
    // Handle artist data
    artist = std::string(pbTrack->artist[0].name);
  }

  if (pbTrack->has_album) {
    // Handle album data
    album = std::string(pbTrack->album.name);

    if (pbTrack->album.has_cover_group &&
        pbTrack->album.cover_group.image_count > 0) {
      auto imageId =
          pbArrayToVector(pbTrack->album.cover_group.image[0].file_id);
      imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
    }
  }

  number = pbTrack->has_number ? pbTrack->number : 0;
  discNumber = pbTrack->has_disc_number ? pbTrack->disc_number : 0;
  duration = pbTrack->duration;
}

void TrackInfo::loadPbEpisode(Episode* pbEpisode,
                              const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = std::string(pbEpisode->name);

  if (pbEpisode->covers->image_count > 0) {
    // Handle episode info
    auto imageId = pbArrayToVector(pbEpisode->covers->image[0].file_id);
    imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
  }

  number = pbEpisode->has_number ? pbEpisode->number : 0;
  discNumber = 0;
  duration = pbEpisode->duration;
}

QueuedTrack::QueuedTrack(
    ProvidedTrack& ref, std::shared_ptr<cspot::Context> ctx,
    std::shared_ptr<bell::WrappedSemaphore> playableSemaphore,
    int64_t requestedPosition)
    : requestedPosition((uint32_t)requestedPosition), ctx(ctx) {
  trackMetrics = std::make_shared<TrackMetrics>(ctx, requestedPosition);
  this->playableSemaphore = playableSemaphore;
  this->ref = ref;
  this->audioFormat = ctx->config.audioFormat;
  if (!strstr(ref.uri, "spotify:delimiter")) {
    this->gid = base62Decode(ref.uri);
    state = State::QUEUED;
    PlayPathProfiler::mark("track.queued",
                             PlayPathProfiler::shortTrackRef(ref.uri));
  } else {
    state = State::FAILED;
    playableSemaphore->give();
  }
}

QueuedTrack::~QueuedTrack() {
  //if (state < State::READY)
  //  playableSemaphore->give();
  state = State::FAILED;

  if (pendingMercuryRequest != 0) {
    ctx->session->unregister(pendingMercuryRequest);
  }

  if (pendingAudioKeyRequest != 0) {
    ctx->session->unregisterAudioKey(pendingAudioKeyRequest);
  }
  pb_release(Track_fields, &pbTrack);
  pb_release(Episode_fields, &pbEpisode);
}

std::shared_ptr<cspot::CDNAudioFile> QueuedTrack::getAudioFile() {
  if (state != State::READY) {
    return nullptr;
  }

  if (warmedAudioFile) {
    return warmedAudioFile;
  }

  return std::make_shared<cspot::CDNAudioFile>(cdnUrl, audioKey);
}

void QueuedTrack::warmCdnStream() {
  std::lock_guard<std::mutex> lock(warmMutex_);
  if (state != State::READY || streamOpened || cdnUrl.empty()) {
    return;
  }

  try {
    warmedAudioFile = std::make_shared<cspot::CDNAudioFile>(cdnUrl, audioKey);
    PlayPathProfiler::mark("track.cdn_warm_begin",
                           PlayPathProfiler::shortTrackRef(ref.uri));
#ifndef CONFIG_BELL_NOCODEC
    warmedAudioFile->openStream();
#else
    ssize_t headerSize = 0;
    if (warmedAudioFile->openStream(headerSize) == nullptr) {
      warmedAudioFile.reset();
      streamOpened = false;
      ESP_LOGW("TrackQueue", "CDN warm failed for %s",
               PlayPathProfiler::shortTrackRef(ref.uri));
      return;
    }
#endif
    streamOpened = warmedAudioFile->isStreamOpen();
    if (!streamOpened) {
      warmedAudioFile.reset();
      ESP_LOGW("TrackQueue", "CDN warm failed for %s",
               PlayPathProfiler::shortTrackRef(ref.uri));
      return;
    }
    PlayPathProfiler::mark("track.cdn_warm_done",
                           PlayPathProfiler::shortTrackRef(ref.uri));
  } catch (const std::exception& ex) {
    warmedAudioFile.reset();
    streamOpened = false;
    ESP_LOGW("TrackQueue", "CDN warm exception for %s: %s",
             PlayPathProfiler::shortTrackRef(ref.uri), ex.what());
  } catch (...) {
    warmedAudioFile.reset();
    streamOpened = false;
    ESP_LOGW("TrackQueue", "CDN warm unknown exception for %s",
             PlayPathProfiler::shortTrackRef(ref.uri));
  }
}

void QueuedTrack::stepParseMetadata(Track* pbTrack, Episode* pbEpisode) {
  int alternativeCount, filesCount = 0;
  bool canPlay = false;
  AudioFile* selectedFiles = nullptr;

  const char* countryCode = ctx->session->getCountryCode().c_str();

  if (gid.first == SpotifyFileType::TRACK) {
    ESP_LOGI("TrackQueue", "Track name: %s", pbTrack->name);
    PlayPathProfiler::mark("track.metadata_done",
                             PlayPathProfiler::shortTrackRef(ref.uri));
//    ESP_LOGI("TrackQueue", "Track duration: %d", pbTrack->duration);

    // Check if we can play the track, if not, try alternatives
    if (TrackDataUtils::doRestrictionsApply(
            pbTrack->restriction, pbTrack->restriction_count, countryCode)) {
      // Go through alternatives
      for (int x = 0; x < pbTrack->alternative_count; x++) {
        if (!TrackDataUtils::doRestrictionsApply(
                pbTrack->alternative[x].restriction,
                pbTrack->alternative[x].restriction_count, countryCode)) {
          selectedFiles = pbTrack->alternative[x].file;
          filesCount = pbTrack->alternative[x].file_count;
          trackId = pbArrayToVector(pbTrack->alternative[x].gid);
          break;
        }
      }
    } else {
      // We can play the track
      selectedFiles = pbTrack->file;
      filesCount = pbTrack->file_count;
      trackId = pbArrayToVector(pbTrack->gid);
    }

    if (trackId.size() > 0) {
      // Load track information
      trackInfo.loadPbTrack(pbTrack, trackId);
    }
  } else {
    // Handle episodes
//    ESP_LOGI("TrackQueue", "Episode name: %s", pbEpisode->name);
//    ESP_LOGI("TrackQueue", "Episode duration: %d", pbEpisode->duration);

    // Check if we can play the episode
    if (!TrackDataUtils::doRestrictionsApply(pbEpisode->restriction,
                                             pbEpisode->restriction_count,
                                             countryCode)) {
      selectedFiles = pbEpisode->file;
      filesCount = pbEpisode->file_count;
      trackId = pbArrayToVector(pbEpisode->gid);

      // Load track information
      trackInfo.loadPbEpisode(pbEpisode, trackId);
    }
  }

  // Find playable file
  for (int x = 0; x < filesCount; x++) {
    CSPOT_LOG(debug, "File format: %d", selectedFiles[x].format);
    if (selectedFiles[x].format == audioFormat) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      break;  // If file found stop searching
    }

    // Fallback to OGG Vorbis 96kbps
    if (fileId.size() == 0 &&
        selectedFiles[x].format == AudioFormat_OGG_VORBIS_96) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      ESP_LOGD("TrackQueue", "Falling back to OGG Vorbis 96kbps");
    }
  }

  // No viable files found for playback
  if (fileId.size() == 0) {
    ESP_LOGE("TrackQueue", "File not available for playback");

    // no alternatives for song
    state = State::FAILED;
    playableSemaphore->give();
    return;
  }
  identifier = bytesToHexString(fileId);
  state = State::KEY_REQUIRED;
}

void QueuedTrack::stepLoadAudioFile(
    std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {
  PlayPathProfiler::mark("track.audio_key_request",
                         PlayPathProfiler::shortTrackRef(ref.uri));
  // Request audio key
  this->pendingAudioKeyRequest = ctx->session->requestAudioKey(
      trackId, fileId,
      [this, &trackListMutex, updateSemaphore](
          bool success, const std::vector<uint8_t>& audioKey) {
        std::scoped_lock lock(trackListMutex);

        if (success) {
          ESP_LOGD("TrackQueue", "Got audio key");
          PlayPathProfiler::mark("track.audio_key_done",
                                 PlayPathProfiler::shortTrackRef(this->ref.uri));
          this->audioKey =
              std::vector<uint8_t>(audioKey.begin() + 4, audioKey.end());

          state = State::CDN_REQUIRED;
          updateSemaphore->give();
        } else {
          ESP_LOGE("TrackQueue", "Failed to get audio key");
          retries++;
          state = State::KEY_REQUIRED;
          if (retries > 10) {
            if (audioFormat > AudioFormat_OGG_VORBIS_96) {
              audioFormat = (AudioFormat)(audioFormat - 1);
              state = State::QUEUED;
            } else {
              state = State::FAILED;
              playableSemaphore->give();
            }
          }
        }
      });

  state = State::PENDING_KEY;
}

void QueuedTrack::stepLoadCDNUrl(const std::string& accessKey) {
  if (accessKey.size() == 0) {
    ESP_LOGD("TrackQueue", "Waiting for access key...");
    PlayPathProfiler::mark("track.cdn_waiting_access_key",
                           PlayPathProfiler::shortTrackRef(ref.uri));
    return;
  }

  // Request CDN URL with retry logic
  PlayPathProfiler::mark("track.cdn_request",
                         PlayPathProfiler::shortTrackRef(ref.uri));
  ESP_LOGD("TrackQueue", "Received access key, fetching CDN URL... (attempt %d/5)", retries + 1);

  // Check retry limit
  if (retries >= 5) {
    ESP_LOGE("TrackQueue", "Max retries reached for CDN URL, failing track: %s", 
             trackInfo.name.c_str());
    state = State::FAILED;
    playableSemaphore->give();
    return;
  }

  try {
    const std::string fileIdHex = bytesToHexString(fileId);
    std::string lastError;
    if (CdnUrlCache::lookup(fileIdHex, cdnUrl)) {
      state = State::READY;
      retries = 0;
      ESP_LOGD("TrackQueue", "CDN URL cache hit for file %s", fileIdHex.c_str());
    } else {
      const std::vector<std::string> requestUrls = {
        string_format(
            "https://spclient.wg.spotify.com/storage-resolve/files/audio/"
            "interactive/%s?product=0&alt=json",
            fileIdHex.c_str()),
        string_format(
            "https://api.spotify.com/v1/storage-resolve/files/audio/"
            "interactive/%s?alt=json",
            fileIdHex.c_str()),
      };

      for (const auto& requestUrl : requestUrls) {
      ESP_LOGD("TrackQueue", "Requesting CDN URL from: %s", requestUrl.c_str());

      auto req = bell::HTTPClient::get(
          requestUrl,
          {bell::HTTPClient::ValueHeader({"Authorization", "Bearer " + accessKey}),
           bell::HTTPClient::ValueHeader({"Connection", "close"}),
           bell::HTTPClient::ValueHeader({"User-Agent", "Spotify/8.8.0"})},
          10000);

      if (!req) {
        lastError = "HTTP request failed - null response";
        continue;
      }

      std::string_view result = req->body();
      if (result.empty()) {
        lastError = "Empty response from Spotify API";
        continue;
      }

      ESP_LOGD("TrackQueue", "CDN response received, size: %d", result.size());
      cdnUrl = extractUrlFromResponseText(result);
#ifdef BELL_ONLY_CJSON
      if (cdnUrl.empty()) {
        cJSON* jsonResult = cJSON_Parse(result.data());
        if (!jsonResult) {
          lastError = "Failed to parse JSON response";
          continue;
        }

        cdnUrl = extractUrlFromJsonValue(jsonResult);
        cJSON_Delete(jsonResult);
      }
#else
      if (cdnUrl.empty()) {
        lastError = "No CDN URL found in storage-resolve response: " +
                    responsePreview(result);
        ESP_LOGW("TrackQueue", "%s", lastError.c_str());
        continue;
      }
#endif
      if (!cdnUrl.empty()) {
        break;
      }

      lastError = "No CDN URL found in storage-resolve response: " +
                  responsePreview(result);
      ESP_LOGW("TrackQueue", "%s", lastError.c_str());
      }
    }

    if (cdnUrl.empty()) {
      throw std::runtime_error(lastError.empty() ? std::string("Empty CDN URL received")
                                                 : lastError);
    }

    state = State::READY;
    retries = 0; // Reset retry count on success
    CdnUrlCache::store(bytesToHexString(fileId), cdnUrl);
    PlayPathProfiler::mark("track.cdn_done",
                           PlayPathProfiler::shortTrackRef(ref.uri));
    
  } catch (const std::exception& e) {
    const char* errMsg = e.what();
    ESP_LOGW("TrackQueue", "Network error fetching CDN URL (attempt %d/5): %s",
             retries + 1, (errMsg != nullptr && errMsg[0] != '\0') ? errMsg : "unknown error");
    retries++;
    
    // Record retry time for exponential backoff
    lastRetryTime = esp_log_timestamp();
    
    // Exponential backoff: 1s, 2s, 4s, 8s, 16s
    int backoffMs = 1000 * (1 << (retries - 1));

    // Don't block the TrackQueue task - set state to retry and let the main loop handle it
    state = State::CDN_REQUIRED;
    
    // Schedule retry (don't call playableSemaphore->give() yet)
    return;
    
  } catch (...) {
    ESP_LOGW("TrackQueue", "Unknown error fetching CDN URL (attempt %d/5)", retries + 1);
    retries++;
    
    // Record retry time for exponential backoff
    lastRetryTime = esp_log_timestamp();
    
    // Exponential backoff for unknown errors too
    int backoffMs = 1000 * (1 << (retries - 1));

    state = State::CDN_REQUIRED;
    return;
  }
  
  ESP_LOGD("TrackQueue", "Signaling playableSemaphore (track state: %s)",
           state == State::READY ? "READY" : "FAILED");
  if (state == State::READY) {
    char detail[64];
    snprintf(detail, sizeof(detail), "track=%s",
             PlayPathProfiler::shortTrackRef(ref.uri));
    PlayPathProfiler::mark("track.ready_signal", detail);
    notifyPlayHeadTrackReady();
  }
  playableSemaphore->give();
}

void QueuedTrack::stepLoadMetadata(
    Track* pbTrack, Episode* pbEpisode, std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {
  // Prepare request ID
  std::string requestUrl =
      string_format("hm://metadata/3/%s/%s",
                    gid.first == SpotifyFileType::TRACK ? "track" : "episode",
                    bytesToHexString(gid.second).c_str());
  PlayPathProfiler::mark("track.metadata_request",
                         PlayPathProfiler::shortTrackRef(ref.uri));

  auto responseHandler = [this, pbTrack, pbEpisode, &trackListMutex,
                          updateSemaphore](const MercurySession::Response &res) {
    std::scoped_lock lock(trackListMutex);

    if (res.parts.size() == 0) {
      ESP_LOGE("TrackQueue", "Invalid Metadata");
      // Invalid metadata, cannot proceed
      state = State::FAILED;
      updateSemaphore->give();
      playableSemaphore->give();
      return;
    }

    // Parse the metadata
    if (gid.first == SpotifyFileType::TRACK) {
      pb_release(Track_fields, pbTrack);
      pbDecode(*pbTrack, Track_fields, const_cast<std::vector<uint8_t>&>(res.parts[0]));
    } else {
      pb_release(Episode_fields, pbEpisode);
      pbDecode(*pbEpisode, Episode_fields, const_cast<std::vector<uint8_t>&>(res.parts[0]));
    }

    // Parse received metadata
    stepParseMetadata(pbTrack, pbEpisode);

    updateSemaphore->give();
  };
  // Execute the request
  if (pbTrack != NULL || pbEpisode != NULL)
    pendingMercuryRequest = ctx->session->execute(
        MercurySession::RequestType::GET, requestUrl, responseHandler);

  // Set the state to pending
  state = State::PENDING_META;
}

TrackQueue::TrackQueue(std::shared_ptr<cspot::Context> ctx)
    : bell::Task("CSpotTrackQueue", 1024 * 48, 2, CSPOT_TRACK_QUEUE_CORE), ctx(ctx) {
  accessKeyFetcher = std::make_shared<cspot::AccessKeyFetcher>(ctx);
  processSemaphore = std::make_shared<bell::WrappedSemaphore>();
  playableSemaphore = std::make_shared<bell::WrappedSemaphore>();

  // Start the task
  startTask();
};

TrackQueue::~TrackQueue() {
  stopTask();

  std::scoped_lock lock(tracksMutex);
}

void TrackQueue::runTask() {
  isRunning = true;

  std::scoped_lock lock(runningMutex);

  std::deque<std::shared_ptr<QueuedTrack>> trackQueue;

  while (isRunning) {
    if (processSemaphore->twait(200)) {
      if (!preloadedTracks.size())
        continue;
    }

    // Make sure we have the newest access key
    accessKey = accessKeyFetcher->getAccessKey();
    {
      std::scoped_lock lock(tracksMutex);

      trackQueue = preloadedTracks;
    }

    for (auto& track : trackQueue) {
      if (track) {
        if (headOnlyPreloadMode.load()) {
          if (trackQueue.empty() || track != trackQueue.front()) {
            continue;
          }
        }
        if (this->processTrack(track))
          break;
        BELL_SLEEP_MS(1);
      }
    }
    BELL_SLEEP_MS(1);
  }
}

void TrackQueue::stopTask() {
  if (isRunning) {
    isRunning = false;
    processSemaphore->give();
    std::scoped_lock lock(runningMutex);
  }
}

std::shared_ptr<QueuedTrack> TrackQueue::consumeTrack(
    std::shared_ptr<QueuedTrack> prevTrack, int& offset) {
  std::scoped_lock lock(tracksMutex);

  ESP_LOGD("TrackQueue", "consumeTrack called: preloadedTracks.size()=%d", preloadedTracks.size());
  
  if (!preloadedTracks.size()) {
    ESP_LOGE("TrackQueue", "No preloaded tracks available");
    offset = -1;
    return nullptr;
  }

  auto prevTrackIter =
      std::find(preloadedTracks.begin(), preloadedTracks.end(), prevTrack);

  if (prevTrackIter != preloadedTracks.end()) {
    // Get offset of next track
    offset = prevTrackIter - preloadedTracks.begin() + 1;
  } else {
    offset = 0;
  }
  if (offset >= preloadedTracks.size()) {
    // Last track in preloaded queue
    return nullptr;
  }
  return preloadedTracks[offset];
}

bool TrackQueue::processTrack(std::shared_ptr<QueuedTrack> track) {
  switch (track->state) {
    case QueuedTrack::State::QUEUED:
      track->stepLoadMetadata(&track->pbTrack, &track->pbEpisode, tracksMutex,
                              processSemaphore);
      break;
    case QueuedTrack::State::KEY_REQUIRED:
      track->stepLoadAudioFile(tracksMutex, processSemaphore);
      break;
    case QueuedTrack::State::CDN_REQUIRED: {
      // Check if we need to wait for exponential backoff
      if (track->retries > 0 && track->lastRetryTime > 0) {
        uint64_t currentTime = esp_log_timestamp();
        uint64_t timeSinceLastRetry = currentTime - track->lastRetryTime;
        
        // Calculate required backoff time: 1s, 2s, 4s, 8s, 16s
        uint64_t backoffMs = 1000 * (1 << (track->retries - 1));
        
        if (timeSinceLastRetry < backoffMs) {
          // Not enough time has passed, don't retry yet
          ESP_LOGD("TrackQueue", "CDN retry backoff: waiting %llu more ms", 
                   backoffMs - timeSinceLastRetry);
          return false;
        }
      }
      
      track->stepLoadCDNUrl(accessKey);
      break;
    }
    default:
      return false;
      // Do not perform any action
      break;
  }
  return true;
}
