#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <algorithm>
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

struct BinSensor {
  int trigPin;
  int echoPin;
  float emptyCm;
  float fullCm;
};

constexpr size_t kBinCount = 2;
constexpr BinSensor kBins[] = {
    {18, 34, 0.0f, 5.0f},
    {19, 35, 0.0f, 5.0f},
    {21, 36, 0.0f, 5.0f},
    {22, 39, 0.0f, 5.0f},
};
static_assert(kBinCount >= 2 && kBinCount <= sizeof(kBins) / sizeof(kBins[0]),
              "Enable 2 to 4 bin sensors");
constexpr unsigned long kEchoTimeoutUs = 25000;
constexpr unsigned long kPingSpacingMs = 70;
constexpr float kMinDistanceCm = 2.0f;
constexpr float kMaxDistanceCm = 400.0f;
constexpr unsigned long kDistanceReportIntervalMs = 30000;

uint8_t jpeg[kMaxJpegBytes];
bool otaStarted = false;
unsigned long lastDistanceReportMs = 0;

void setup() {
  Serial.begin(kBaud);
  Serial1.setRxBufferSize(4096);
  Serial1.begin(kBaud, SERIAL_8N1, kCameraRxPin, kCameraTxPin);
  Serial1.setTimeout(8000);
  Serial2.begin(kBaud, SERIAL_8N1, kGatewayRxPin, kGatewayTxPin);
  Serial2.setTimeout(2000);
  for (size_t i = 0; i < kBinCount; ++i) {
    pinMode(kBins[i].trigPin, OUTPUT);
    digitalWrite(kBins[i].trigPin, LOW);
    pinMode(kBins[i].echoPin, INPUT);
  }
  Serial.println("NODE READY. Enter p to ping gateway, c to capture, or f to measure bin room.");

  if (kOtaWifiSsid[0] != '\0' && kOtaPassword[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(kOtaWifiSsid, kOtaWifiPassword);
  }
}

float measureBinDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  const unsigned long echoUs = pulseIn(echoPin, HIGH, kEchoTimeoutUs);
  // Give echoes time to die out before triggering this or another sensor.
  delay(kPingSpacingMs);
  return echoUs / 58.0f;
}

void reportBinRoom() {
  lastDistanceReportMs = millis();
  // Keep four UART fields; zero marks an unavailable or disabled sensor.
  float distancesCm[4] = {};
  for (size_t i = 0; i < kBinCount; ++i) {
    const BinSensor &bin = kBins[i];
    float samples[3];
    size_t valid = 0;
    for (size_t j = 0; j < sizeof(samples) / sizeof(samples[0]); ++j) {
      const float cm = measureBinDistance(bin.trigPin, bin.echoPin);
      if (cm >= kMinDistanceCm && cm <= kMaxDistanceCm) {
        samples[valid++] = cm;
      }
    }

    Serial.printf("BIN %u: ", static_cast<unsigned int>(i + 1));
    if (valid < 2) {
      Serial.println("room unknown (no reliable echo)");
      continue;
    }

    std::sort(samples, samples + valid);
    const float distanceCm = (samples[(valid - 1) / 2] + samples[valid / 2]) / 2.0f;
    distancesCm[i] = distanceCm;
    if (bin.fullCm < kMinDistanceCm || bin.emptyCm > kMaxDistanceCm ||
        bin.emptyCm <= bin.fullCm) {
      Serial.printf("%.1f cm clearance (calibration needed)\n", distanceCm);
      continue;
    }

    const float remaining = 100.0f * (distanceCm - bin.fullCm) / (bin.emptyCm - bin.fullCm);
    Serial.printf("%.0f%% room left (%.1f cm clearance)\n",
                  constrain(remaining, 0.0f, 100.0f), distanceCm);
  }
  Serial2.printf("DIST %.2f %.2f %.2f %.2f\n",
                 distancesCm[0], distancesCm[1], distancesCm[2], distancesCm[3]);
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
    case 'f':
      reportBinRoom();
      break;
    default:
      if (millis() - lastDistanceReportMs >= kDistanceReportIntervalMs) {
        reportBinRoom();
      } else {
        delay(1);
      }
      break;
  }
}
