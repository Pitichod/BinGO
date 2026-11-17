#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <esp_now.h>
#include "Weight.h"


#if __has_include("OtaSecrets.h")
#include <ArduinoOTA.h>
#include "OtaSecrets.h"

constexpr char otaHostname[] = "bingo-sensor-node";
bool otaReady = false;
#endif

#if __has_include("EspNowPeer.h")
#include "EspNowPeer.h"
#else
constexpr uint8_t peerMac[6] = {};
#endif

bool espNowReady = false;
std::atomic<uint32_t> receivedPing{0};
std::atomic<uint16_t> sortRequest{0};
constexpr uint8_t distanceSensorCount = 2;
constexpr uint8_t trigPins[] = {18, 19, 21, 22};
constexpr uint8_t echoPins[] = {34, 35, 36, 39};
static_assert(distanceSensorCount >= 2 && distanceSensorCount <= 4, "Enable 2 to 4 distance sensors");
constexpr uint8_t rotatePin = 25;
constexpr uint8_t tiltPin = 26;
constexpr uint8_t servoPwmBits = 14;
constexpr unsigned long servoTestMs = 600;





// Servo calibration in degrees (servo commands, not bin bearings).
constexpr float rotateAxisDeg[] = {85, 162}; // Home / bins 1 and 3, then bins 2 and 4.
constexpr float tiltLevelDeg = 90;
constexpr float dumpTiltDeg = 90;
constexpr float rotateOffsetDeg = 0;
constexpr float tiltOffsetDeg = 0;
constexpr float testAnglesDeg[] = {72, 108}; // Lowercase and uppercase test commands.




constexpr float tiltSideDeg[] = {tiltLevelDeg - dumpTiltDeg, tiltLevelDeg + dumpTiltDeg};
static_assert(dumpTiltDeg >= 0 && tiltSideDeg[0] + tiltOffsetDeg >= 0 &&
              tiltSideDeg[1] + tiltOffsetDeg <= 180, "Dump tilt must fit the 0-180 degree range");
constexpr unsigned long servoStepMs = 800;

constexpr unsigned maxDumpRetries = 2;  // Extra dumps after the first attempt. Use 0 to disable.
constexpr unsigned long dumpVerifyMs = 2500;

enum class ServoPhase { Idle, Test, Level, Rotate, Dump, Return, Home, Verify };
enum class DumpStatus : uint32_t { Normal, Retrying, ItemStuck, CheckScale, ServoError };
ServoPhase servoPhase = ServoPhase::Idle;
DumpStatus dumpStatus = DumpStatus::Normal;
unsigned dumpRetries = 0;
bool rotateAttached = false;
bool tiltAttached = false;
unsigned targetBin = 0;
unsigned long servoStarted = 0;

void stopServo() {
  if (rotateAttached) ledcDetach(rotatePin);
  if (tiltAttached) ledcDetach(tiltPin);
  pinMode(rotatePin, OUTPUT);
  pinMode(tiltPin, OUTPUT);
  digitalWrite(rotatePin, LOW);
  digitalWrite(tiltPin, LOW);
  rotateAttached = tiltAttached = false;
  servoPhase = ServoPhase::Idle;
}

void cancelDump() {
  stopServo();
  if (dumpStatus == DumpStatus::Retrying) dumpStatus = DumpStatus::Normal;
}

bool setServoAngle(uint8_t pin, float baseAngleDeg) {
  const float angleDeg = baseAngleDeg + (pin == rotatePin ? rotateOffsetDeg : tiltOffsetDeg);
  if (!(angleDeg >= 0 && angleDeg <= 180)) {
    if (servoPhase != ServoPhase::Idle) dumpStatus = DumpStatus::ServoError;
    stopServo();
    Serial.printf("Servo GPIO%u: %.1f deg is outside 0-180 deg\n",
                  static_cast<unsigned>(pin), angleDeg);
    return false;
  }
  const uint32_t pulseUs = 1000 + static_cast<uint32_t>(angleDeg * 1000 / 180 + 0.5f);
  bool &attached = pin == rotatePin ? rotateAttached : tiltAttached;
  if (!attached) attached = ledcAttach(pin, 50, servoPwmBits);
  const uint32_t duty = (pulseUs * (1UL << servoPwmBits) + 10000) / 20000;
  if (attached && ledcWrite(pin, duty)) {
    Serial.printf("Servo GPIO%u: %.1f deg\n", static_cast<unsigned>(pin), angleDeg);
    return true;
  }
  if (servoPhase != ServoPhase::Idle) dumpStatus = DumpStatus::ServoError;
  stopServo();
  Serial.println("Servo PWM failed");
  return false;
}

void testServo(uint8_t pin, float angleDeg) {
  if (servoPhase != ServoPhase::Idle) return;
  if (setServoAngle(pin, angleDeg)) {
    servoPhase = ServoPhase::Test;
    servoStarted = millis();
  }
}

