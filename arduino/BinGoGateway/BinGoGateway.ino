#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_now.h>
#include <esp_random.h>
#include "Yolo.h"
#include "BinClasses.h"
#include "GatewayDisplay.h"
#include "EnvironmentSensors.h"

#if __has_include("OtaSecrets.h")
#include <ArduinoOTA.h>
#include "OtaSecrets.h"

constexpr char otaHostname[] = "bingo-gateway";
bool otaReady = false;
#endif

#if __has_include("EspNowPeer.h")
#include "EspNowPeer.h"
#else
constexpr uint8_t peerMac[6] = {};
#endif

bool espNowReady = false;
enum class DumpStatus : uint32_t { Normal, Retrying, ItemStuck, CheckScale, ServoError };
struct SensorReply {
  uint32_t token;
  uint16_t distanceMm[4];
  int32_t weightGrams;
  uint32_t readyItemId;
  DumpStatus dumpStatus;
};
static_assert(sizeof(SensorReply) == 24, "ESP-NOW reply size");
QueueHandle_t sensorReplies = nullptr;
uint32_t pendingPing = 0;
unsigned long pingStarted = 0;
bool showPingResult = false;
bool wifiWasOnline = false;
bool nodeChecked = false;
bool nodeOnline = false;
unsigned long lastNodeProbe = 0;
std::atomic<uint16_t> sortReply{0};
uint8_t sortSequence = 0;
uint8_t awaitingSort = 0;
unsigned long sortSentAt = 0;
bool yoloPending = false;
unsigned long yoloRequestedAt = 0;
uint32_t lastCapturedItem = 0;
DumpStatus nodeDumpStatus = DumpStatus::Normal;

void reportDumpStatus(DumpStatus status);

void receivePing(const esp_now_recv_info_t *info, const uint8_t *data, int length) {
  if (memcmp(info->src_addr, peerMac, sizeof(peerMac)) != 0) return;
  if (length == 2) sortReply.store((uint16_t(data[0]) << 8) | data[1]);
  else if (length == static_cast<int>(sizeof(SensorReply))) xQueueOverwrite(sensorReplies, data);
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
  sensorReplies = xQueueCreate(1, sizeof(SensorReply));
  if (!sensorReplies) {
    Serial.println("ESP-NOW: could not allocate sensor reply queue");
    return;
  }
  esp_err_t result = esp_now_init();
  if (result == ESP_OK) result = esp_now_add_peer(&peer);
  if (result == ESP_OK) result = esp_now_register_recv_cb(receivePing);
  espNowReady = result == ESP_OK;
  Serial.printf("ESP-NOW init: %s\n", esp_err_to_name(result));
  if (!espNowReady) showLcd("ESP-NOW", "Startup error");
}

void pingSensorNode(bool showResult) {
  if (!espNowReady || WiFi.status() != WL_CONNECTED) {
    if (showResult) Serial.println("Sensor node ESP-NOW link is not ready");
    return;
  }
  if (pendingPing) {
    showPingResult |= showResult;
    return;
  }
  lastNodeProbe = millis();
  const uint32_t token = esp_random() | 1;
  const esp_err_t result =
      esp_now_send(peerMac, reinterpret_cast<const uint8_t *>(&token), sizeof(token));
  if (result != ESP_OK) {
    nodeChecked = true;
    nodeOnline = false;
    if (showResult) Serial.printf("ESP-NOW send: %s\n", esp_err_to_name(result));
    return;
  }
  pendingPing = token;
  pingStarted = millis();
  showPingResult = showResult;
}

void updateHomeStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    nodeChecked = nodeOnline = false;
    pendingPing = 0;
    setLcdHomeStatus(wifiWasOnline ? "Wi-Fi lost" : "Joining Wi-Fi...");
    return;
  }
  wifiWasOnline = true;
  if (!espNowReady) setLcdHomeStatus("Check node link");
  else if (!nodeChecked) setLcdHomeStatus("Looking for node");
  else if (!nodeOnline) setLcdHomeStatus("Node offline");
  else if (nodeDumpStatus == DumpStatus::Retrying) setLcdHomeStatus("Retrying dump...");
  else if (nodeDumpStatus == DumpStatus::ItemStuck) setLcdHomeStatus("Clear the tray");
  else if (nodeDumpStatus == DumpStatus::CheckScale) setLcdHomeStatus("Check the scale");
  else if (nodeDumpStatus == DumpStatus::ServoError) setLcdHomeStatus("Check the servo");
  else if (!tlsClockReady()) setLcdHomeStatus("Syncing time...");
  else setLcdHomeStatus("Ready to sort");
}

