#pragma once

#include <functional>

extern "C" {
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
}

namespace btspeaker {

// RAII wrapper for ESP-IDF handles
template<typename Handle>
class EspHandle {
public:
    using DeleterFunc = void(*)(Handle);
    
    EspHandle(Handle handle = nullptr, DeleterFunc deleter = nullptr) 
        : handle_(handle), deleter_(deleter) {}
    
    ~EspHandle() { reset(); }
    
    // Move constructor
    EspHandle(EspHandle&& other) noexcept 
        : handle_(other.handle_), deleter_(other.deleter_) {
        other.handle_ = nullptr;
        other.deleter_ = nullptr;
    }
    
    // Move assignment
    EspHandle& operator=(EspHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            deleter_ = other.deleter_;
            other.handle_ = nullptr;
            other.deleter_ = nullptr;
        }
        return *this;
    }
    
    // Delete copy operations
    EspHandle(const EspHandle&) = delete;
    EspHandle& operator=(const EspHandle&) = delete;
    
    void reset(Handle new_handle = nullptr, DeleterFunc new_deleter = nullptr) {
        if (handle_ && deleter_) {
            deleter_(handle_);
        }
        handle_ = new_handle;
        deleter_ = new_deleter;
    }
    
    Handle get() const { return handle_; }
    Handle* get_ptr() { return &handle_; }
    operator bool() const { return handle_ != nullptr; }
    
    Handle release() {
        Handle h = handle_;
        handle_ = nullptr;
        deleter_ = nullptr;
        return h;
    }

private:
    Handle handle_;
    DeleterFunc deleter_;
};

// Specialized deleters for ESP-IDF handles
void audio_pipeline_deleter(audio_pipeline_handle_t handle);
void audio_element_deleter(audio_element_handle_t handle);
void audio_event_iface_deleter(audio_event_iface_handle_t handle);

// Type aliases for commonly used handles
using PipelineHandle = EspHandle<audio_pipeline_handle_t>;
using ElementHandle = EspHandle<audio_element_handle_t>;
using EventIfaceHandle = EspHandle<audio_event_iface_handle_t>;

} // namespace btspeaker 