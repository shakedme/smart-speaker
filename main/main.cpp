//
// Created by Shaked Melman on 29/05/2025.
//

#include "main.hpp"
#include "src/bluetooth_audio_c_api.cpp"

extern "C" void app_main() {
    // Initialize the Bluetooth Audio Application
    app_bluetooth_audio_init();
    
    // Run the main application (this will block)
    app_bluetooth_audio_run();
    
    // Clean shutdown (this line may never be reached in normal operation)
    app_bluetooth_audio_shutdown();
} 