#include "HTTPClient.h"

#include <stdarg.h>   // for va_list, va_start, va_end
#include <stdio.h>    // for vsnprintf
#include <string.h>   // for memcpy
#include <algorithm>  // for transform
#include <algorithm>
#include <cassert>    // for assert
#include <cctype>     // for tolower
#include <ostream>    // for operator<<, basic_ostream
#include <stdexcept>  // for runtime_error
#include <utility>

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "BellSocket.h"  // for bell
#include "BellUtils.h"   // for BELL_SLEEP_MS

#include "BellUtils.h"   // for BELL_SLEEP_MS

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#ifdef error
#undef error
#endif

using namespace bell;

namespace {
void logHttpClientError(const char* format, ...) {
  if (format == nullptr) {
#ifdef ESP_PLATFORM
    ESP_LOGE("httpClient", "HTTP client error");
#endif
    return;
  }

  va_list args;
  va_start(args, format);
#ifdef ESP_PLATFORM
  char message[256];
  vsnprintf(message, sizeof(message), format, args);
  ESP_LOGE("httpClient", "%s", message);
#else
  char message[512];
  vsnprintf(message, sizeof(message), format, args);
  if (bell::bellGlobalLogger != nullptr) {
    bell::bellGlobalLogger->error(__FILE__, __LINE__, "httpClient", "%s",
                                  message);
  }
#endif
  va_end(args);
}

const char* safeExceptionMessage(const std::exception& ex) {
  const char* message = ex.what();
  return (message != nullptr && message[0] != '\0') ? message : "unknown error";
}
}  // namespace

int HTTPClient::Response::connect(const std::string& url, size_t numHeaders) {
  try {
    urlParser = bell::URLParser::parse(url);
    maxHeaders = numHeaders;
    delete[] phResponseHeaders;  // Ensure no memory leaks if reallocating
    phResponseHeaders = new phr_header[maxHeaders];

    // Open socket of type
    const int openErr = this->socketStream.open(
        urlParser.host, urlParser.port, urlParser.schema == "https");
    if (openErr != 0) {
      logHttpClientError("Stream operation failed while connecting: %s",
                         "connection failed");
      return 1;
    }
    return 0;
  } catch (const std::invalid_argument& ex) {
    logHttpClientError("Error while parsing URL: %s", safeExceptionMessage(ex));
    return 1;
  } catch (const std::runtime_error& ex) {
    logHttpClientError("Stream operation failed while connecting: %s",
                       safeExceptionMessage(ex));
    return 1;
  } catch (const std::exception& ex) {
    logHttpClientError("Unexpected exception: %s", safeExceptionMessage(ex));
    return 2;
  }
}

int HTTPClient::Response::reconnect() {
  if (retries__ == 0)
    return 3;
  retries__--;
  try {
    // Ensure the stream is properly closed before attempting to reconnect
    if (this->socketStream.isOpen()) {
      this->socketStream.flush();
      this->socketStream.close();
    }
    BELL_SLEEP_MS(10);
    return this->socketStream.open(urlParser.host, urlParser.port,
                                   urlParser.schema == "https");
  } catch (const std::runtime_error& ex) {
    logHttpClientError("Stream operation failed while reconnecting: %s",
                       safeExceptionMessage(ex));
    return 1;
  } catch (const std::exception& ex) {
    logHttpClientError("Unexpected exception: %s", safeExceptionMessage(ex));
    return 2;
  }
}

HTTPClient::Response::~Response() {
  delete[] phResponseHeaders;  // Free the dynamically allocated header array
  if (this->socketStream.isOpen()) {
    this->socketStream.close();
  }
}

