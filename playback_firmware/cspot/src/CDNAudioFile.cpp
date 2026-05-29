#include "CDNAudioFile.h"

#include <string.h>          // for memcpy
#include <algorithm>         // for std::min
#include <functional>        // for __base
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <stdexcept>         // for runtime_error
#include <string_view>       // for string_view
#include <type_traits>       // for remove_extent_t

#include "AccessKeyFetcher.h"  // for AccessKeyFetcher
#include "BellLogger.h"        // for AbstractLogger
#include "Crypto.h"
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "PlayPathProfiler.h"
#include "SocketStream.h"      // for SocketStream
#include "Utils.h"             // for bigNumAdd, bytesToHexString, string...
#include "WrappedSemaphore.h"  // for WrappedSemaphore
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

CDNAudioFile::CDNAudioFile(const std::string& cdnUrl,
                           const std::vector<uint8_t>& audioKey)
    : cdnUrl(cdnUrl), audioKey(audioKey) {
  this->crypto = std::make_unique<Crypto>();
}

CDNAudioFile::~CDNAudioFile() = default;

size_t CDNAudioFile::getPosition() {
  return this->position;
}

void CDNAudioFile::seek(size_t newPos) {
  this->enableRequestMargin = true;
  this->position = newPos;
}

CDNAudioFile::Diagnostics CDNAudioFile::consumeDiagnostics() {
  std::lock_guard<std::mutex> lock(diagnosticsMutex);
  Diagnostics snapshot = diagnostics;
  diagnostics = {};
  return snapshot;
}

void CDNAudioFile::decryptWith(Crypto& cryptoInstance,
                               uint8_t* dst,
                               size_t nbytes,
                               size_t pos) {
  if (audioKey.size() != 16 && audioKey.size() != 24 && audioKey.size() != 32) {
    throw std::runtime_error("Invalid AES key length");
  }
  auto calculatedIV = bigNumAdd(audioAESIV, pos / 16);
  cryptoInstance.aesCTRXcrypt(this->audioKey, calculatedIV, dst, nbytes);
}

bool CDNAudioFile::fetchRangeInto(std::unique_ptr<bell::HTTPClient::Response>& connection,
                                  Crypto& cryptoInstance,
                                  uint8_t* dst,
                                  size_t requestPosition,
                                  size_t requestCapacity,
                                  size_t& bytesRead) {
  bytesRead = 0;
  if (requestPosition >= totalFileSize + SPOTIFY_OPUS_HEADER || requestCapacity == 0) {
    return false;
  }

  const size_t maxEnd = totalFileSize + SPOTIFY_OPUS_HEADER - 1;
  const size_t endPosition = std::min(requestPosition + requestCapacity - 1, maxEnd);
  const size_t expectedBytes = endPosition - requestPosition + 1;

  {
    std::lock_guard<std::mutex> lock(diagnosticsMutex);
    diagnostics.rangeRequests++;
  }

  if (!connection) {
    connection = bell::HTTPClient::get(
        this->cdnUrl,
        {bell::HTTPClient::RangeHeader::range(
            static_cast<int32_t>(requestPosition),
            static_cast<int32_t>(endPosition))},
        20);
    if (!connection || !connection->stream().isOpen()) {
      std::lock_guard<std::mutex> lock(diagnosticsMutex);
      diagnostics.rangeFailures++;
      return false;
    }
  } else if (!connection->get(
                 cdnUrl,
                 {bell::HTTPClient::RangeHeader::range(
                     static_cast<int32_t>(requestPosition),
                     static_cast<int32_t>(endPosition))})) {
    std::lock_guard<std::mutex> lock(diagnosticsMutex);
    diagnostics.rangeFailures++;
    return false;
  }

  auto& stream = connection->stream();
  stream.read((char*)dst, expectedBytes);
  if (stream.fail() || static_cast<size_t>(stream.gcount()) != expectedBytes) {
    std::lock_guard<std::mutex> lock(diagnosticsMutex);
    diagnostics.rangeFailures++;
    return false;
  }

  bytesRead = expectedBytes;
  decryptWith(cryptoInstance, dst, bytesRead, requestPosition);
  return true;
}

#ifndef CONFIG_BELL_NOCODEC
/**
  * @brief Opens connection to the provided cdn url, and fetches track metadata.
  */
void CDNAudioFile::openStream() {
  if (streamOpen_) {
    return;
  }
  PlayPathProfiler::mark("cdn.open_stream_begin");
  for (int attempt = 0; attempt < 3 && !streamOpen_; ++attempt) {
    if (openStreamAttempt()) {
      break;
    }
    httpConnection.reset();
  }
  if (!streamOpen_) {
    PlayPathProfiler::mark("cdn.open_stream_failed");
    CSPOT_LOG(error, "Failed to open CDN stream after retries");
  }
}