bool runDumpCycle() {
  if (setServoAngle(tiltPin, tiltLevelDeg)) {
    servoPhase = ServoPhase::Level;
    servoStarted = millis();
    Serial.printf("Bin %u: target %u deg\n", targetBin + 1, targetBin * 90);
    return true;
  }
  return false;
}

bool startDump(unsigned binIndex) {
  if (servoPhase != ServoPhase::Idle) {
    Serial.println("Servo busy; wait or send x");
    return false;
  }
  targetBin = binIndex;
  dumpRetries = 0;
  if (!runDumpCycle()) {
    dumpStatus = DumpStatus::ServoError;
    return false;
  }
  dumpStatus = DumpStatus::Normal;
  return true;
}

void verifyDump() {
  const int32_t grams = readWeightGrams();
  if (trayIsEmpty(grams)) {
    servoPhase = ServoPhase::Idle;
    dumpStatus = DumpStatus::Normal;
    Serial.println("Dump verified: tray empty");
    return;
  }
  if (millis() - servoStarted < dumpVerifyMs) return;

  if (grams == INT32_MIN || grams <= emptyTrayMaxGrams || millis() - lastWeightSample >= 500) {
    dumpStatus = DumpStatus::CheckScale;
    Serial.println("Dump could not be verified: check tray and scale");
  } else if (dumpRetries < maxDumpRetries) {
    ++dumpRetries;
    dumpStatus = DumpStatus::Retrying;
    Serial.printf("Dump retry %u/%u to Bin %u: %ld g remains\n", dumpRetries, maxDumpRetries,
                  targetBin + 1, static_cast<long>(grams));
    if (runDumpCycle()) return;
  } else {
    dumpStatus = DumpStatus::ItemStuck;
    Serial.printf("Dump failed after %u retries: %ld g remains; clear tray\n",
                  dumpRetries, static_cast<long>(grams));
  }
  servoPhase = ServoPhase::Idle;
}

void updateServo() {
  if (servoPhase == ServoPhase::Idle) return;
  if (servoPhase == ServoPhase::Verify) {
    verifyDump();
    return;
  }
  const unsigned long wait = servoPhase == ServoPhase::Test ? servoTestMs : servoStepMs;
  if (millis() - servoStarted < wait) return;

  switch (servoPhase) {
    case ServoPhase::Level:
      if (setServoAngle(rotatePin, rotateAxisDeg[targetBin % 2])) servoPhase = ServoPhase::Rotate;
      break;
    case ServoPhase::Rotate:
      if (setServoAngle(tiltPin, tiltSideDeg[targetBin / 2])) servoPhase = ServoPhase::Dump;
      break;
    case ServoPhase::Dump:
      if (setServoAngle(tiltPin, tiltLevelDeg)) servoPhase = ServoPhase::Return;
      break;
    case ServoPhase::Return:
      if (setServoAngle(rotatePin, rotateAxisDeg[0])) servoPhase = ServoPhase::Home;
      break;
    case ServoPhase::Home:
      stopServo();
      servoPhase = ServoPhase::Verify;
      Serial.println("Home 0 deg: checking tray weight");
      break;
    case ServoPhase::Test:
      stopServo();
      break;
    case ServoPhase::Idle:
    case ServoPhase::Verify:
      break;
  }
  servoStarted = millis();
}

struct SensorReply {
  uint32_t token;
  uint16_t distanceMm[4];
  int32_t weightGrams;
  uint32_t readyItemId;
  DumpStatus dumpStatus;
};
static_assert(sizeof(SensorReply) == 24, "ESP-NOW reply size");

uint16_t measureDistance(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  const unsigned long duration = pulseIn(echo, HIGH, 25000);
  delay(60);  // Give echoes time to decay before firing another transducer.
  const unsigned long mm = (duration * 10 + 29) / 58;
  return mm >= 20 && mm <= 4000 ? static_cast<uint16_t>(mm) : 0;
}

void receiveGatewayMessage(const esp_now_recv_info_t *info, const uint8_t *data, int length) {
  if (memcmp(info->src_addr, peerMac, sizeof(peerMac)) != 0) return;
  if (length == 2) {
    if (data[0] && data[1] >= 1 && data[1] <= 4) {
      sortRequest.store((uint16_t(data[0]) << 8) | data[1]);
    }
    return;
  }
  if (length != static_cast<int>(sizeof(uint32_t))) return;
  uint32_t token;
  memcpy(&token, data, sizeof(token));
  receivedPing.store(token);
}

