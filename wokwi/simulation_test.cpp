#include <Arduino.h>

// Simulate your Bluetooth Speaker status with LEDs
#define LED_BLUETOOTH_STATUS 22  // Green LED - BT connected
#define LED_AUDIO_STATUS     25  // Red LED - Audio playing  
#define LED_SYSTEM_STATUS     2  // Blue LED - System status

// Simulate AVRCP commands via Serial
enum SimulatedAVRCPCommand {
    CMD_PLAY = 1,
    CMD_PAUSE = 2, 
    CMD_STOP = 3,
    CMD_NEXT = 4,
    CMD_PREV = 5
};

enum PlaybackState {
    STATE_STOPPED = 0,
    STATE_PLAYING = 1,
    STATE_PAUSED = 2
};

class BluetoothAudioSimulator {
private:
    PlaybackState current_state = STATE_STOPPED;
    bool bluetooth_connected = false;
    unsigned long last_status_update = 0;
    
public:
    void init() {
        Serial.begin(115200);
        
        // Initialize status LEDs
        pinMode(LED_BLUETOOTH_STATUS, OUTPUT);
        pinMode(LED_AUDIO_STATUS, OUTPUT);
        pinMode(LED_SYSTEM_STATUS, OUTPUT);
        
        // System startup sequence
        Serial.println("=== ESP32 Bluetooth Speaker Simulator ===");
        Serial.println("Initializing system...");
        
        // Simulate initialization steps
        digitalWrite(LED_SYSTEM_STATUS, HIGH);
        delay(500);
        
        Serial.println("✓ NVS Flash initialized");
        delay(200);
        
        Serial.println("✓ GPIO configured");
        delay(200);
        
        Serial.println("✓ I2S pins configured (GPIO 22, 25, 26)");
        delay(200);
        
        Serial.println("✓ AVRCP Target initialized");
        delay(200);
        
        Serial.println("✓ Audio pipeline ready");
        delay(200);
        
        Serial.println("System ready! Waiting for Bluetooth connection...");
        Serial.println("\nSimulator Commands:");
        Serial.println("1 = PLAY, 2 = PAUSE, 3 = STOP, 4 = NEXT, 5 = PREV");
        Serial.println("9 = Toggle BT connection");
        
        update_leds();
    }
    
    void loop() {
        // Check for serial commands
        if (Serial.available()) {
            int cmd = Serial.parseInt();
            handle_command(cmd);
        }
        
        // Periodic status updates
        if (millis() - last_status_update > 5000) {
            print_status();
            last_status_update = millis();
        }
        
        // Simulate audio processing blink
        if (current_state == STATE_PLAYING) {
            static unsigned long last_blink = 0;
            if (millis() - last_blink > 500) {
                digitalWrite(LED_AUDIO_STATUS, !digitalRead(LED_AUDIO_STATUS));
                last_blink = millis();
            }
        }
        
        delay(10);
    }
    
private:
    void handle_command(int cmd) {
        switch (cmd) {
            case 9: // Toggle Bluetooth connection
                bluetooth_connected = !bluetooth_connected;
                Serial.printf("Bluetooth %s\n", bluetooth_connected ? "CONNECTED" : "DISCONNECTED");
                if (!bluetooth_connected) {
                    current_state = STATE_STOPPED;
                }
                update_leds();
                break;
                
            case CMD_PLAY:
                if (bluetooth_connected) {
                    current_state = STATE_PLAYING;
                    Serial.println("🎵 AVRCP: PLAY command - Audio playing");
                } else {
                    Serial.println("❌ No Bluetooth connection");
                }
                update_leds();
                break;
                
            case CMD_PAUSE:
                if (bluetooth_connected && current_state == STATE_PLAYING) {
                    current_state = STATE_PAUSED;
                    Serial.println("⏸️ AVRCP: PAUSE command - Audio paused");
                } else {
                    Serial.println("❌ Cannot pause - not playing");
                }
                update_leds();
                break;
                
            case CMD_STOP:
                if (bluetooth_connected) {
                    current_state = STATE_STOPPED;
                    Serial.println("⏹️ AVRCP: STOP command - Audio stopped");
                } else {
                    Serial.println("❌ No Bluetooth connection");
                }
                update_leds();
                break;
                
            case CMD_NEXT:
                if (bluetooth_connected) {
                    Serial.println("⏭️ AVRCP: NEXT command - Track navigation not implemented");
                } else {
                    Serial.println("❌ No Bluetooth connection");
                }
                break;
                
            case CMD_PREV:
                if (bluetooth_connected) {
                    Serial.println("⏮️ AVRCP: PREV command - Track navigation not implemented");
                } else {
                    Serial.println("❌ No Bluetooth connection");
                }
                break;
                
            default:
                if (cmd != 0) { // Ignore empty reads
                    Serial.println("❓ Unknown command");
                }
                break;
        }
    }
    
    void update_leds() {
        // Bluetooth status LED
        digitalWrite(LED_BLUETOOTH_STATUS, bluetooth_connected ? HIGH : LOW);
        
        // Audio status LED
        if (current_state == STATE_PLAYING) {
            // Will blink in main loop
        } else if (current_state == STATE_PAUSED) {
            digitalWrite(LED_AUDIO_STATUS, HIGH);
        } else {
            digitalWrite(LED_AUDIO_STATUS, LOW);
        }
    }
    
    void print_status() {
        Serial.println("\n--- Status ---");
        Serial.printf("Bluetooth: %s\n", bluetooth_connected ? "CONNECTED" : "DISCONNECTED");
        Serial.printf("Playback: %s\n", get_state_name(current_state));
        Serial.printf("System: %s\n", "RUNNING");
        Serial.println("-------------\n");
    }
    
    const char* get_state_name(PlaybackState state) {
        switch (state) {
            case STATE_STOPPED: return "STOPPED";
            case STATE_PLAYING: return "PLAYING";
            case STATE_PAUSED: return "PAUSED";
            default: return "UNKNOWN";
        }
    }
};

BluetoothAudioSimulator simulator;

void setup() {
    simulator.init();
}

void loop() {
    simulator.loop();
} 