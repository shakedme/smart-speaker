//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_EVENT_BUS_HPP
#define BT_SPEAKER_EVENT_BUS_HPP

#pragma once

#include "../audio/audio_types.hpp"
#include <functional>
#include <unordered_map>
#include <vector>
#include <memory>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
}

namespace smart_speaker {

// Event types for inter-module communication
    enum class EventType : uint32_t {
        // Audio events
        AudioStateChanged = 0x1000,
        AudioSourceConnected,
        AudioSourceDisconnected,
        AudioVolumeChanged,
        AudioError,

        // Bluetooth events
        BluetoothConnected = 0x2000,
        BluetoothDisconnected,
        BluetoothAudioStarted,
        BluetoothAudioStopped,

        // Voice events
        VoiceWakeWord = 0x3000,
        VoiceCommandReceived,
        VoiceProcessingStarted,
        VoiceProcessingCompleted,
        VoiceError,

        // Network events
        WiFiConnected = 0x4000,
        WiFiDisconnected,
        SpotifyConnected,
        SpotifyDisconnected,
        LLMResponseReceived,

        // System events
        SystemStarted = 0x5000,
        SystemError,
        SystemShutdown,
        PerformanceWarning
    };

// Generic event data structure - Fixed template specialization
    struct Event {
        EventType type;
        uint32_t timestamp_ms;
        size_t data_size;
        std::shared_ptr<void> data;

        // Default constructor
        Event() : type(EventType::SystemStarted), timestamp_ms(0), data_size(0), data(nullptr) {}

        // General constructor for typed data
        template<typename T>
        Event(EventType t, std::shared_ptr<T> d)
                : type(t), timestamp_ms(esp_timer_get_time() / 1000),
                  data_size(sizeof(T)), data(std::static_pointer_cast<void>(d)) {}

        // Specialization for void type (no data events)
        Event(EventType t)
                : type(t), timestamp_ms(esp_timer_get_time() / 1000),
                  data_size(0), data(nullptr) {}

        template<typename T>
        std::shared_ptr<T> get_data() const {
            return std::static_pointer_cast<T>(data);
        }
    };

// Event handler interface
    using EventHandler = std::function<void(const Event&)>;

// Thread-safe event bus for decoupled module communication
    class EventBus {
    private:
        std::unordered_map<EventType, std::vector<EventHandler>> handlers_;
        mutable SemaphoreHandle_t mutex_;
        QueueHandle_t event_queue_;
        TaskHandle_t dispatch_task_;
        bool running_;

        static constexpr size_t MAX_EVENTS = 32;
        static constexpr size_t STACK_SIZE = 4096;
        static constexpr UBaseType_t TASK_PRIORITY = 10;

        // Event dispatch task
        static void dispatch_task_impl(void* params);
        void dispatch_loop();

    public:
        EventBus();
        ~EventBus();

        // Non-copyable, movable
        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;
        EventBus(EventBus&&) = default;
        EventBus& operator=(EventBus&&) = default;

        // Start/stop event processing
        bool start();
        void stop();

        // Event subscription (module registration)
        void subscribe(EventType type, EventHandler handler);
        void unsubscribe(EventType type);

        // Event publishing (non-blocking) - Fixed template handling
        template<typename T>
        bool publish(EventType type, std::shared_ptr<T> data) {
            if (!running_) return false;

            Event event(type, data);
            return xQueueSend(event_queue_, &event, 0) == pdTRUE;
        }

        // Convenience method for simple events without data - Fixed
        bool publish(EventType type) {
            if (!running_) return false;

            Event event(type);  // Use the void constructor
            return xQueueSend(event_queue_, &event, 0) == pdTRUE;
        }

        // Synchronous event dispatch (for critical events)
        template<typename T>
        void publish_sync(EventType type, std::shared_ptr<T> data) {
            if (!running_) return;

            Event event(type, data);
            dispatch_event(event);
        }

    private:
        void dispatch_event(const Event& event);
    };

// Event data structures for specific events
    struct AudioStateData {
        AudioState old_state;
        AudioState new_state;
        AudioSource source;
    };

    struct VolumeChangeData {
        AudioSource source;
        float old_volume;
        float new_volume;
        bool muted;
    };

    struct VoiceCommandData {
        std::string command;
        float confidence;
        uint32_t duration_ms;
    };

    struct PerformanceData {
        float cpu_usage;
        size_t free_heap;
        uint32_t audio_underruns;
        uint32_t audio_overruns;
    };

    struct ErrorData {
        int error_code;
        std::string error_message;
        std::string module_name;
    };

// Global event bus instance (singleton pattern)
    class GlobalEventBus {
    private:
        static std::unique_ptr<EventBus> instance_;
        static SemaphoreHandle_t creation_mutex_;

    public:
        static EventBus& get_instance();
        static void destroy_instance();
    };

// Convenience macros for common event operations
#define PUBLISH_EVENT(type, data) \
    GlobalEventBus::get_instance().publish(type, std::make_shared<decltype(data)>(data))

#define SUBSCRIBE_EVENT(type, handler) \
    GlobalEventBus::get_instance().subscribe(type, handler)

#define PUBLISH_SIMPLE_EVENT(type) \
    GlobalEventBus::get_instance().publish(type)

} // namespace smart_speaker

#endif //BT_SPEAKER_EVENT_BUS_HPP