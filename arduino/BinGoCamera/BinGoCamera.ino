#include <Arduino.h>
#include <esp_camera.h>
#include <esp_rom_crc.h>

constexpr int flashPin = 4;
bool cameraReady = false;

void initCamera() {
  camera_config_t config = {};
  config.pin_pwdn = 32;
  config.pin_reset = -1;
  config.pin_xclk = 0;
  config.pin_sccb_sda = 26;
  config.pin_sccb_scl = 27;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_pclk = 22;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 16;
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  const esp_err_t result = esp_camera_init(&config);
  cameraReady = result == ESP_OK;
  Serial.printf("Camera init: %s\n", esp_err_to_name(result));
}

void sendPhoto() {
  camera_fb_t *frame = nullptr;
  if (cameraReady) {
    digitalWrite(flashPin, HIGH);
    delay(150);

    // Drop the frame that may have been queued before the flash turned on.
    camera_fb_t *queuedFrame = esp_camera_fb_get();
    if (queuedFrame) {
      esp_camera_fb_return(queuedFrame);
      frame = esp_camera_fb_get();
    }
    digitalWrite(flashPin, LOW);
  }
  if (!frame || frame->len < 4 || frame->len > 48 * 1024) {
    Serial1.println("ERR");
  } else {
    const auto crc = esp_rom_crc32_le(0, frame->buf, frame->len);
    Serial1.printf("JPEG %u %08lx\n", static_cast<unsigned>(frame->len),
                   static_cast<unsigned long>(crc));
    Serial1.write(frame->buf, frame->len);
    Serial1.flush();
  }
  if (frame) esp_camera_fb_return(frame);
}

#if __has_include("OtaSecrets.h")
#include <ArduinoOTA.h>
#include <WiFi.h>
#include "OtaSecrets.h"

constexpr char otaHostname[] = "bingo-camera";
bool otaReady = false;
#endif

void setup() {
  Serial.begin(115200);
  pinMode(flashPin, OUTPUT);
  digitalWrite(flashPin, LOW);
  Serial1.begin(115200, SERIAL_8N1, 14, 13);
  Serial.println("Camera UART ready");
  initCamera();
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
      ArduinoOTA.onStart([]() { Serial.println("OTA started"); });
      ArduinoOTA.onEnd([]() { Serial.println("OTA complete"); });
      ArduinoOTA.onError([](ota_error_t error) {
        if (error == OTA_BEGIN_ERROR) {
          Serial.println("OTA error: could not initialize update; check running partition scheme");
        } else if (error == OTA_CONNECT_ERROR) {
          Serial.println("OTA error: could not connect back to uploader over TCP");
        } else {
          Serial.printf("OTA error: %u\n", static_cast<unsigned>(error));
        }
      });
      ArduinoOTA.begin();
      otaReady = true;
      Serial.printf("OTA READY %s at ", otaHostname);
      Serial.println(WiFi.localIP());
    }
    ArduinoOTA.handle();
  }
#endif
  const int command = Serial1.read();
  if (command == '?') {
    Serial1.write('C');
    Serial.println("Ping received");
  } else if (command == 'P') {
    sendPhoto();
  }
}
