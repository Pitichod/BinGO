#include <Arduino.h>
#include <ArduinoOTA.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <Wire.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_rom_crc.h>
#include <time.h>

#if __has_include("Secrets.h")
#include "Secrets.h"
#else
#include "Secrets.example.h"
#endif

#if __has_include("OtaSecrets.h")
#include "OtaSecrets.h"
#else
constexpr char kOtaPassword[] = "";
#endif

constexpr unsigned long kBaud = 115200;
constexpr size_t kMaxJpegBytes = 64 * 1024;
constexpr size_t kMaxResponseBytes = 8 * 1024;
constexpr int kNodeRxPin = 33;
constexpr int kNodeTxPin = 32;
constexpr int kLcdSdaPin = 21;
constexpr int kLcdSclPin = 22;
constexpr uint8_t kLcdColumns = 16;
constexpr uint8_t kLcdRows = 2;
constexpr uint8_t kLcdAddresses[] = {0x27, 0x3F};

constexpr char kMultipartHead[] =
    "--BinGoPhotoBoundary\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"bingo.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
constexpr char kMultipartTail[] = "\r\n--BinGoPhotoBoundary--\r\n";
constexpr size_t kHeadBytes = sizeof(kMultipartHead) - 1;
constexpr size_t kTailBytes = sizeof(kMultipartTail) - 1;
constexpr time_t kEarliestTlsTime = 1704067200;  // 2024-01-01 UTC

// Receive the JPEG where it belongs in the HTTP body to avoid copying it.
uint8_t requestBody[kHeadBytes + kMaxJpegBytes + kTailBytes];
uint8_t *const jpeg = requestBody + kHeadBytes;
bool otaStarted = false;
LiquidCrystal_I2C *gatewayLcd = nullptr;

void showGatewayStatus(const char *status) {
  if (gatewayLcd == nullptr) {
    return;
  }

  char row[kLcdColumns + 1];
  snprintf(row, sizeof(row), "%-*.*s", kLcdColumns, kLcdColumns, status);
  gatewayLcd->setCursor(0, 1);
  gatewayLcd->print(row);
}

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

struct ApiResponse {
  String body;
  bool omitted = false;
};

String summarizePrediction(const String &body, char *lcdClassName) {
  cJSON *root = cJSON_ParseWithOpts(body.c_str(), nullptr, true);
  const cJSON *images = cJSON_GetObjectItemCaseSensitive(root, "images");
  const cJSON *image = cJSON_GetArrayItem(images, 0);
  const cJSON *results = cJSON_GetObjectItemCaseSensitive(image, "results");

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
    for (char *character = lcdClassName; *character != '\0'; ++character) {
      if (*character == '\r' || *character == '\n') {
        *character = ' ';
      }
    }
    snprintf(summary, sizeof(summary), "class=%d name=%.63s confidence=%.5f",
             bestClass, bestName, bestConfidence);
  }
  cJSON_Delete(root);
  String line(summary);
  line.replace('\r', ' ');
  line.replace('\n', ' ');
  return line;
}

esp_err_t printApiResponse(esp_http_client_event_t *event) {
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
    Serial.write(static_cast<const uint8_t *>(event->data), event->data_len);
    int status = esp_http_client_get_status_code(event->client);
    auto &response = *static_cast<ApiResponse *>(event->user_data);
    if (status >= 200 && status < 300 && !response.omitted) {
      if (response.body.length() + event->data_len > kMaxResponseBytes ||
          !response.body.concat(static_cast<const char *>(event->data), event->data_len)) {
        response.omitted = true;
      }
    }
  }
  return ESP_OK;
}

