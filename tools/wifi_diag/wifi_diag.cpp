// WiFi diagnostic for the ESP32-P4-WIFI6-Touch-LCD-7B (C6 co-processor via ESP-Hosted).
//
// Findings so far (C6 on ESP-Hosted 2.12.11):
//   - STA scan, AP+STA scan and async scan all work standalone (~3.5 s, 20-28 APs);
//   - flooding esp_wifi_get_config, alone or during a scan, does not upset the C6.
// This version runs WiFiManager's non-blocking portal exactly as the radar sets it up,
// with nothing else, and prints a heartbeat every second so a stalled loop is visible.
// Join "CapsuleRadar-Diag" from a phone and tap Configure WiFi.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include "esp32-hal-hosted.h"
#include "esp_wifi.h"

// Step 2: the plain portal works (scan OK with a phone connected). Now add what the
// radar's main.cpp does around it, to find which one wedges the C6:
//   A wifi_prefer_strongest_ap()  B getWiFiIsSaved() every loop after 20 s  C 2nd WebServer:80
static WiFiManager wm;
static WebServer web(80);

static void wifi_prefer_strongest_ap() {          // copy of main.cpp's
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    if (!WiFi.mode(WIFI_STA)) return;
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return;
    if (conf.sta.scan_method == WIFI_ALL_CHANNEL_SCAN && conf.sta.sort_method == WIFI_CONNECT_AP_BY_SIGNAL) return;
    conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (esp_wifi_set_config(WIFI_IF_STA, &conf) == ESP_OK) Serial.println("[diag] A: prefer-strongest config written");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== WiFiManager portal diagnostic (P4 7B) ===");
    wm.setDebugOutput(true, WM_DEBUG_VERBOSE);
    wm.setConfigPortalBlocking(false);
    wm.setTitle("Capsule Radar diag");
    wm.setSaveConfigCallback([]() { Serial.println("[diag] credentials saved"); });
    wifi_prefer_strongest_ap();                                        // A
    if (wm.autoConnect("CapsuleRadar-Diag")) Serial.println("[diag] connected (saved network)");
    else Serial.println("[diag] portal open: join CapsuleRadar-Diag, tap Configure WiFi");
    web.on("/radar", []() { web.send(200, "text/plain", "radar web server"); });
    web.begin();                                                       // C
    Serial.println("[diag] C: second WebServer on :80 started");
}

void loop() {
    static uint32_t lastBeat = 0, maxGap = 0, lastLoop = 0;
    const uint32_t now = millis();
    if (lastLoop && now - lastLoop > maxGap) maxGap = now - lastLoop;
    lastLoop = now;
    wm.process();
    web.handleClient();
    static uint32_t lastRetry = 0, isSavedCalls = 0;                   // B (main.cpp's pattern)
    if (millis() - lastRetry > 20000UL && wm.getWiFiIsSaved()) {
        lastRetry = millis();
        WiFi.begin();
    } else if (millis() - lastRetry > 20000UL) {
        isSavedCalls++;
    }
    if (now - lastBeat >= 1000) {
        Serial.printf("[B] getWiFiIsSaved calls last second: %lu\n", (unsigned long)isSavedCalls);
        isSavedCalls = 0;
        lastBeat = now;
        Serial.printf("[beat] %lus  longest loop gap %lums  stations %d\n",
                      (unsigned long)(now / 1000), (unsigned long)maxGap, WiFi.softAPgetStationNum());
        maxGap = 0;
    }
}
