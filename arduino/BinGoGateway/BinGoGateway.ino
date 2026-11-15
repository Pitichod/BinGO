#include "../BinGoCommon.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <atomic>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_rom_crc.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <time.h>

#include "Mpu6050Diagnostic.h"

#if __has_include("Secrets.h")
#include "Secrets.h"
#else
#include "Secrets.example.h"
#endif

#if __has_include("BlynkSecrets.h")
#include "BlynkSecrets.h"
#else
#include "BlynkSecrets.example.h"
#endif

#if defined(BLYNK_TEMPLATE_ID) && defined(BLYNK_TEMPLATE_NAME)
#define BINGO_BLYNK_CAMERA_CONTROL
#include <BlynkSimpleEsp32_SSL.h>
#endif

#if defined(BINGO_BLYNK_CAMERA_CONTROL) && defined(BLYNK_PHOTO_URL)
#include <WebServer.h>
#endif

// ── Constants ────────────────────────────────────────────────────────────────

constexpr bool kMpuDiagnosticMode = false;
constexpr size_t kMaxResponseBytes = 8 * 1024;
constexpr int kNodeRxPin = 33;
constexpr int kNodeTxPin = 32;
constexpr int kLcdSdaPin = 21;
constexpr int kLcdSclPin = 22;
constexpr uint8_t kLcdColumns = 16;
constexpr uint8_t kLcdRows = 2;
constexpr uint8_t kLcdAddresses[] = {0x27, 0x3F};
constexpr unsigned long kLcdResultDurationMs = 5000;
constexpr int kLightSdaPin = 18;
constexpr int kLightSclPin = 19;
constexpr uint8_t kBh1750Address = 0x23;
constexpr uint8_t kBh1750ContinuousHighResolution = 0x10;
constexpr unsigned long kLightReadIntervalMs = 1000;
constexpr unsigned long kLightPublishIntervalMs = 30000;
constexpr unsigned long kCaptureTimeoutMs = 25000;
constexpr time_t kEarliestTlsTime = 1704067200;  // 2024-01-01 UTC

// ── Multipart body layout ────────────────────────────────────────────────────

constexpr char kMultipartHead[] =
    "--BinGoPhotoBoundary\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"bingo.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
constexpr char kMultipartTail[] = "\r\n--BinGoPhotoBoundary--\r\n";
constexpr size_t kHeadBytes = sizeof(kMultipartHead) - 1;
constexpr size_t kTailBytes = sizeof(kMultipartTail) - 1;

// Receive the JPEG where it belongs in the HTTP body to avoid copying it.
uint8_t requestBody[kHeadBytes + kMaxJpegBytes + kTailBytes];
uint8_t *const jpeg = requestBody + kHeadBytes;

// ── Global state ─────────────────────────────────────────────────────────────

bool otaStarted = false;
LiquidCrystal_I2C *gatewayLcd = nullptr;
bool lcdResultVisible = false;
unsigned long lcdResultShownAtMs = 0;
bool lightSensorReady = false;
unsigned long lastLightReadAtMs = 0;
unsigned long lastLightPublishedAtMs = 0;
bool lightPublished = false;

struct BinDistances { float cm[4]; };

QueueHandle_t blynkDistances = nullptr;
QueueHandle_t blynkLightReadings = nullptr;
TaskHandle_t blynkPublisher = nullptr;
SemaphoreHandle_t cloudRequest = nullptr;
std::atomic<bool> captureRequested{false};
bool waitingForCapture = false;
unsigned long captureStartedAtMs = 0;

struct CaptureUpdate {
  char classification[128] = "No valid classification";
  unsigned long photoRevision = 0;
};

#ifdef BINGO_BLYNK_CAMERA_CONTROL
QueueHandle_t blynkCaptureUpdates = nullptr;
bool reconnectBlynk = false;  // Accessed only while holding cloudRequest.

BLYNK_WRITE(V5) {
  if (param.asInt() == 1) {
    captureRequested = true;
  }
}
#endif

#if defined(BINGO_BLYNK_CAMERA_CONTROL) && defined(BLYNK_PHOTO_URL)
static_assert(sizeof(BLYNK_PHOTO_URL) + sizeof("?v=4294967295") <= 256,
              "BLYNK_PHOTO_URL is too long for Blynk Image Gallery");