String predictWaste(size_t imageSize, char *lcdClassName) {
  if (kWifiSsid[0] == '\0' || kUltralyticsApiKey[0] == '\0' ||
      strstr(kUltralyticsPredictUrl, "/OWNER/PROJECT/MODEL/") != nullptr) {
    return "API ERR configure Secrets.h";
  }
  if (strncmp(kUltralyticsPredictUrl, "https://", 8) != 0) {
    return "API ERR HTTPS URL required";
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to Wi-Fi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(kWifiSsid, kWifiPassword);
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
      delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
      return "API ERR Wi-Fi timeout";
    }
  }

  if (time(nullptr) < kEarliestTlsTime) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    unsigned long started = millis();
    while (time(nullptr) < kEarliestTlsTime && millis() - started < 10000) {
      delay(100);
    }
    if (time(nullptr) < kEarliestTlsTime) {
      return "API ERR clock sync timeout";
    }
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
  if (client == nullptr) {
    return "API ERR HTTP client init";
  }

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
  // ESP-IDF can return a client error while handling a server's HTTP 401.
  if (status >= 300) {
    return String("API ERR HTTP ") + status;
  }
  if (result != ESP_OK) {
    return String("API ERR ") + esp_err_to_name(result);
  }
  if (status < 200) {
    return String("API ERR HTTP ") + status;
  }

  // The sensor node already reads one newline-terminated API reply.
  String reply = String("API OK HTTP ") + status;
  if (response.omitted) {
    reply += " | Response omitted (UART limit 8192 bytes or low memory)";
  } else if (!response.body.isEmpty()) {
    String prediction = summarizePrediction(response.body, lcdClassName);
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

void setup() {
  Serial.begin(kBaud);
  Serial2.setRxBufferSize(4096);
  Serial2.begin(kBaud, SERIAL_8N1, kNodeRxPin, kNodeTxPin);
  Serial2.setTimeout(8000);
  setupGatewayLcd();
  Serial.println("GATEWAY READY");

  if (kWifiSsid[0] != '\0' && kOtaPassword[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(kWifiSsid, kWifiPassword);
  }
}

void loop() {
  if (!otaStarted && kOtaPassword[0] != '\0' && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname("bingo-gateway");
    ArduinoOTA.setPassword(kOtaPassword);
    ArduinoOTA.begin();
    otaStarted = true;
    Serial.print("OTA READY bingo-gateway at ");
    Serial.println(WiFi.localIP());
  }
  if (otaStarted) {
    ArduinoOTA.handle();
  }

  if (!Serial2.available()) {
    delay(1);
    return;
  }

  String header = Serial2.readStringUntil('\n');
  header.trim();
  if (header == "PING") {
    Serial2.println("PONG");
    return;
  }

  unsigned int imageSize = 0;
  unsigned long expectedCrc = 0;
  unsigned int weightGrams = 0;
  if (sscanf(header.c_str(), "ITEM %u %lx %u", &imageSize, &expectedCrc,
             &weightGrams) != 3 || imageSize < 4 || imageSize > kMaxJpegBytes) {
    Serial2.println("GATEWAY ERR header");
    return;
  }
  if (Serial2.readBytes(jpeg, imageSize) != imageSize) {
    Serial2.println("GATEWAY ERR incomplete_image");
    return;
  }
  if (esp_rom_crc32_le(0, jpeg, imageSize) != expectedCrc) {
    Serial2.println("GATEWAY ERR crc");
    return;
  }
  if (jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
      jpeg[imageSize - 2] != 0xff || jpeg[imageSize - 1] != 0xd9) {
    Serial2.println("GATEWAY ERR jpeg_markers");
    return;
  }

  Serial.printf("GATEWAY OK %u bytes, %u g, CRC %08lx\n",
                imageSize, weightGrams, expectedCrc);
  Serial2.println("GATEWAY OK");
  showGatewayStatus("Classifying...");
  char lcdClassName[kLcdColumns + 1] = {};
  String result = predictWaste(imageSize, lcdClassName);
  Serial.println(result);
  Serial2.println(result);
  if (result.startsWith("API OK")) {
    showGatewayStatus(lcdClassName[0] ? lcdClassName : "No prediction");
  } else {
    showGatewayStatus("API error");
  }
}
