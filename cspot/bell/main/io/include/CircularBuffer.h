#pragma once

#include <cstdint>  // for uint8_t
#include <cstring>  // for size_t
#include <memory>   // for unique_ptr
#include <mutex>    // for mutex

#include "WrappedSemaphore.h"  // for WrappedSemaphore

namespace bell {
    class CircularBuffer {
    public:
        CircularBuffer(size_t dataCapacity);
        ~CircularBuffer();

        // Disable copy constructor and copy assignment
        CircularBuffer(const CircularBuffer&) = delete;
        CircularBuffer& operator=(const CircularBuffer&) = delete;

        // Enable move constructor and move assignment
        CircularBuffer(CircularBuffer&& other) noexcept;
        CircularBuffer& operator=(CircularBuffer&& other) noexcept;

        std::unique_ptr<bell::WrappedSemaphore> dataSemaphore;

        size_t size() const { return dataSize; }

        size_t capacity() const { return dataCapacity; }

        size_t write(const uint8_t* data, size_t bytes);
        size_t read(uint8_t* data, size_t bytes);
        void emptyBuffer();
        void emptyExcept(size_t size);

    private:
        std::mutex bufferMutex;
        size_t begIndex = 0;
        size_t endIndex = 0;
        size_t dataSize = 0;
        size_t dataCapacity = 0;
        uint8_t* buffer = nullptr;  // Raw pointer for PSRAM allocation
    };
}  // namespace bell