void startEspNow() {
  static bool attempted = false;
  if (attempted || WiFi.status() != WL_CONNECTED) return;
  attempted = true;
  Serial.printf("ESP-NOW STA MAC %s, channel %d\n", WiFi.macAddress().c_str(), WiFi.channel());
  const uint8_t unsetMac[6] = {};
  if (memcmp(peerMac, unsetMac, sizeof(peerMac)) == 0) {
    Serial.println("Set the other board's MAC in EspNowPeer.h");
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, sizeof(peerMac));
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0;  // Follow the channel selected by the Wi-Fi network.
  esp_err_t result = esp_now_init();
  if (result == ESP_OK) result = esp_now_add_peer(&peer);
  if (result == ESP_OK) result = esp_now_register_recv_cb(receiveGatewayMessage);
  espNowReady = result == ESP_OK;
  Serial.printf("ESP-NOW init: %s\n", esp_err_to_name(result));
}

void setup() {
  Serial.begin(115200);
  beginWeight();
  pinMode(rotatePin, OUTPUT);
  digitalWrite(rotatePin, LOW);
  pinMode(tiltPin, OUTPUT);
  digitalWrite(tiltPin, LOW);
  Serial.println("Servo test: r/R rotate, t/T tilt, x stop PWM");
  Serial.println("Servo sequence: 1-4 select bin");
  for (uint8_t i = 0; i < distanceSensorCount; ++i) {
    pinMode(trigPins[i], OUTPUT);
    digitalWrite(trigPins[i], LOW);
    pinMode(echoPins[i], INPUT);
  }
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
  updateServo();
  if (servoPhase == ServoPhase::Idle || servoPhase == ServoPhase::Verify) updateWeight();
  else {
    sampleCount = 0;
    itemTrigger.pause();
  }
  if (servoPhase == ServoPhase::Idle &&
      (dumpStatus == DumpStatus::ItemStuck || dumpStatus == DumpStatus::CheckScale) &&
      trayIsEmpty(readWeightGrams())) {
    dumpStatus = DumpStatus::Normal;
  }
  const int command = Serial.read();
  if (command >= '1' && command <= '4') startDump(command - '1');
  switch (command) {
    case 'r': testServo(rotatePin, testAnglesDeg[0]); break;
    case 'R': testServo(rotatePin, testAnglesDeg[1]); break;
    case 't': testServo(tiltPin, testAnglesDeg[0]); break;
    case 'T': testServo(tiltPin, testAnglesDeg[1]); break;
    case 'x': cancelDump(); break;
    case 'w':
    case 'z':
    case 'k':
      if (servoPhase == ServoPhase::Idle) weightCommand(command);
      else Serial.println("Wait for the servo cycle to finish");
      break;
  }
#if __has_include("OtaSecrets.h")
  if (wifiSsid[0] && otaPassword[0] && WiFi.status() == WL_CONNECTED) {
    if (!otaReady) {
      ArduinoOTA.setHostname(otaHostname);
      ArduinoOTA.setPassword(otaPassword);
      ArduinoOTA.onStart(cancelDump);
      ArduinoOTA.begin();
      otaReady = true;
      Serial.printf("OTA READY %s at ", otaHostname);
      Serial.println(WiFi.localIP());
    }
    ArduinoOTA.handle();
  }
#endif
  startEspNow();
  const uint16_t request = sortRequest.exchange(0);
  if (request && espNowReady && WiFi.status() == WL_CONNECTED) {
    const uint8_t bin = request & 0xff;
    Serial.printf("ESP-NOW sort request: Bin %u\n", static_cast<unsigned>(bin));
    uint8_t status;
    if (servoPhase != ServoPhase::Idle) status = 2;
    else status = startDump(bin - 1) ? 1 : 4;
    const uint8_t reply[] = {static_cast<uint8_t>(request >> 8), status};
    const esp_err_t result = esp_now_send(peerMac, reply, sizeof(reply));
    if (result != ESP_OK) Serial.printf("Sort reply failed: %s\n", esp_err_to_name(result));
  }
  const uint32_t token = receivedPing.exchange(0);
  if (token && espNowReady && WiFi.status() == WL_CONNECTED) {
    SensorReply reply{token, {UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX},
                      INT32_MIN, 0, dumpStatus};
    for (uint8_t i = 0; i < distanceSensorCount; ++i) {
      reply.distanceMm[i] = measureDistance(trigPins[i], echoPins[i]);
    }
    if (servoPhase == ServoPhase::Idle || servoPhase == ServoPhase::Verify) {
      updateWeight();
      reply.weightGrams = readWeightGrams();
      if (servoPhase == ServoPhase::Idle && dumpStatus == DumpStatus::Normal &&
          reply.weightGrams != INT32_MIN && millis() - lastWeightSample < 500) {
        reply.readyItemId = itemTrigger.readyItem();
      }
    }
    const esp_err_t result =
        esp_now_send(peerMac, reinterpret_cast<const uint8_t *>(&reply), sizeof(reply));
    if (result != ESP_OK) Serial.printf("ESP-NOW reply: %s\n", esp_err_to_name(result));
  }
}
