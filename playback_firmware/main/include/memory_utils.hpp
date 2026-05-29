#pragma once

#include <memory>
#include <freertos/ringbuf.h>

extern "C" {
#include "esp_heap_caps.h"
#include "esp_log.h"
}

namespace btspeaker {

/**
 * @brief Memory allocation utilities for PSRAM optimization
 */
    class MemoryUtils {
    public:

        /**
         * @brief Log memory statistics
         */
        static void logMemoryStats(const char *context = "") {
            ESP_LOGI("MEM_STATS", "%s - Free heap: %lu, Free PSRAM: %zu, Largest free block: %zu",
                     context,
                     esp_get_free_heap_size(),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    };

/**
 * @brief RAII wrapper for PSRAM-allocated ring buffers
 */
    class PSRAMRingBuffer {
    private:
        RingbufHandle_t handle_;
        uint8_t *storage_;
        StaticRingbuffer_t *buffer_struct_;

    public:
        PSRAMRingBuffer(size_t size, RingbufferType_t type = RINGBUF_TYPE_BYTEBUF)
                : handle_(nullptr), storage_(nullptr), buffer_struct_(nullptr) {

            storage_ = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
            if (!storage_) return;

            // Allocate structure in internal RAM (small, needs to be fast)
            buffer_struct_ = static_cast<StaticRingbuffer_t *>(
                    heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_8BIT));
            if (!buffer_struct_) {
                free(storage_);
                storage_ = nullptr;
                return;
            }

            handle_ = xRingbufferCreateStatic(size, type, storage_, buffer_struct_);
        }

        ~PSRAMRingBuffer() {
            if (handle_) {
                vRingbufferDelete(handle_);
            }
            if (storage_) free(storage_);
            if (buffer_struct_) free(buffer_struct_);
        }

        [[nodiscard]] RingbufHandle_t get() const { return handle_; }

        [[nodiscard]] bool isValid() const { return handle_ != nullptr; }
    };

} // namespace btspeaker 