bool CDNAudioFile::openStreamAttempt() {
  CSPOT_LOG(info, "Opening HTTP stream to %s", this->cdnUrl.c_str());

  auto connection = bell::HTTPClient::get(
      this->cdnUrl,
      {bell::HTTPClient::RangeHeader::range(0, OPUS_HEADER_SIZE - 1)}, 20);
  if (!connection || !connection->stream().isOpen()) {
    return false;
  }
  httpConnection.reset();
  httpConnection = std::move(connection);

  auto& stream = httpConnection->stream();
  stream.read((char*)header.data(), OPUS_HEADER_SIZE);
  if (stream.fail() || stream.gcount() != OPUS_HEADER_SIZE) {
    return false;
  }

  this->totalFileSize =
      this->httpConnection->totalLength() - SPOTIFY_OPUS_HEADER;

  this->decrypt(header.data(), OPUS_HEADER_SIZE, 0);

  size_t footerStartLocation =
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) -
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) % 16;

  this->footer = std::vector<uint8_t>(
      this->totalFileSize - footerStartLocation + SPOTIFY_OPUS_HEADER);
  if (!this->httpConnection->get(
          cdnUrl, {bell::HTTPClient::RangeHeader::last(footer.size())})) {
    return false;
  }

  stream.read((char*)footer.data(), this->footer.size());
  if (stream.fail() ||
      static_cast<size_t>(stream.gcount()) != this->footer.size()) {
    return false;
  }

  this->decrypt(footer.data(), footer.size(), footerStartLocation);
  CSPOT_LOG(info, "Header and footer bytes received");
  PlayPathProfiler::mark("cdn.open_stream_done");
  streamOpen_ = true;
  this->position = 0;
  this->lastRequestPosition = 0;
  this->lastRequestCapacity = 0;
  return true;
}

size_t CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  if (bytes == 0 || position >= this->totalFileSize) {
    return 0;
  }
  if (position + bytes > this->totalFileSize) {
    bytes = this->totalFileSize - position;
  }

  size_t offsetPosition = position + SPOTIFY_OPUS_HEADER;
  size_t actualFileSize = this->totalFileSize + SPOTIFY_OPUS_HEADER;

  if (offsetPosition < OPUS_HEADER_SIZE &&
      bytes + offsetPosition <= OPUS_HEADER_SIZE) {
    memcpy(dst, this->header.data() + offsetPosition, bytes);
    position += bytes;
    return bytes;
  }

  if (offsetPosition >= (actualFileSize - this->footer.size())) {
    size_t toReadBytes = bytes;

    if ((position + bytes) > this->totalFileSize) {
      toReadBytes = this->totalFileSize - position;
    }

    size_t footerOffset =
        offsetPosition - (actualFileSize - this->footer.size());
    memcpy(dst, this->footer.data() + footerOffset, toReadBytes);

    position += toReadBytes;
    return toReadBytes;
  }

  if (offsetPosition >= this->lastRequestPosition &&
      offsetPosition < this->lastRequestPosition + this->lastRequestCapacity) {
    size_t toRead = bytes;

    if ((toRead + offsetPosition) >
        this->lastRequestPosition + lastRequestCapacity) {
      toRead = this->lastRequestPosition + lastRequestCapacity - offsetPosition;
    }

    memcpy(dst, this->httpBuffer.data() + offsetPosition - lastRequestPosition,
           toRead);
    position += toRead;

    return toRead;
  }

  size_t requestPosition = (offsetPosition) - ((offsetPosition) % 16);
  if (this->enableRequestMargin && requestPosition > SEEK_MARGIN_SIZE) {
    requestPosition = (offsetPosition - SEEK_MARGIN_SIZE) -
                      ((offsetPosition - SEEK_MARGIN_SIZE) % 16);
    this->enableRequestMargin = false;
  }

  if (httpBuffer.size() < HTTP_BUFFER_SIZE) {
    httpBuffer.resize(HTTP_BUFFER_SIZE);
  }
  size_t bytesRead = 0;
  if (!fetchRangeInto(httpConnection,
                      *crypto,
                      httpBuffer.data(),
                      requestPosition,
                      HTTP_BUFFER_SIZE,
                      bytesRead)) {
    return 0;
  }
  this->lastRequestPosition = requestPosition;
  this->lastRequestCapacity = bytesRead;

  return readBytes(dst, bytes);
}
#else
/**
 * @brief Opens a connection to the CDN URL and fills the first buffer with track header data.
 *
 * @param header_size Reference to a size_t variable where the size of the header is stored.
 * @return Pointer to the beginning of the HTTP buffer where the track header data is stored.
 */
