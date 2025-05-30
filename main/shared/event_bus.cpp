//
// EventBus Implementation - Add this to the end of smart_speaker.cpp or create shared/event_bus.cpp
//

#include "event_bus.hpp"


namespace smart_speaker {

// EventBus Constructor
    EventBus::EventBus()
            : mutex_(xSemaphoreCreateMutex()),
              event_queue_(xQueueCreate(MAX_EVENTS, sizeof(Event))),
              dispatch_task_(nullptr),
              running_(false) {
        ESP_LOGI("EVENT_BUS", "EventBus constructor");
    }

// EventBus Destructor
    EventBus::~EventBus() {
        ESP_LOGI("EVENT_BUS", "EventBus destructor");
        stop();

        if (mutex_) {
            vSemaphoreDelete(mutex_);
        }
        if (event_queue_) {
            vQueueDelete(event_queue_);
        }
    }

// Start event processing
    bool EventBus::start() {
        ESP_LOGI("EVENT_BUS", "EventBus start");

        if (running_) {
            return true;
        }

        running_ = true;

        // Create dispatch task
        BaseType_t result = xTaskCreate(
                dispatch_task_impl,
                "event_dispatch",
                STACK_SIZE,
                this,
                TASK_PRIORITY,
                &dispatch_task_
        );

        if (result != pdPASS) {
            ESP_LOGE("EVENT_BUS", "Failed to create dispatch task");
            running_ = false;
            return false;
        }

        return true;
    }

// Stop event processing
    void EventBus::stop() {
        ESP_LOGI("EVENT_BUS", "EventBus stop");

        if (!running_) {
            return;
        }

        running_ = false;

        // Wait for dispatch task to finish
        if (dispatch_task_) {
            vTaskDelay(pdMS_TO_TICKS(100)); // Give it time to stop
            dispatch_task_ = nullptr;
        }
    }

// Subscribe to events
    void EventBus::subscribe(EventType type, EventHandler handler) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        handlers_[type].push_back(handler);
        xSemaphoreGive(mutex_);

        ESP_LOGI("EVENT_BUS", "Subscribed to event type: %lu", static_cast<uint32_t>(type));
    }

// Unsubscribe from events
    void EventBus::unsubscribe(EventType type) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        handlers_[type].clear();
        xSemaphoreGive(mutex_);

        ESP_LOGI("EVENT_BUS", "Unsubscribed from event type: %lu", static_cast<uint32_t>(type));
    }

// Static dispatch task implementation
    void EventBus::dispatch_task_impl(void* params) {
        EventBus* bus = static_cast<EventBus*>(params);
        bus->dispatch_loop();
        vTaskDelete(nullptr);
    }

// Dispatch loop
    void EventBus::dispatch_loop() {
        ESP_LOGI("EVENT_BUS", "Dispatch loop started");

        Event event;
        while (running_) {
            // Wait for events with a timeout
            if (xQueueReceive(event_queue_, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
                dispatch_event(event);
            }
        }

        ESP_LOGI("EVENT_BUS", "Dispatch loop stopped");
    }

// Dispatch single event
    void EventBus::dispatch_event(const Event& event) {
        xSemaphoreTake(mutex_, portMAX_DELAY);

        auto it = handlers_.find(event.type);
        if (it != handlers_.end()) {
            for (const auto& handler : it->second) {
                handler(event);
            }
        }

        xSemaphoreGive(mutex_);
    }

} // namespace smart_speaker