WebServer photoServer(80);
size_t latestPhotoBytes = 0;

void serveLatestPhoto() {
  photoServer.sendHeader("Cache-Control", "no-store");
  if (latestPhotoBytes == 0) {
    photoServer.send(404, "text/plain", "Capture a photo first");
  } else {
    photoServer.send_P(200, "image/jpeg", reinterpret_cast<const char *>(jpeg), latestPhotoBytes);
  }
}
#endif

// ── Helpers ──────────────────────────────────────────────────────────────────

void startClockSync() {
  static bool started = false;
  if (!started && WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    started = true;
  }
}

// Block until Wi-Fi and a valid TLS clock are available.  Returns false on timeout.
bool ensureConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to Wi-Fi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(kWifiSsid, kWifiPassword);
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
      delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }
  }
  startClockSync();
  if (time(nullptr) < kEarliestTlsTime) {
    unsigned long started = millis();
    while (time(nullptr) < kEarliestTlsTime && millis() - started < 10000) {
      delay(100);
    }
  }
  return time(nullptr) >= kEarliestTlsTime;
}

void showGatewayStatus(const char *status) {
  if (gatewayLcd == nullptr) return;
  char row[kLcdColumns + 1];
  snprintf(row, sizeof(row), "%-*.*s", kLcdColumns, kLcdColumns, status);
  gatewayLcd->setCursor(0, 1);
  gatewayLcd->print(row);
}

void finishCapture(const char *error) {
  if (!waitingForCapture) return;
  waitingForCapture = false;
  captureRequested = false;
  if (error != nullptr) {
    Serial.println(error);
    showGatewayStatus(error);
    lcdResultShownAtMs = millis();
    lcdResultVisible = true;
  }
}

// ── LCD & sensors ────────────────────────────────────────────────────────────

void setupGatewayLcd() {
  if (!Wire.begin(kLcdSdaPin, kLcdSclPin, 100000)) {
    Serial.println("LCD ERR I2C startup");
    return;
  }
  for (uint8_t address : kLcdAddresses) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      static LiquidCrystal_I2C lcd(address, kLcdColumns, kLcdRows);
      lcd.begin(kLcdColumns, kLcdRows);
      lcd.backlight();
      constexpr char title[] = "Bingo";
      lcd.setCursor((kLcdColumns - strlen(title)) / 2, 0);
      lcd.print(title);
      gatewayLcd = &lcd;
      showGatewayStatus("Ready");
      Serial.printf("LCD READY at 0x%02X\n", static_cast<unsigned int>(address));
      return;
    }
  }
  Serial.println("LCD ERR no device at 0x27 or 0x3F");
}

void setupLightSensor() {
  if (!Wire1.begin(kLightSdaPin, kLightSclPin, 100000)) {
    Serial.println("BH1750 ERR I2C startup");
    return;
  }
  Wire1.beginTransmission(kBh1750Address);
  Wire1.write(kBh1750ContinuousHighResolution);
  if (Wire1.endTransmission() != 0) {
    Serial.println("BH1750 ERR no response at 0x23");
    return;
  }
  lastLightReadAtMs = millis();
  lightSensorReady = true;
}

void printLightLevel() {
  const unsigned long now = millis();
  if (!lightSensorReady || now - lastLightReadAtMs < kLightReadIntervalMs) return;
  lastLightReadAtMs = now;

  if (Wire1.requestFrom(kBh1750Address, size_t{2}) != 2) {
    Serial.println("BH1750 ERR read");
    return;
  }
  const uint16_t raw = (Wire1.read() << 8) | Wire1.read();
  const float lux = raw / 1.2f;
  Serial.printf("BH1750 %.1f lux\n", lux);

  if (blynkPublisher != nullptr &&
      (!lightPublished || now - lastLightPublishedAtMs >= kLightPublishIntervalMs)) {
    xQueueOverwrite(blynkLightReadings, &lux);
    xTaskNotifyGive(blynkPublisher);
    lastLightPublishedAtMs = now;
    lightPublished = true;
  }
}

// ── Blynk publisher (FreeRTOS task) ──────────────────────────────────────────

