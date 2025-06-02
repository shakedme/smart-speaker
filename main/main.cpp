//
// Created by Shaked Melman on 29/05/2025.
//

#include "main.hpp"
#include "app/app_bluetooth_audio.cpp"


extern "C" void app_main() {
    static btspeaker::BluetoothAudioApp app;
    app.init();
    app.run();
} 