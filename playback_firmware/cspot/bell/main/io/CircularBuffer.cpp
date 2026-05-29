#include "CircularBuffer.h"

#include <algorithm>  // for min
#include <esp_heap_caps.h>  // for heap_caps_malloc/free

using namespace bell;

CircularBuffer::CircularBuffer(size_t dataCapacity) {
    this->dataCapacity = dataCapacity;

    // Allocate buffer in PSRAM
    buffer = static_cast<uint8_t*>(heap_caps_malloc(dataCapacity, MALLOC_CAP_SPIRAM));

    // Fallback to internal RAM if PSRAM allocation fails
    if (buffer == nullptr) {
        buffer = static_cast<uint8_t*>(malloc(dataCapacity));
        if (buffer == nullptr) {
            throw std::bad_alloc();
        }
    }

    // Initialize buffer to zero
    memset(buffer, 0, dataCapacity);

    this->dataSemaphore = std::make_unique<bell::WrappedSemaphore>(5);
}

CircularBuffer::~CircularBuffer() {
    if (buffer != nullptr) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
}

CircularBuffer::CircularBuffer(CircularBuffer&& other) noexcept {
    std::lock_guard<std::mutex> guard(other.bufferMutex);

    buffer = other.buffer;
    dataCapacity = other.dataCapacity;
    begIndex = other.begIndex;
    endIndex = other.endIndex;
    dataSize = other.dataSize;
    dataSemaphore = std::move(other.dataSemaphore);

    other.buffer = nullptr;
    other.dataCapacity = 0;
    other.begIndex = 0;
    other.endIndex = 0;
    other.dataSize = 0;
}

CircularBuffer& CircularBuffer::operator=(CircularBuffer&& other) noexcept {
    if (this != &other) {
        std::lock(bufferMutex, other.bufferMutex);
        std::lock_guard<std::mutex> guard1(bufferMutex, std::adopt_lock);
        std::lock_guard<std::mutex> guard2(other.bufferMutex, std::adopt_lock);

        // Free existing buffer
        if (buffer != nullptr) {
            heap_caps_free(buffer);
        }

        buffer = other.buffer;
        dataCapacity = other.dataCapacity;
        begIndex = other.begIndex;
        endIndex = other.endIndex;
        dataSize = other.dataSize;
        dataSemaphore = std::move(other.dataSemaphore);

        other.buffer = nullptr;
        other.dataCapacity = 0;
        other.begIndex = 0;
        other.endIndex = 0;
        other.dataSize = 0;
    }
    return *this;
}

size_t CircularBuffer::write(const uint8_t* data, size_t bytes) {
    if (bytes == 0 || buffer == nullptr)
        return 0;

    std::lock_guard<std::mutex> guard(bufferMutex);
    size_t bytesToWrite = std::min(bytes, dataCapacity - dataSize);

    // Write in a single step
    if (bytesToWrite <= dataCapacity - endIndex) {
        memcpy(buffer + endIndex, data, bytesToWrite);
        endIndex += bytesToWrite;
        if (endIndex == dataCapacity)
            endIndex = 0;
    }
        // Write in two steps
    else {
        size_t firstChunkSize = dataCapacity - endIndex;
        memcpy(buffer + endIndex, data, firstChunkSize);
        size_t secondChunkSize = bytesToWrite - firstChunkSize;
        memcpy(buffer, data + firstChunkSize, secondChunkSize);
        endIndex = secondChunkSize;
    }

    dataSize += bytesToWrite;

    // this->dataSemaphore->give();
    return bytesToWrite;
}

void CircularBuffer::emptyBuffer() {
    std::lock_guard<std::mutex> guard(bufferMutex);
    begIndex = 0;
    dataSize = 0;
    endIndex = 0;
}

void CircularBuffer::emptyExcept(size_t sizeToSet) {
    std::lock_guard<std::mutex> guard(bufferMutex);
    if (sizeToSet > dataSize)
        sizeToSet = dataSize;
    dataSize = sizeToSet;
    endIndex = begIndex + sizeToSet;
    if (endIndex > dataCapacity) {
        endIndex -= dataCapacity;
    }
}

size_t CircularBuffer::read(uint8_t* data, size_t bytes) {
    if (bytes == 0 || buffer == nullptr)
        return 0;

    std::lock_guard<std::mutex> guard(bufferMutex);
    size_t bytesToRead = std::min(bytes, dataSize);

    // Read in a single step
    if (bytesToRead <= dataCapacity - begIndex) {
        memcpy(data, buffer + begIndex, bytesToRead);
        begIndex += bytesToRead;
        if (begIndex == dataCapacity)
            begIndex = 0;
    }
        // Read in two steps
    else {
        size_t firstChunkSize = dataCapacity - begIndex;
        memcpy(data, buffer + begIndex, firstChunkSize);
        size_t secondChunkSize = bytesToRead - firstChunkSize;
        memcpy(data + firstChunkSize, buffer, secondChunkSize);
        begIndex = secondChunkSize;
    }

    dataSize -= bytesToRead;
    return bytesToRead;
}