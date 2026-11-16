#include <Arduino.h>
#include "Yolo.h"

#if __has_include("OtaSecrets.h")
#include <ArduinoOTA.h>
#include <WiFi.h>
#include "OtaSecrets.h"

constexpr char otaHostname[] = "bingo-gateway";
bool otaReady = false;
#endif

void checkLink(HardwareSerial &link, char expected, const char *name) {
  while (link.read() >= 0) {
    // Discard bytes left over from a previous attempt.
  }
  link.write('?');

  const unsigned long started = millis();
  while (millis() - started < 1000) {
    if (link.read() == expected) {
      Serial.print(name);
      Serial.println(" OK");
      return;
    }
    delay(1);
  }

  Serial.print(name);
  Serial.println(" TIMEOUT");
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 33, 32);
  Serial1.setRxBufferSize(4096);
  Serial1.begin(115200, SERIAL_8N1, 27, 26);
  Serial1.setTimeout(6000);
  Serial.println("n: Sensor node, c: Camera link, y: Capture and run YOLO");
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
  switch (Serial.read()) {
    case 'y':
      runYolo(Serial1);
      break;
    case 'n':
      checkLink(Serial2, 'N', "Sensor node");
      break;
    case 'c':
      checkLink(Serial1, 'C', "Camera");
      break;
  }
}