void reportDumpStatus(DumpStatus status) {
  if (status == nodeDumpStatus) return;
  nodeDumpStatus = status;
  switch (status) {
    case DumpStatus::Retrying:
      Serial.println("Dump: retrying the same bin");
      break;
    case DumpStatus::ItemStuck:
      Serial.println("ALERT: item still on tray after dump retries");
      showLcd("Item stuck", "Clear the tray");
      break;
    case DumpStatus::CheckScale:
      Serial.println("ALERT: could not verify weight after dump");
      showLcd("Dump unverified", "Check tray/scale");
      break;
    case DumpStatus::ServoError:
      Serial.println("ALERT: servo cycle could not complete");
      showLcd("Dump stopped", "Check the servo");
      break;
    case DumpStatus::Normal:
      break;
  }
}

void sendSort(uint8_t bin) {
  if (!espNowReady || WiFi.status() != WL_CONNECTED) {
    Serial.println("Sort skipped: Sensor node ESP-NOW link is not ready");
    return;
  }
  if (awaitingSort) {
    Serial.println("Sort: waiting for previous command reply");
    return;
  }
  if (++sortSequence == 0) ++sortSequence;
  const uint8_t request[] = {sortSequence, bin};
  sortReply.store(0);
  const esp_err_t result = esp_now_send(peerMac, request, sizeof(request));
  if (result != ESP_OK) {
    Serial.printf("Sort send failed: %s\n", esp_err_to_name(result));
    return;
  }
  awaitingSort = sortSequence;
  sortSentAt = millis();
  Serial.printf("Sort: Bin %u command queued, waiting for Sensor node\n", static_cast<unsigned>(bin));
}

void sortYoloResult(const String &label) {
  if (label.isEmpty()) {
    showLcd("YOLO", "No valid class");
    Serial.println("Sort skipped: no valid class");
    return;
  }
  showLcd("Detected", label.c_str());
  const uint8_t bin = binForClass(label.c_str());
  if (!bin) {
    Serial.printf("Sort skipped: no unique bin for %s\n", label.c_str());
    return;
  }
  Serial.printf("Sort %s -> Bin %u\n", label.c_str(), static_cast<unsigned>(bin));
  sendSort(bin);
}

void classifyItem() {
  setLcdHomeStatus("Reading item...");
  showLcd("YOLO", "Classifying...");
  sortYoloResult(runYolo(Serial1));
}