void sendBlynkUpdate(const String &url, const char *logPrefix) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("%s WAIT Wi-Fi\n", logPrefix);
    return;
  }
  if (time(nullptr) < kEarliestTlsTime) {
    Serial.printf("%s WAIT clock sync\n", logPrefix);
    return;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 5000;
  config.disable_auto_redirect = true;

  xSemaphoreTake(cloudRequest, portMAX_DELAY);
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    xSemaphoreGive(cloudRequest);
    Serial.printf("%s ERR HTTP client init\n", logPrefix);
    return;
  }
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  xSemaphoreGive(cloudRequest);

  if (result == ESP_OK && status == 200) {
    Serial.printf("%s OK HTTP 200\n", logPrefix);
  } else if (status > 0 && status != 200) {
    Serial.printf("%s ERR HTTP %d\n", logPrefix, status);
  } else {
    Serial.printf("%s ERR %s\n", logPrefix, esp_err_to_name(result));
  }
}

void sendDistancesToBlynk(const BinDistances &distances) {
  String url = "https://" BLYNK_SERVER "/external/api/batch/update?token=" BLYNK_AUTH_TOKEN;
  const size_t baseLength = url.length();
  for (size_t i = 0; i < 4; ++i) {
    if (distances.cm[i] > 0.0f) {
      char field[20];
      snprintf(field, sizeof(field), "&v%u=%.2f", static_cast<unsigned int>(i), distances.cm[i]);
      if (!url.concat(field)) {
        Serial.println("BLYNK ERR URL allocation");
        return;
      }
    }
  }
  if (url.length() == baseLength) {
    Serial.println("BLYNK SKIP no valid distance");
    return;
  }
  sendBlynkUpdate(url, "BLYNK");
}

void sendLightToBlynk(float lux) {
  char field[20];
  snprintf(field, sizeof(field), "&v4=%.2f", lux);
  String url = "https://" BLYNK_SERVER "/external/api/update?token=" BLYNK_AUTH_TOKEN;
  if (url.concat(field)) {
    sendBlynkUpdate(url, "BLYNK V4");
  } else {
    Serial.println("BLYNK V4 ERR URL allocation");
  }
}

void publishBlynkReadings(void *) {
#ifdef BINGO_BLYNK_CAMERA_CONTROL
  CaptureUpdate update = {};
  bool updatePending = false;
#endif
  for (;;) {
#ifdef BINGO_BLYNK_CAMERA_CONTROL
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    if (xQueueReceive(blynkCaptureUpdates, &update, 0) == pdTRUE) {
      updatePending = true;
    }
    if (WiFi.status() == WL_CONNECTED && time(nullptr) >= kEarliestTlsTime &&
        xSemaphoreTake(cloudRequest, 0) == pdTRUE) {
      if (reconnectBlynk) {
        reconnectBlynk = false;
        Blynk.connect(100);
      } else {
        Blynk.run();
      }
      if (updatePending && Blynk.connected()) {
#ifdef BLYNK_PHOTO_URL
        char url[256];
        snprintf(url, sizeof(url), "%s?v=%lu", BLYNK_PHOTO_URL, update.photoRevision);
        Blynk.setProperty(V7, "urls", url);
#endif
        Blynk.virtualWrite(V6, update.classification);
        updatePending = !Blynk.connected();
      }
      xSemaphoreGive(cloudRequest);
    }
#else
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#endif
    BinDistances distances;
    if (xQueueReceive(blynkDistances, &distances, 0) == pdTRUE) {
      sendDistancesToBlynk(distances);
    }
    float lux;
    if (xQueueReceive(blynkLightReadings, &lux, 0) == pdTRUE) {
      sendLightToBlynk(lux);
    }
  }
}

// ── AI prediction ────────────────────────────────────────────────────────────

struct ApiResponse {
  String body;
  bool omitted = false;
};

