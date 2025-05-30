//
// Created by Shaked Melman on 29/05/2025.
//

#ifndef BT_SPEAKER_TASK_WRAPPER_H
#define BT_SPEAKER_TASK_WRAPPER_H

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include <utility>  // For std::exchange

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
}

namespace smart_speaker {

// Task priorities for different module types
    enum class TaskPriority : UBaseType_t {
        AudioOutput = 23,      // Highest - real-time audio output
        AudioProcessing = 22,  // High - audio mixing and processing
        AudioInput = 21,       // High - audio input handling
        VoiceDetection = 15,   // Medium - voice activity detection
        NetworkProcess = 12,   // Medium - network operations
        VoiceProcess = 10,     // Lower - voice command processing
        CloudAPI = 8,          // Lower - cloud API calls
        Background = 5,        // Lowest - housekeeping
        SystemMonitor = 3      // Lowest - performance monitoring
    };

// CPU core assignment
    enum class TaskCore : BaseType_t {
        Audio = 0,    // Core 0 - dedicated for audio processing
        Network = 1,  // Core 1 - network and general processing
        Any = tskNO_AFFINITY
    };

// RAII task wrapper with C++ exception safety
    class Task {
    private:
        TaskHandle_t handle_;
        std::string name_;
        std::atomic<bool> should_stop_;
        std::atomic<bool> running_;

        using TaskFunction = std::function<void()>;
        std::unique_ptr<TaskFunction> task_function_;

        // Static wrapper for FreeRTOS C API
        static void task_wrapper(void* params);

    public:
        // Constructor with function object
        template<typename Func>
        Task(const std::string& name,
             TaskPriority priority,
             size_t stack_size,
             TaskCore core,
             Func&& func)
                : name_(name), should_stop_(false), running_(false) {

            task_function_ = std::make_unique<TaskFunction>(std::forward<Func>(func));

            BaseType_t result = xTaskCreatePinnedToCore(
                    task_wrapper,
                    name_.c_str(),
                    stack_size,
                    this,  // Pass this object to wrapper
                    static_cast<UBaseType_t>(priority),
                    &handle_,
                    static_cast<BaseType_t>(core)
            );

            if (result != pdPASS) {
                ESP_LOGE("TASK", "Failed to create task: %s", name_.c_str());
                // Don't throw exceptions since they're disabled
                handle_ = nullptr;
                return;
            }

            ESP_LOGI("TASK", "Created task: %s (priority=%d, core=%d)",
                     name_.c_str(), static_cast<int>(priority), static_cast<int>(core));
        }

        // Destructor ensures clean shutdown
        ~Task() {
            stop();
            if (handle_) {
                vTaskDelete(handle_);
                ESP_LOGI("TASK", "Destroyed task: %s", name_.c_str());
            }
        }

        // Non-copyable, movable
        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;

        Task(Task&& other) noexcept
                : handle_(std::exchange(other.handle_, nullptr)),
                  name_(std::move(other.name_)),
                  should_stop_(other.should_stop_.load()),
                  running_(other.running_.load()),
                  task_function_(std::move(other.task_function_)) {}

        // Task control
        void stop() {
            should_stop_.store(true, std::memory_order_release);

            // Wait for task to acknowledge stop
            uint32_t wait_count = 0;
            while (running_.load(std::memory_order_acquire) && wait_count < 100) {
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_count++;
            }

            if (running_.load()) {
                ESP_LOGW("TASK", "Task %s did not stop gracefully", name_.c_str());
            }
        }

        void suspend() {
            if (handle_) vTaskSuspend(handle_);
        }

        void resume() {
            if (handle_) vTaskResume(handle_);
        }

        // Status queries
        bool is_running() const {
            return running_.load(std::memory_order_acquire);
        }

        bool should_stop() const {
            return should_stop_.load(std::memory_order_acquire);
        }

        const std::string& name() const { return name_; }
        TaskHandle_t handle() const { return handle_; }

        UBaseType_t priority() const {
            return handle_ ? uxTaskPriorityGet(handle_) : 0;
        }

