#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_rom_crc.h>
#include <time.h>

#if __has_include("YoloSecrets.h")
#include "YoloSecrets.h"
#else
#include "YoloSecrets.example.h"
#endif

constexpr char multipartHead[] =
    "--BinGoJPEG\r\nContent-Disposition: form-data; name=\"conf\"\r\n\r\n0.25\r\n"
    "--BinGoJPEG\r\nContent-Disposition: form-data; name=\"iou\"\r\n\r\n0.5\r\n"
    "--BinGoJPEG\r\nContent-Disposition: form-data; name=\"imgsz\"\r\n\r\n640\r\n"
    "--BinGoJPEG\r\nContent-Disposition: form-data; name=\"file\"; filename=\"image.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n";
constexpr char multipartTail[] = "\r\n--BinGoJPEG--\r\n";
constexpr size_t maxJpegBytes = 48 * 1024;
uint8_t yoloBody[sizeof(multipartHead) - 1 + maxJpegBytes + sizeof(multipartTail) - 1];

inline bool tlsClockReady() {
  return time(nullptr) >= 1704067200;
}

inline void startTlsClockSync() {
  static bool started = false;
  if (!started && WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    started = true;
  }
}

struct YoloResponse {
  String body;
  bool incomplete = false;
};

inline esp_err_t collectYoloResponse(esp_http_client_event_t *event) {
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
    Serial.write(static_cast<const uint8_t *>(event->data), event->data_len);
    auto &response = *static_cast<YoloResponse *>(event->user_data);
    if (!response.incomplete &&
        (response.body.length() + event->data_len > 8192 ||
         !response.body.concat(static_cast<const char *>(event->data), event->data_len))) {
      response.incomplete = true;
    }
  }
  return ESP_OK;
}

inline String bestYoloClass(const String &body) {
  cJSON *root = cJSON_ParseWithOpts(body.c_str(), nullptr, true);
  const cJSON *image = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "images"), 0);
  const cJSON *results = cJSON_GetObjectItemCaseSensitive(image, "results");
  const char *bestName = nullptr;
  double bestScore = -1;
  const cJSON *item = nullptr;
  if (cJSON_IsArray(results)) {
    cJSON_ArrayForEach(item, results) {
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      const cJSON *score = cJSON_GetObjectItemCaseSensitive(item, "confidence");
      if (cJSON_IsString(name) && name->valuestring[0] && cJSON_IsNumber(score) &&
          score->valuedouble > 0 && score->valuedouble <= 1 && score->valuedouble > bestScore) {
        bestName = name->valuestring;
        bestScore = score->valuedouble;
      }
    }
  }
  String label;
  if (bestName) {
    label = bestName;
    Serial.printf("YOLO top class: %s (%.1f%%)\n", bestName, bestScore * 100);
  }
  cJSON_Delete(root);
  return label;
}

inline String runYolo(HardwareSerial &camera) {
  if (!yoloApiKey[0] || strncmp(yoloUrl, "https://", 8) != 0) {
    Serial.println("YOLO: configure HTTPS URL and API key in YoloSecrets.h");
    return {};
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("YOLO: Wi-Fi is disconnected");
    return {};
  }
  if (!tlsClockReady()) {
    Serial.println("YOLO: TLS clock unavailable");
    return {};
  }

  while (camera.read() >= 0) {}
  camera.write('P');
  char header[32] = {};
  const size_t headerLength = camera.readBytesUntil('\n', header, sizeof(header) - 1);
  unsigned length = 0;
  unsigned long crc = 0;
  if (headerLength == sizeof(header) - 1 ||
      sscanf(header, "JPEG %u %lx", &length, &crc) != 2 ||
      length < 4 || length > maxJpegBytes) {
    Serial.println("YOLO: no valid JPEG header from camera");
    return {};
  }

  uint8_t *jpeg = yoloBody + sizeof(multipartHead) - 1;
  if (camera.readBytes(jpeg, length) != length ||
      esp_rom_crc32_le(0, jpeg, length) != crc ||
      jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
      jpeg[length - 2] != 0xff || jpeg[length - 1] != 0xd9) {
    Serial.println("YOLO: incomplete or invalid JPEG");
    return {};
  }
  memcpy(yoloBody, multipartHead, sizeof(multipartHead) - 1);
  memcpy(jpeg + length, multipartTail, sizeof(multipartTail) - 1);
  const String authorization = String("Bearer ") + yoloApiKey;

  esp_http_client_config_t config = {};
  config.url = yoloUrl;
  config.method = HTTP_METHOD_POST;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 35000;
  config.disable_auto_redirect = true;
  YoloResponse response;
  config.event_handler = collectYoloResponse;
  config.user_data = &response;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    Serial.println("YOLO: HTTP client init failed");
    return {};
  }
  esp_http_client_set_header(client, "Authorization", authorization.c_str());
  esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=BinGoJPEG");
  esp_http_client_set_post_field(client, reinterpret_cast<const char *>(yoloBody),
                                sizeof(multipartHead) - 1 + length + sizeof(multipartTail) - 1);
  Serial.printf("YOLO: sending %u bytes\n", length);
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  Serial.printf("\nYOLO HTTP %d (%s)\n", status, esp_err_to_name(result));
  if (result != ESP_OK || status != 200) return {};
  if (response.incomplete) {
    Serial.println("YOLO: response too large or memory unavailable");
    return {};
  }
  return bestYoloClass(response.body);
}