String summarizePrediction(const String &body, char *lcdClassName, char *classification,
                           size_t classificationSize) {
  cJSON *root = cJSON_ParseWithOpts(body.c_str(), nullptr, true);
  const cJSON *results = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "images"), 0), "results");

  const char *bestName = nullptr;
  int bestClass = 0;
  double bestConfidence = -1;
  const cJSON *item = nullptr;
  if (cJSON_IsArray(results)) {
    cJSON_ArrayForEach(item, results) {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      const cJSON *classId = cJSON_GetObjectItemCaseSensitive(item, "class");
      const cJSON *confidence = cJSON_GetObjectItemCaseSensitive(item, "confidence");
      if (!cJSON_IsString(name) || name->valuestring[0] == '\0' ||
          !cJSON_IsNumber(classId) || classId->valueint < 0 ||
          classId->valuedouble != classId->valueint ||
          !cJSON_IsNumber(confidence)) {
        continue;
      }
      double score = confidence->valuedouble;
      if (score >= 0 && score <= 1 && score > bestConfidence) {
        bestName = name->valuestring;
        bestClass = classId->valueint;
        bestConfidence = score;
      }
    }
  }

  char summary[128] = "No valid classification";
  if (bestName != nullptr) {
    snprintf(lcdClassName, kLcdColumns + 1, "%.*s", kLcdColumns, bestName);
    for (char *c = lcdClassName; *c; ++c) {
      if (*c == '\r' || *c == '\n') *c = ' ';
    }
    snprintf(summary, sizeof(summary), "class=%d name=%.63s confidence=%.5f",
             bestClass, bestName, bestConfidence);
    snprintf(classification, classificationSize, "%.63s (%.1f%%)", bestName,
             bestConfidence * 100);
  }
  cJSON_Delete(root);
  String line(summary);
  line.replace('\r', ' ');
  line.replace('\n', ' ');
  return line;
}

esp_err_t printApiResponse(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
  Serial.write(static_cast<const uint8_t *>(event->data), event->data_len);
  int status = esp_http_client_get_status_code(event->client);
  auto &response = *static_cast<ApiResponse *>(event->user_data);
  if (status >= 200 && status < 300 && !response.omitted) {
    if (response.body.length() + event->data_len > kMaxResponseBytes ||
        !response.body.concat(static_cast<const char *>(event->data), event->data_len)) {
      response.omitted = true;
    }
  }
  return ESP_OK;
}

String predictWaste(size_t imageSize, char *lcdClassName, char *classification,
                    size_t classificationSize) {
  if (kWifiSsid[0] == '\0' || kUltralyticsApiKey[0] == '\0' ||
      strstr(kUltralyticsPredictUrl, "/OWNER/PROJECT/MODEL/") != nullptr) {
    return "API ERR configure Secrets.h";
  }
  if (strncmp(kUltralyticsPredictUrl, "https://", 8) != 0) {
    return "API ERR HTTPS URL required";
  }
  if (!ensureConnected()) {
    return WiFi.status() != WL_CONNECTED ? "API ERR Wi-Fi timeout"
                                         : "API ERR clock sync timeout";
  }

  memcpy(requestBody, kMultipartHead, kHeadBytes);
  memcpy(jpeg + imageSize, kMultipartTail, kTailBytes);
  String authorization = String("Bearer ") + kUltralyticsApiKey;
  ApiResponse response;
  esp_http_client_config_t config = {};
  config.url = kUltralyticsPredictUrl;
  config.method = HTTP_METHOD_POST;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 35000;
  config.disable_auto_redirect = true;
  config.event_handler = printApiResponse;
  config.user_data = &response;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return "API ERR HTTP client init";

  esp_http_client_set_header(client, "Authorization", authorization.c_str());
  esp_http_client_set_header(client, "Accept-Encoding", "identity");
  esp_http_client_set_header(client, "Content-Type",
                             "multipart/form-data; boundary=BinGoPhotoBoundary");
  esp_http_client_set_post_field(client, reinterpret_cast<const char *>(requestBody),
                                 kHeadBytes + imageSize + kTailBytes);
  Serial.printf("Sending %u JPEG bytes to Ultralytics\n", static_cast<unsigned int>(imageSize));
  esp_err_t result = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  Serial.println();

  if (status >= 300)                return String("API ERR HTTP ") + status;
  if (result != ESP_OK)             return String("API ERR ") + esp_err_to_name(result);
  if (status < 200)                 return String("API ERR HTTP ") + status;

  String reply = String("API OK HTTP ") + status;
  if (response.omitted) {
    reply += " | Response omitted (UART limit 8192 bytes or low memory)";
  } else if (!response.body.isEmpty()) {
    String prediction = summarizePrediction(response.body, lcdClassName, classification,
                                            classificationSize);
    response.body.replace('\r', ' ');
    response.body.replace('\n', ' ');
    if (!reply.reserve(reply.length() + 6 + prediction.length() + response.body.length())) {
      return reply + " | Response omitted (low memory)";
    }
    reply += " | ";
    reply += prediction;
    reply += " | ";
    reply += response.body;
  }
  return reply;
}