        void set_priority(TaskPriority new_priority) {
            if (handle_) {
                vTaskPrioritySet(handle_, static_cast<UBaseType_t>(new_priority));
            }
        }

    private:
        friend void task_wrapper(void* params);

        void run() {
            running_.store(true, std::memory_order_release);
            ESP_LOGI("TASK", "Task started: %s", name_.c_str());

            // Since exceptions are disabled, use simple error handling
            if (task_function_) {
                (*task_function_)();
            } else {
                ESP_LOGE("TASK", "Task %s has no function to execute", name_.c_str());
            }

            running_.store(false, std::memory_order_release);
            ESP_LOGI("TASK", "Task finished: %s", name_.c_str());
        }
    };

// Utility class for task coordination
    class TaskCoordinator {
    private:
        std::vector<std::unique_ptr<Task>> tasks_;
        SemaphoreHandle_t mutex_;

    public:
        TaskCoordinator() : mutex_(xSemaphoreCreateMutex()) {}

        ~TaskCoordinator() {
            stop_all_tasks();
            if (mutex_) {
                vSemaphoreDelete(mutex_);
            }
        }

        // Add task to coordinator
        template<typename Func>
        bool add_task(const std::string& name,
                      TaskPriority priority,
                      size_t stack_size,
                      TaskCore core,
                      Func&& func) {
            xSemaphoreTake(mutex_, portMAX_DELAY);

            auto task = std::make_unique<Task>(name, priority, stack_size, core,
                                               std::forward<Func>(func));

            // Check if task creation was successful
            if (task->handle() == nullptr) {
                xSemaphoreGive(mutex_);
                return false;
            }

            tasks_.push_back(std::move(task));
            xSemaphoreGive(mutex_);
            return true;
        }

        // Stop all tasks gracefully
        void stop_all_tasks() {
            xSemaphoreTake(mutex_, portMAX_DELAY);

            ESP_LOGI("TASK", "Stopping %zu tasks...", tasks_.size());

            // Signal all tasks to stop
            for (auto& task : tasks_) {
                task->stop();
            }

            // Clear task list (destructors will clean up)
            tasks_.clear();

            xSemaphoreGive(mutex_);
            ESP_LOGI("TASK", "All tasks stopped");
        }

        size_t task_count() const {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            size_t count = tasks_.size();
            xSemaphoreGive(mutex_);
            return count;
        }

        // Get task statistics
        void print_task_stats() const {
            xSemaphoreTake(mutex_, portMAX_DELAY);

            ESP_LOGI("TASK", "=== Task Statistics ===");
            for (const auto& task : tasks_) {
                ESP_LOGI("TASK", "Task: %s, Priority: %lu, Running: %s",
                         task->name().c_str(),
                         task->priority(),
                         task->is_running() ? "Yes" : "No");
            }

            xSemaphoreGive(mutex_);
        }
    };

// Factory functions for common task patterns
    namespace TaskFactory {

// Create high-priority audio task
        template<typename Func>
        std::unique_ptr<Task> create_audio_task(const std::string& name, Func&& func) {
            return std::make_unique<Task>(name, TaskPriority::AudioProcessing,
                                          4096, TaskCore::Audio, std::forward<Func>(func));
        }

// Create network processing task
        template<typename Func>
        std::unique_ptr<Task> create_network_task(const std::string& name, Func&& func) {
            return std::make_unique<Task>(name, TaskPriority::NetworkProcess,
                                          8192, TaskCore::Network, std::forward<Func>(func));
        }

// Create background monitoring task
        template<typename Func>
        std::unique_ptr<Task> create_monitor_task(const std::string& name, Func&& func) {
            return std::make_unique<Task>(name, TaskPriority::SystemMonitor,
                                          2048, TaskCore::Any, std::forward<Func>(func));
        }

    } // namespace TaskFactory

// Static task wrapper implementation
inline void Task::task_wrapper(void* params) {
    Task* task = static_cast<Task*>(params);
    if (task) {
        task->run();
    }
    vTaskDelete(nullptr);  // Clean up this task
}

} // namespace smart_speaker

#endif //BT_SPEAKER_TASK_WRAPPER_H