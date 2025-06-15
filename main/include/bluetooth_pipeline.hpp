#pragma once

#include "esp_raii_wrapper.hpp"
#include <cstring>

extern "C" {
#include "esp_err.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "board.h"  // Use centralized pin definitions
}

namespace btspeaker {

// - I2S_BCK_IO (25): Bit Clock → PCM5102A BCK
// - I2S_WS_IO (22):  Word Select → PCM5102A LCK  
// - I2S_DO_IO (26):  Data Out → PCM5102A DIN

class BluetoothPipeline {
public:
    BluetoothPipeline() = default;
    ~BluetoothPipeline() = default;
    
    // Delete copy operations
    BluetoothPipeline(const BluetoothPipeline&) = delete;
    BluetoothPipeline& operator=(const BluetoothPipeline&) = delete;
    
    // Move operations
    BluetoothPipeline(BluetoothPipeline&&) = default;
    BluetoothPipeline& operator=(BluetoothPipeline&&) = default;
    
    esp_err_t setupPipeline();
    esp_err_t startPipeline();
    esp_err_t stopPipeline();
    esp_err_t pausePipeline();
    esp_err_t resumePipeline();

    void cleanup();

private:
    // RAII-managed resources
    PipelineHandle pipeline_;
    ElementHandle i2SStreamWriter;
    ElementHandle btStreamReader;
    EventIfaceHandle evt_;
    bool btServiceStarted = false;
};

} // namespace btspeaker 