bool HTTPClient::Response::rawRequest(const std::string& url,
                                      const std::string& method,
                                      const std::vector<uint8_t>& content,
                                      Headers& headers) {
  urlParser = bell::URLParser::parse(url);

  // Prepare a request
  const char* reqEnd = "\r\n";

  try {
    socketStream << method << " " << urlParser.path << " HTTP/1.1" << reqEnd;
    socketStream << "Host: " << urlParser.host << ":" << urlParser.port
                 << reqEnd;
    socketStream << "Connection: keep-alive" << reqEnd;
    socketStream << "Accept: */*" << reqEnd;

    // Write content
    if (!content.empty()) {
      socketStream << "Content-Length: " << content.size() << reqEnd;
    }

    // Write headers
    for (auto& header : headers) {
      socketStream << header.first << ": " << header.second << reqEnd;
    }

    socketStream << reqEnd;

    // Write request body if it exists
    if (!content.empty()) {
      socketStream.write(reinterpret_cast<const char*>(content.data()),
                         content.size());
    }

    socketStream.flush();
  } catch (const std::ios_base::failure& ex) {
    logHttpClientError("Stream operation failed while sending request: %s",
                       safeExceptionMessage(ex));
    if (reconnect() != 0) {
      return false;  // If reconnection fails, return failure
    }
    return rawRequest(url, method, content, headers);  // Retry the request
  }

  // Parse response
  try {
    return readResponseHeaders();
  } catch (const std::underflow_error& ex) {
    logHttpClientError("Underflow error during response reading: %s",
                       safeExceptionMessage(ex));
    this->socketStream.flush();
    this->socketStream.close();
    return false;
  } catch (const std::runtime_error& ex) {
    logHttpClientError("Runtime error while receiving packet: %s",
                       safeExceptionMessage(ex));
    if (reconnect() != 0) {
      return false;
    }
    return rawRequest(url, method, content, headers);
  } catch (const std::exception& ex) {
    logHttpClientError("Unexpected exception: %s", safeExceptionMessage(ex));
    return false;
  }
}

