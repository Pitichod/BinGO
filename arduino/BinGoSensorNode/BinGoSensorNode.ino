#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#if __has_include("OtaSecrets.h")
#include "OtaSecrets.h"
#else
constexpr char kOtaWifiSsid[] = "";
constexpr char kOtaWifiPassword[] = "";
constexpr char kOtaPassword[] = "";
#endif

constexpr unsigned long kBaud = 115200;
constexpr size_t kMaxJpegBytes = 64 * 1024;
constexpr int kCameraRxPin = 27;
constexpr int kCameraTxPin = 26;
constexpr int kGatewayRxPin = 33;
constexpr int kGatewayTxPin = 32;
constexpr unsigned int kTestWeightGrams = 42;

uint8_t jpeg[kMaxJpegBytes];
bool otaStarted = false;

void setup() {
  Serial.begin(kBaud);
  Serial1.setRxBufferSize(4096);
  Serial1.begin(kBaud, SERIAL_8N1, kCameraRxPin, kCameraTxPin);
  Serial1.setTimeout(8000);
  Serial2.begin(kBaud, SERIAL_8N1, kGatewayRxPin, kGatewayTxPin);
  Serial2.setTimeout(2000);
  Serial.println("NODE READY. Enter p to ping gateway or c to capture.");

  if (kOtaWifiSsid[0] != '\0' && kOtaPassword[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(kOtaWifiSsid, kOtaWifiPassword);
  }
}

void captureAndForward() {
  // Drop unread data left by an interrupted capture before requesting another.
  while (Serial1.available()) {
    Serial1.read();
  }
  Serial1.write('C');
  String header = Serial1.readStringUntil('\n');
  unsigned int imageSize = 0;
  unsigned long expectedCrc = 0;
  if (sscanf(header.c_str(), "JPEG %u %lx", &imageSize, &expectedCrc) != 2 ||
      imageSize < 4 || imageSize > sizeof(jpeg)) {
    Serial.printf("CAM HEADER ERROR [%s]\n", header.c_str());
    return;
  }

  size_t received = Serial1.readBytes(jpeg, imageSize);
  if (received != imageSize) {
    Serial.printf("CAM TIMEOUT %u/%u bytes\n", static_cast<unsigned int>(received),
                  imageSize);
    return;
  }
  if (esp_rom_crc32_le(0, jpeg, imageSize) != expectedCrc) {
    Serial.println("CAM CRC ERROR");
    return;
  }

  Serial.printf("CAM OK %u bytes. Forwarding with test weight %u g.\n",
                imageSize, kTestWeightGrams);
  Serial2.printf("ITEM %u %08lx %u\n", imageSize, expectedCrc, kTestWeightGrams);
  Serial2.write(jpeg, imageSize);
  Serial2.flush();
  // Allow the gateway to report its own timeout if bytes were lost.
  Serial2.setTimeout(9000);
  String reply = Serial2.readStringUntil('\n');
  reply.trim();
  Serial.println(reply.isEmpty() ? "GATEWAY TIMEOUT" : reply.c_str());
  if (reply == "GATEWAY OK") {
    Serial2.setTimeout(75000);
    String apiReply = Serial2.readStringUntil('\n');
    apiReply.trim();
    Serial.println(apiReply.isEmpty() ? "API TIMEOUT" : apiReply.c_str());
  }
  Serial2.setTimeout(2000);
}

void loop() {
  if (!otaStarted && kOtaPassword[0] != '\0' && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname("bingo-sensor-node");
    ArduinoOTA.setPassword(kOtaPassword);
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.print("OTA READY bingo-sensor-node at ");
    Serial.println(WiFi.localIP());
  }
  if (otaStarted) {
    ArduinoOTA.handle();
  }

  switch (Serial.read()) {
    case 'p': {
      Serial2.println("PING");
      String reply = Serial2.readStringUntil('\n');
      reply.trim();
      Serial.println(reply.isEmpty() ? "GATEWAY TIMEOUT" : reply.c_str());
      break;
    }
    case 'c':
      captureAndForward();
      break;
    default:
      delay(1);
      break;
  }
}