// ── Loop handlers ────────────────────────────────────────────────────────────

void handleDistances(const String &header) {
  BinDistances distances;
  int end = 0;
  if (sscanf(header.c_str(), "DIST %f %f %f %f%n", &distances.cm[0], &distances.cm[1],
             &distances.cm[2], &distances.cm[3], &end) != 4 ||
      end == 0 || header.c_str()[end] != '\0') {
    Serial.println("BLYNK ERR distance packet");
    return;
  }
  for (float cm : distances.cm) {
    if (cm != 0.0f && !(cm >= 2.0f && cm <= 400.0f)) {
      Serial.println("BLYNK ERR distance range");
      return;
    }
  }
  if (blynkPublisher != nullptr) {
    xQueueOverwrite(blynkDistances, &distances);
    xTaskNotifyGive(blynkPublisher);
  }
}

bool validateJpeg(size_t imageSize, unsigned long expectedCrc) {
  if (Serial2.readBytes(jpeg, imageSize) != imageSize) {
    Serial2.println("GATEWAY ERR incomplete_image");
    return false;
  }
  if (esp_rom_crc32_le(0, jpeg, imageSize) != expectedCrc) {
    Serial2.println("GATEWAY ERR crc");
    return false;
  }
  if (jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
      jpeg[imageSize - 2] != 0xff || jpeg[imageSize - 1] != 0xd9) {
    Serial2.println("GATEWAY ERR jpeg_markers");
    return false;
  }
  return true;
}

void handleItem(const String &header) {
  unsigned int imageSize = 0;
  unsigned long expectedCrc = 0;
  unsigned int weightGrams = 0;
  if (sscanf(header.c_str(), "ITEM %u %lx %u", &imageSize, &expectedCrc,
             &weightGrams) != 3 || imageSize < 4 || imageSize > kMaxJpegBytes) {
    Serial2.println("GATEWAY ERR header");
    finishCapture("JPEG error");
    return;
  }
#if defined(BINGO_BLYNK_CAMERA_CONTROL) && defined(BLYNK_PHOTO_URL)
  latestPhotoBytes = 0;
#endif
  if (!validateJpeg(imageSize, expectedCrc)) {
    finishCapture("JPEG error");
    return;
  }
#if defined(BINGO_BLYNK_CAMERA_CONTROL) && defined(BLYNK_PHOTO_URL)
  latestPhotoBytes = imageSize;
#endif

  Serial.printf("GATEWAY OK %u bytes, %u g, CRC %08lx\n", imageSize, weightGrams, expectedCrc);
  Serial2.println("GATEWAY OK");
  showGatewayStatus("Classifying...");

  char lcdClassName[kLcdColumns + 1] = {};
  CaptureUpdate update;
  if (cloudRequest != nullptr) xSemaphoreTake(cloudRequest, portMAX_DELAY);
#ifdef BINGO_BLYNK_CAMERA_CONTROL
  if (blynkPublisher != nullptr) {
    Blynk.disconnect();
    reconnectBlynk = true;
  }
#endif
  String result = predictWaste(imageSize, lcdClassName, update.classification,
                               sizeof(update.classification));
  if (cloudRequest != nullptr) xSemaphoreGive(cloudRequest);
  Serial.println(result);
  Serial2.println(result);

  if (result.startsWith("API OK")) {
    showGatewayStatus(lcdClassName[0] ? lcdClassName : "No prediction");
  } else {
    showGatewayStatus("API error");
    snprintf(update.classification, sizeof(update.classification), "%s", result.c_str());
  }
#ifdef BINGO_BLYNK_CAMERA_CONTROL
  if (blynkPublisher != nullptr) {
    update.photoRevision = millis();
    xQueueOverwrite(blynkCaptureUpdates, &update);
    xTaskNotifyGive(blynkPublisher);
  }
#endif
  lcdResultShownAtMs = millis();
  lcdResultVisible = true;
  finishCapture(nullptr);
}