bool HTTPClient::Response::readResponseHeaders() {
  const char* msgPointer;
  size_t msgLen;
  int pret;
  int minorVersion;
  int status;
  size_t prevbuflen = 0;
  size_t numHeaders = maxHeaders;
  this->httpBufferAvailable = 0;
  size_t retries = 0;
  bool foundHttpHeader = false;

  while (true) {
    socketStream.getline((char*)httpBuffer.data() + httpBufferAvailable,
                         httpBuffer.size() - httpBufferAvailable);

    auto bytesRead = socketStream.gcount();

    if (bytesRead > 0) {
      prevbuflen = httpBufferAvailable;
      httpBufferAvailable += bytesRead;

      if (!foundHttpHeader) {
        const char* httpStart = strstr((const char*)httpBuffer.data(), "HTTP/");
        if (httpStart) {
          if ((httpStart >= (const char*)httpBuffer.data()) &&
              (httpStart <
               (const char*)httpBuffer.data() + httpBufferAvailable)) {
            size_t offset = httpStart - (const char*)httpBuffer.data();
            if (offset > httpBufferAvailable) {
              BELL_LOG(debug, "httpClient",
                       "Invalid offset detected: offset=%zu, "
                       "httpBufferAvailable=%zu",
                       offset, httpBufferAvailable);
              throw std::runtime_error("Invalid offset for memmove");
            }
            memmove(httpBuffer.data(), httpStart, httpBufferAvailable - offset);
            httpBufferAvailable -= offset;
            foundHttpHeader = true;
          } else {
            BELL_LOG(debug, "httpClient",
                     "Invalid offset for memmove: httpBufferAvailable=%zu",
                     httpBufferAvailable);
            throw std::runtime_error("Invalid offset for memmove");
          }
        } else {
          throw std::runtime_error("HTTP header not found");
        }
      }

      if (httpBufferAvailable >= 2) {
        memcpy(httpBuffer.data() + httpBufferAvailable - 2, "\r\n", 2);
      } else {
        std::string rawResponse((char*)httpBuffer.data(), httpBufferAvailable);
        throw std::out_of_range("Insufficient space to restore CRLF");
      }

      numHeaders = maxHeaders;
      pret = phr_parse_response((const char*)httpBuffer.data(),
                                httpBufferAvailable, &minorVersion, &status,
                                &msgPointer, &msgLen, phResponseHeaders,
                                &numHeaders, prevbuflen);

      if (pret > 0) {
        break;
      } else if (pret == -1) {
        throw std::runtime_error("Cannot parse HTTP response");
      }

      assert(pret == -2);
    }

    if (httpBufferAvailable == 0) {
      if (socketStream.eof()) {
        throw std::underflow_error("Buffer not available and EOF reached");
      } else if (retries < 10 && socketStream.isOpen()) {
        BELL_LOG(debug, "httpClient",
                 "Buffer not available. Waiting for more data...");
        BELL_SLEEP_MS(10);
        retries++;
      } else {
        throw std::underflow_error("Buffer not available and yield failed");
      }
    }

    if (httpBufferAvailable >= httpBuffer.size()) {
      std::string rawResponse((char*)httpBuffer.data(), httpBufferAvailable);
      BELL_LOG(debug, "httpClient", "Raw HTTP response so far:\n%s",
               rawResponse.c_str());
      throw std::runtime_error("Response too large");
    }
  }

  if (numHeaders > maxHeaders) {
    throw std::runtime_error("Too many headers");
  }
  this->responseHeaders = {};
  auto bufferStart = (const char*)httpBuffer.data();
  for (size_t i = 0; i < numHeaders; ++i) {
    // Check if name is out of bounds
    if ((phResponseHeaders[i].name - bufferStart) +
            phResponseHeaders[i].name_len >
        httpBuffer.size()) {
      throw std::out_of_range("Header name out of bounds");
    }

    // Check if value is out of bounds
    if ((phResponseHeaders[i].value - bufferStart) +
            phResponseHeaders[i].value_len >
        httpBuffer.size()) {
      throw std::out_of_range("Header value out of bounds");
    }

    this->responseHeaders.emplace_back(
        std::string(phResponseHeaders[i].name, phResponseHeaders[i].name_len),
        std::string(phResponseHeaders[i].value,
                    phResponseHeaders[i].value_len));
  }

  std::string_view contentLengthView = header("content-length");
  if (!contentLengthView.empty()) {
    this->hasContentSize = true;
    this->contentSize = std::stoi(std::string(contentLengthView));
  }

  return true;
}

bool HTTPClient::Response::get(const std::string& url, Headers headers) {
  return this->rawRequest(url, "GET", {}, headers);
}

bool HTTPClient::Response::post(const std::string& url, Headers headers,
                                const std::vector<uint8_t>& body) {
  return this->rawRequest(url, "POST", body, headers);
}

size_t HTTPClient::Response::contentLength() {
  return contentSize;
}

std::string_view HTTPClient::Response::header(const std::string& headerName) {
  for (auto& header : this->responseHeaders) {
    assert(!header.first.empty() && "Header key shouldn't be empty");
    assert(!header.second.empty() && "Header value shouldn't be empty");
    std::string headerValue = header.first;
    std::transform(headerValue.begin(), headerValue.end(), headerValue.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (headerName == headerValue) {
      const auto& value = header.second;
      return std::string_view(value);
    }
  }

  return "";
}

size_t HTTPClient::Response::totalLength() {
  auto rangeHeader = header("content-range");

  if (rangeHeader.find("/") != std::string::npos) {
    return std::stoi(
        std::string(rangeHeader.substr(rangeHeader.find("/") + 1)));
  }

  return this->contentLength();
}

void HTTPClient::Response::readRawBody() {
  if (contentSize > 0 && rawBody.size() == 0) {
    rawBody = std::vector<uint8_t>(contentSize);
    socketStream.read((char*)rawBody.data(), contentSize);
  }
}

std::string_view HTTPClient::Response::body() {
  readRawBody();
  return std::string_view((char*)rawBody.data(), rawBody.size());
}

std::vector<uint8_t> HTTPClient::Response::bytes() {
  readRawBody();
  return rawBody;
}