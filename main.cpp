/*
    Ultrasonic-Proximity-Node v1.0.0
    - Migrated to ParamServer v1.2.0 unified messaging framework.
    - Standardized Critical Alert status code (Status 2).
    - Dispatches "msg dd01" UDP alerts for supervisor WAV playback.
    - Features non-blocking local one-shot buzzer & quiet re-arm lockout state machine.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "ParamServer.h"

// ========== HARDWARE PIN DEFINITIONS ==========
#define TRIG_PIN   22
#define ECHO_PIN   21
#define BUZZER_PIN 2

// ========== PARAMSERVER CONTROL VARIABLES ==========
// Bound directly to the UDP command engine and stored in LittleFS
float threshold = 180.0;       // Bound to "threshold" (Distance in cm)
int alarm_duration = 1000;     // Bound to "alarm_dur" (Local buzzer pulse time in ms)
int system_status = 0;         // Bound to "status" -> 0 = Ready, 2 = Critical Motion Latched
int device_number = 0;         // Bound to "device_number" (Default ID 00 for unassigned)
int time_zone = 0;             // Default to UTC
float dst = 0;                 // Default no daylight saving time
String device_name = "Proximity Node"; // Default, change to actual name
String version = "Ultrasonic-Proximity-Node v1.0.0";

// Transient Parameter (Read-Only live sensor telemetry, not saved to Flash)
float distance = 0.0;          // Bound to "distance"

// ========== TIMING CONSTANTS ==========
#define RESET_DELAY_MS      2000   // Obstacle must be clear for 2 seconds to re-arm
#define ECHO_TIMEOUT_US     20000  // Maximum pulse timeout (~340 cm range limit)
#define TRIGGER_DELAY_MS    1000   // Obstacle must persist for 1 second to confirm exit intent

// ========== CONFIGURATION CONSTANTS FOR OTA ==========
const char* otaHostname = "ProximityNode";
const char* otaPassword = "admin";

// ========== RUNTIME STATE MACHINE VARIABLES ==========
unsigned long last_measurement_time = 0;
unsigned long objectFirstSeenMs = 0;
unsigned long buzzer_start_time = 0;
unsigned long clearFirstSeenMs = 0;

enum LocalAlarmState { LOCAL_IDLE, LOCAL_ALARMING, LOCAL_LOCKED_OUT };
LocalAlarmState local_state = LOCAL_IDLE;

// ========== INSTANTIATE CORE OBJECTS ==========
ParamServer server(8888); // Listens on Port 8888 for supervisor commands

// ========== FUNCTION PROTOTYPES ==========
float measureDistanceCM();
void setupOTA();

void setupOTA() {
    ArduinoOTA.setHostname(otaHostname);
    if (strlen(otaPassword) > 0) {
        ArduinoOTA.setPassword(otaPassword);
    }
    
    ArduinoOTA.setTimeout(30000);
    ArduinoOTA.setPort(3232);

    ArduinoOTA.onStart([]() {
        Serial.println("\n[*] OTA Update initiating...");
    });
    
    ArduinoOTA.onEnd([]() {
        Serial.println("\n[+] OTA Update finalized. Rebooting...");
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[-] OTA Error Code [%u]\n", error);
    });

    ArduinoOTA.begin();
}

void setup() {
    Serial.begin(115200);
    
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // 1. Initial boot acknowledgement beep (100ms)
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);

    // 2. Register Parameters with ParamServer v1.1.0 Protocol
    server.registerParam("version", &version, true, true);
    server.registerParam("threshold", &threshold);
    server.registerParam("alarm_dur", &alarm_duration);
    server.registerParam("status", &system_status);
    server.registerParam("device_number", &device_number);
    server.registerParam("device_name", &device_name);
    server.registerParam("time_zone", &time_zone);
    server.registerParam("dst", &dst);
    
    // Register live telemetry as Read-Only and Non-Persistent (skip LittleFS writes)
    server.registerParam("distance", &distance, false, true);

    // Offset (hours * 3600), DST (hours * 3600), NTP Server
    server.configureTime(time_zone * 3600, dst * 3600, "pool.ntp.org");

    // 4. Start Parameter Server & WiFiManager Portal Network Layer
    if (!server.begin("Motion_Alarm_AP")) {
        Serial.println("[-] Core ParamServer Network Initialization Failure!");
        while(1);
    }

    // Pass the restored or default device ID to the messaging engine
    server.setDeviceNumber(device_number);

    // 5. Initialize Wireless OTA Engine
    setupOTA();

    Serial.printf("[+] System Ready. Device ID: %02d | Target Threshold: %.1f cm\n", 
                  device_number, threshold);
}

void loop() {
    // 1. Service background network stacks
    server.update();
    ArduinoOTA.handle();

    unsigned long nowMs = millis();

    // 2. Automatic Wi-Fi connection health check (Every 10 seconds)
    static unsigned long last_wifi_check_time = 0;
    if (nowMs - last_wifi_check_time > 10000) {
        last_wifi_check_time = nowMs;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[!] Wi-Fi disconnected. Attempting background reconnect...");
            WiFi.reconnect(); 
        }
    }

    // 3. Instant timed local buzzer shutoff
    if (local_state == LOCAL_ALARMING && (nowMs - buzzer_start_time >= (unsigned long)alarm_duration)) {
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("[*] One-shot local alarm pulse complete. Entering caregiver quiet lockout.");
        local_state = LOCAL_LOCKED_OUT;
    }

    // 4. Main Ultrasonic State Machine Runtime (Every 200ms)
    if (nowMs - last_measurement_time >= 200) {
        last_measurement_time = nowMs;
        
        // Sync ParamServer engine with on-the-fly UDP parameter updates
        static int last_known_dev_num = -1;
        if (device_number != last_known_dev_num) { 
            server.setDeviceNumber(device_number); 
            last_known_dev_num = device_number; 
            Serial.printf("[+] Device ID updated to: %02d\n", device_number);
        }

        distance = measureDistanceCM();
        bool isObstacle = (distance < threshold);
        
        // Debug Terminal Output (Every ~1 Second)
        static int debugCounter = 0;
        if (++debugCounter >= 5) {
            debugCounter = 0;
            String modeStr = (local_state == LOCAL_IDLE) ? "READY" : 
                             (local_state == LOCAL_ALARMING) ? "ALARMING" : "LOCKED_OUT";
                             
            Serial.printf("[Radar] Dist: %.1f cm | Target: %s | UDP Latch: %d | Local State: %s\n", 
                          (distance > 998.0 ? 400.0 : distance), 
                          (isObstacle ? "YES" : "NO"), 
                          system_status,
                          modeStr.c_str());
        }

        // =======================================================
        //  UNIFIED LOCAL ONE-SHOT & UDP ALARM STATE ENGINE
        // =======================================================
        switch (local_state) {
            case LOCAL_IDLE:
                clearFirstSeenMs = 0;
                if (isObstacle) {
                    if (objectFirstSeenMs == 0) {
                        objectFirstSeenMs = nowMs;
                        Serial.println("[*] Entry threshold crossed. Timing confirmation...");
                    } 
                    else if (nowMs - objectFirstSeenMs >= TRIGGER_DELAY_MS) {
                        // A. Trigger local physical buzzer
                        Serial.println(">>> LOCAL ONE-SHOT ALARM ACTIVE <<<");
                        digitalWrite(BUZZER_PIN, HIGH);
                        buzzer_start_time = nowMs;
                        objectFirstSeenMs = 0;
                        local_state = LOCAL_ALARMING;
                        
                        // B. Lock local status to Critical (2) and dispatch "msg dd01" to supervisors
                        system_status = 2; 
                        Serial.printf("[CRITICAL] EDGE_MOTION_DETECTED! Dispatching 'msg %02d01' alerts...\n", device_number);
                        
                        // Formats "msg dd01", sends via UDP to all supervisors, and locks their status matrix to 2
                        server.dispatchCritical(1); 
                    }
                } else {
                    objectFirstSeenMs = 0;
                }
                break;

            case LOCAL_ALARMING:
                // High-speed clock shutoff managed by Step 3 above
                break;

            case LOCAL_LOCKED_OUT:
                if (isObstacle) {
                    // Patient/obstacle still present; maintain silent lockout
                    clearFirstSeenMs = 0;
                } else {
                    // Path clear; verify it remains clear for the full 2-second window
                    if (clearFirstSeenMs == 0) {
                        clearFirstSeenMs = nowMs;
                        Serial.println("[*] Path cleared. Monitoring 2-second re-arm criteria...");
                    } 
                    else if (nowMs - clearFirstSeenMs >= RESET_DELAY_MS) {
                        Serial.println("--- Lockout expired. Local motion system re-armed ---");
                        
                        // Sound crisp 10ms alignment pulse to signify system is ready
                        digitalWrite(BUZZER_PIN, HIGH);
                        delay(10); 
                        digitalWrite(BUZZER_PIN, LOW);
                        
                        clearFirstSeenMs = 0;
                        system_status = 0;       // Reset status back to Ready (0)
                        local_state = LOCAL_IDLE; // Re-enable detection state engine
                    }
                }
                break;
        }
    }

    // Yield CPU time back to FreeRTOS to keep Wi-Fi stack stable
    vTaskDelay(1);
}

// ========== HC-SR04 PULSE CONVERSION ENGINE ==========
float measureDistanceCM() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    long duration = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
    
    if (duration == 0) {
        return 999.0; // Out-of-bounds safety handler
    }
    
    return (duration * 0.0343) / 2.0;
}