void showEnvironmentSensors() {
  char top[17];
  char bottom[17];
  if (isfinite(temperatureC) && isfinite(humidityPercent)) {
    snprintf(top, sizeof(top), "T:%.0fC H:%.0f%%", temperatureC, humidityPercent);
    Serial.printf("DHT11: %.1f C, %.1f %% RH\n", temperatureC, humidityPercent);
  } else {
    snprintf(top, sizeof(top), "DHT11: --");
    Serial.println("DHT11: unavailable");
  }
  if (isfinite(lightLux)) {
    snprintf(bottom, sizeof(bottom), "Light: %.0f lx", lightLux);
    Serial.printf("BH1750: %.1f lx\n", lightLux);
  } else {
    snprintf(bottom, sizeof(bottom), "Light: --");
    Serial.println("BH1750: unavailable");
  }
  showLcd(top, bottom);
}

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
  const bool i2cReady = Wire.begin(21, 22, 100000);
  if (!i2cReady) Serial.println("I2C startup failed");
  beginEnvironmentSensors(i2cReady);
  if (i2cReady) beginLcd();
  Serial1.setRxBufferSize(4096);
  Serial1.begin(115200, SERIAL_8N1, 27, 26);
  Serial1.setTimeout(6000);
  Serial.println("n: Read bin distances, c: Camera UART, y: Capture and run YOLO");
  Serial.println("s: Test Bin 1 servo cycle over ESP-NOW");
  Serial.println("e: Temperature, humidity and light");
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
  startTlsClockSync();
  updateEnvironmentSensors();
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
  startEspNow();
  if (awaitingSort) {
    const uint16_t reply = sortReply.exchange(0);
    if ((reply >> 8) == awaitingSort) {
      switch (reply & 0xff) {
        case 1: Serial.println("Sort: Sensor node started servo cycle"); break;
        case 2: Serial.println("Sort rejected: servo busy"); break;
        case 3: Serial.println("Sort rejected: bin is reserved"); break;
        case 4: Serial.println("Sort rejected: servo PWM failed"); break;
        default: Serial.println("Sort: unexpected reply"); break;
      }
      awaitingSort = 0;
    } else if (millis() - sortSentAt >= 1000) {
      Serial.println("Sort: no command reply from Sensor node; check firmware and node log");
      awaitingSort = 0;
    }
  }
  SensorReply reply;
  if (pendingPing && xQueueReceive(sensorReplies, &reply, 0) == pdTRUE &&
      reply.token == pendingPing) {
    nodeChecked = nodeOnline = true;
    if (showPingResult) {
      Serial.println("Sensor node OK (ESP-NOW)");
      showMeasurements(reply.weightGrams, reply.distanceMm);
      if (reply.weightGrams == INT32_MIN) Serial.println("Weight: unavailable");
      else Serial.printf("Weight: %ld g\n", static_cast<long>(reply.weightGrams));
      for (unsigned i = 0; i < 4; ++i) {
        const uint16_t mm = reply.distanceMm[i];
        if (mm == UINT16_MAX) continue;
        if (mm == 0) {
          Serial.printf("Bin %u: invalid echo\n", i + 1);
        } else {
          Serial.printf("Bin %u: %.1f cm\n", i + 1, mm / 10.0);
        }
      }
    }
    reportDumpStatus(reply.dumpStatus);
    pendingPing = 0;
    if (reply.readyItemId && reply.readyItemId != lastCapturedItem &&
        millis() - pingStarted < 1000 && tlsClockReady() &&
        WiFi.status() == WL_CONNECTED && !awaitingSort) {
      lastCapturedItem = reply.readyItemId;
      yoloPending = false;
      Serial.printf("YOLO auto: stable item (%ld g)\n", static_cast<long>(reply.weightGrams));
      classifyItem();
    }
  } else if (pendingPing && millis() - pingStarted >= 1000) {
    nodeChecked = true;
    nodeOnline = false;
    if (showPingResult) {
      showLcd("Sensor node", "No reply");
      Serial.println("Sensor node ESP-NOW TIMEOUT");
    }
    pendingPing = 0;
  }
  updateHomeStatus();
  if (yoloPending) {
    if (WiFi.status() != WL_CONNECTED || millis() - yoloRequestedAt >= 12000) {
      yoloPending = false;
      Serial.println("YOLO canceled: clock sync timed out or Wi-Fi disconnected");
    } else if (tlsClockReady() && !awaitingSort) {
      yoloPending = false;
      classifyItem();
    }
  }
  switch (Serial.read()) {
    case 'e':
      showEnvironmentSensors();
      break;
    case 's':
      sendSort(1);
      break;
    case 'y':
      if (yoloPending || awaitingSort) break;
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("YOLO: Wi-Fi is disconnected");
      } else if (tlsClockReady()) {
        classifyItem();
      } else {
        yoloPending = true;
        yoloRequestedAt = millis();
        showLcd("YOLO", "Syncing clock...");
        Serial.println("YOLO queued: starting automatically when the clock is ready");
      }
      break;
    case 'n':
      pingSensorNode(true);
      break;
    case 'c':
      checkLink(Serial1, 'C', "Camera");
      break;
  }
  if (!pendingPing && !awaitingSort && millis() - lastNodeProbe >= 1000) {
    pingSensorNode(false);
  }
}
