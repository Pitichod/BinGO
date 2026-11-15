#pragma once

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>

constexpr unsigned long kBaud = 115200;
constexpr size_t kMaxJpegBytes = 64 * 1024;

// Include OTA credentials if available; checks sketch folder then parent arduino/ folder.
#if __has_include("OtaSecrets.h")
#include "OtaSecrets.h"
#elif __has_include("../OtaSecrets.h")
#include "../OtaSecrets.h"
#else
constexpr char kOtaWifiSsid[] = "";
constexpr char kOtaWifiPassword[] = "";
constexpr char kOtaPassword[] = "";
#endif

// Call from setup() for boards that use OTA Wi-Fi credentials (camera, sensor node).
inline void beginOtaWifi() {
  if (kOtaWifiSsid[0] != '\0' && kOtaPassword[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(kOtaWifiSsid, kOtaWifiPassword);
  }
}

// Call from loop(). Returns true once OTA is active.
inline bool handleOta(bool &started, const char *hostname) {
  if (!started && kOtaPassword[0] != '\0' && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.setPassword(kOtaPassword);
    ArduinoOTA.begin();
    started = true;
    Serial.printf("OTA READY %s at ", hostname);
    Serial.println(WiFi.localIP());
  }
  if (started) {
    ArduinoOTA.handle();
  }
  return started;
}