uint8_t* CDNAudioFile::openStream(ssize_t& header_size) {
  if (streamOpen_) {
    header_size = static_cast<ssize_t>(this->position);
    return &httpBuffer[0];
  }
  PlayPathProfiler::mark("cdn.open_stream_begin");

  for (int attempt = 0; attempt < 3 && !streamOpen_; ++attempt) {
    auto connection = bell::HTTPClient::get(
        this->cdnUrl,
        {bell::HTTPClient::RangeHeader::range(0, HTTP_BUFFER_SIZE - 1)}, 20);
    if (!connection || !connection->stream().isOpen()) {
      continue;
    }
    httpConnection.reset();
    httpConnection = std::move(connection);
    auto& stream = httpConnection->stream();
    this->lastRequestPosition = 0;
    this->lastRequestCapacity = this->httpConnection->contentLength();
    this->totalFileSize = this->httpConnection->totalLength();

    stream.read((char*)this->httpBuffer.data(), lastRequestCapacity);
    if (stream.fail() ||
        static_cast<size_t>(stream.gcount()) != lastRequestCapacity) {
      continue;
    }
    this->decrypt(this->httpBuffer.data(), lastRequestCapacity,
                  this->lastRequestPosition);
    this->position = getHeader();
    header_size = this->position;
    PlayPathProfiler::mark("cdn.open_stream_done");
    streamOpen_ = true;
    return &httpBuffer[0];
  }

  PlayPathProfiler::mark("cdn.open_stream_failed");
  header_size = -1;
  return nullptr;
}

/**
 * @brief Finds the position of the first audio frame in the HTTP response.
 *
 * The OGG Vorbis file starts with three headers. They contain valuable information
 * for decoding the audio. 
 *
 * @return The position of the first audio frame in the HTTP response.
 */
long CDNAudioFile::getHeader() {
  uint32_t offset = SPOTIFY_OPUS_HEADER;

  for (int i = 0; i < 3; ++i) {
    offset += 26;
    if (offset >= HTTP_BUFFER_SIZE) {
      return HTTP_BUFFER_SIZE;
    }
    uint8_t segmentCount = httpBuffer[offset];
    uint32_t segmentEnd = segmentCount + offset + 1;
    ++offset;

    for (uint32_t j = offset; j < segmentEnd; ++j) {
      if (offset >= HTTP_BUFFER_SIZE) {
        return HTTP_BUFFER_SIZE;
      }
      offset += httpBuffer[j];
    }

    if (offset >= HTTP_BUFFER_SIZE) {
      return HTTP_BUFFER_SIZE;
    }
    offset += segmentCount;
  }

  return offset;
}

long CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  if (position + bytes >= this->totalFileSize) {
    if (position >= this->totalFileSize - 1) {
      return 0;
    } else {
      CSPOT_LOG(info, "Truncating read to %d bytes",
                this->totalFileSize - position);
      bytes = this->totalFileSize - position;
    }
  }

  if (position >= this->lastRequestPosition &&
      position < this->lastRequestPosition + this->lastRequestCapacity) {
    size_t toRead = bytes;

    if ((toRead + position) > this->lastRequestPosition + lastRequestCapacity) {
      toRead = this->lastRequestPosition + lastRequestCapacity - position;
    }

    memcpy(dst, this->httpBuffer.data() + position - lastRequestPosition,
           toRead);
    position += toRead;

    return toRead;
  } else {
    size_t requestPosition = (position) - ((position) % 16);
    if (this->enableRequestMargin && requestPosition > SEEK_MARGIN_SIZE) {
      requestPosition =
          (position - SEEK_MARGIN_SIZE) - ((position - SEEK_MARGIN_SIZE) % 16);
      this->enableRequestMargin = false;
    }
    size_t endPosition = requestPosition + HTTP_BUFFER_SIZE - 1;
    if (endPosition > this->totalFileSize) {
      endPosition = this->totalFileSize - 1;
    }

    if (!this->httpConnection->get(
            cdnUrl, {bell::HTTPClient::RangeHeader::range(requestPosition,
                                                          endPosition)})) {
      return -1;
    }
    this->lastRequestPosition = requestPosition;
    this->lastRequestCapacity = this->httpConnection->contentLength();

    this->httpConnection->stream().read((char*)this->httpBuffer.data(),
                                        lastRequestCapacity);
    this->decrypt(this->httpBuffer.data(), lastRequestCapacity,
                  this->lastRequestPosition);

    return readBytes(dst, bytes);
  }

  return bytes;
}
#endif

size_t CDNAudioFile::getSize() {
  return this->totalFileSize;
}

void CDNAudioFile::decrypt(uint8_t* dst, size_t nbytes, size_t pos) {
  decryptWith(*crypto, dst, nbytes, pos);
}
