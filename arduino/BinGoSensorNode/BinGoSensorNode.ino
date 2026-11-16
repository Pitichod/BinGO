#include <Arduino.h>

#if __has_include("OtaSecrets.h")
#include <ArduinoOTA.h>
#include <WiFi.h>
#include "OtaSecrets.h"

constexpr char otaHostname[] = "bingo-sensor-node";
bool otaReady = false;
#endif

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 33, 32);
  Serial.println("Sensor node UART ready");
#if __has_include("OtaSecrets.h")
  if (wifiSsid[0] && otaPassword[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid, wifiPassword);
    Serial.println("Connecting to Wi-Fi for OTA");
  } else {
    Serial.println("Set Wi-Fi SSID and OTA password in OtaSecrets.h");
  }
#else
  Serial.println("Wi-Fi disabled: OtaSecrets.h missing");
#endif
}

void loop() {
#if __has_include("OtaSecrets.h")
  if (wifiSsid[0] && otaPassword[0] && WiFi.status() == WL_CONNECTED) {
    if (!otaReady) {
      ArduinoOTA.setHostname(otaHostname);
      ArduinoOTA.setPassword(otaPassword);
      ArduinoOTA.begin();
      otaReady = true;
      Serial.printf("OTA READY %s at ", otaHostname);
      Serial.println(WiFi.localIP());
    }
    ArduinoOTA.handle();
  }
#endif
  if (Serial2.read() == '?') {
    Serial2.write('N');
    Serial.println("Ping received");
  }
}