// ── Blynk task setup ─────────────────────────────────────────────────────────

void setupBlynkPublisher() {
  if (BLYNK_AUTH_TOKEN[0] == '\0') return;
  cloudRequest = xSemaphoreCreateMutex();
  blynkDistances = xQueueCreate(1, sizeof(BinDistances));
  blynkLightReadings = xQueueCreate(1, sizeof(float));
  bool queuesReady = blynkDistances != nullptr && blynkLightReadings != nullptr;
#ifdef BINGO_BLYNK_CAMERA_CONTROL
  blynkCaptureUpdates = xQueueCreate(1, sizeof(CaptureUpdate));
  queuesReady = queuesReady && blynkCaptureUpdates != nullptr;
  Blynk.config(BLYNK_AUTH_TOKEN, BLYNK_SERVER);
#endif
  if (cloudRequest == nullptr || !queuesReady ||
      xTaskCreate(publishBlynkReadings, "blynk", 8192, nullptr, 1, &blynkPublisher) != pdPASS) {
#ifdef BINGO_BLYNK_CAMERA_CONTROL
    if (blynkCaptureUpdates) { vQueueDelete(blynkCaptureUpdates); blynkCaptureUpdates = nullptr; }
#endif
    if (blynkDistances)      { vQueueDelete(blynkDistances);      blynkDistances = nullptr; }
    if (blynkLightReadings)  { vQueueDelete(blynkLightReadings);  blynkLightReadings = nullptr; }
    if (cloudRequest)        { vSemaphoreDelete(cloudRequest);    cloudRequest = nullptr; }
    Serial.println("BLYNK ERR publisher startup");
  }
}

// ── Arduino entry points ─────────────────────────────────────────────────────

void setup() {
  Serial.begin(kBaud);
  if (kMpuDiagnosticMode) {
    Mpu6050Diagnostic::begin();
    return;
  }

  Serial2.setRxBufferSize(4096);
  Serial2.begin(kBaud, SERIAL_8N1, kNodeRxPin, kNodeTxPin);
  Serial2.setTimeout(8000);
  setupGatewayLcd();
  setupLightSensor();
  Serial.println("GATEWAY READY");

  setupBlynkPublisher();

  if (kWifiSsid[0] != '\0' && (kOtaPassword[0] != '\0' || blynkPublisher != nullptr)) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(kWifiSsid, kWifiPassword);
  }
#if defined(BINGO_BLYNK_CAMERA_CONTROL) && defined(BLYNK_PHOTO_URL)
  photoServer.on("/latest.jpg", HTTP_GET, serveLatestPhoto);
  photoServer.begin();
#endif
}

void loop() {
  if (kMpuDiagnosticMode) {
    Mpu6050Diagnostic::update();
    return;
  }

  if (lcdResultVisible && millis() - lcdResultShownAtMs >= kLcdResultDurationMs) {
    showGatewayStatus("Ready");
    lcdResultVisible = false;
  }

  startClockSync();
  handleOta(otaStarted, "bingo-gateway");
  printLightLevel();

  if (!Serial2.available()) {
    if (waitingForCapture && millis() - captureStartedAtMs >= kCaptureTimeoutMs) {
      finishCapture("Capture timeout");
    } else if (!waitingForCapture && captureRequested) {
      waitingForCapture = true;
      captureStartedAtMs = millis();
      lcdResultVisible = false;
      showGatewayStatus("Capturing...");
      Serial2.println("CAPTURE");
    }
#if defined(BINGO_BLYNK_CAMERA_CONTROL) && defined(BLYNK_PHOTO_URL)
    if (!waitingForCapture) photoServer.handleClient();
#endif
    delay(1);
    return;
  }

  String header = Serial2.readStringUntil('\n');
  header.trim();
  if (header == "PING")        { Serial2.println("PONG"); return; }
  if (header == "CAPTURE ERR") { finishCapture("Camera error"); return; }
  if (header.startsWith("DIST ")) { handleDistances(header); return; }
  handleItem(header);
}
