#include "../BinGoCommon.h"
#include <esp_camera.h>
#include <esp_rom_crc.h>

constexpr int kNodeRxPin = 14;
constexpr int kNodeTxPin = 13;
constexpr int kFlashPin = 4;

bool cameraReady = false;
bool otaStarted = false;

void setup() {
  Serial.begin(kBaud);
  Serial1.begin(kBaud, SERIAL_8N1, kNodeRxPin, kNodeTxPin);
  pinMode(kFlashPin, OUTPUT);
  digitalWrite(kFlashPin, LOW);

  // AI-Thinker ESP32-CAM camera connector. Leave microSD disabled.
  camera_config_t config = {};
  config.pin_pwdn = 32;   config.pin_reset = -1;
  config.pin_xclk = 0;    config.pin_sccb_sda = 26;  config.pin_sccb_scl = 27;
  config.pin_d7 = 35;     config.pin_d6 = 34;        config.pin_d5 = 39;
  config.pin_d4 = 36;     config.pin_d3 = 21;        config.pin_d2 = 19;
  config.pin_d1 = 18;     config.pin_d0 = 5;
  config.pin_vsync = 25;  config.pin_href = 23;      config.pin_pclk = 22;
  config.xclk_freq_hz = 20000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 15;
  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  cameraReady = esp_camera_init(&config) == ESP_OK;
  Serial.println(cameraReady ? "CAM READY" : "Camera init failed");

  beginOtaWifi();
}

void loop() {
  handleOta(otaStarted, "bingo-camera");

  if (Serial1.read() != 'C') {
    delay(1);
    return;
  }
  if (!cameraReady) {
    Serial1.println("ERR camera_init");
    return;
  }

  digitalWrite(kFlashPin, HIGH);
  delay(250);
  // The queue can still hold a frame captured before the flash came on.
  camera_fb_t *previousFrame = esp_camera_fb_get();
  if (previousFrame != nullptr) {
    esp_camera_fb_return(previousFrame);
  }
  camera_fb_t *frame = esp_camera_fb_get();
  digitalWrite(kFlashPin, LOW);
  if (frame == nullptr) {
    Serial1.println("ERR capture");
    return;
  }

  if (frame->format == PIXFORMAT_JPEG && frame->len >= 4 &&
      frame->len <= kMaxJpegBytes) {
    uint32_t crc = esp_rom_crc32_le(0, frame->buf, frame->len);
    Serial1.printf("JPEG %u %08lx\n", static_cast<unsigned int>(frame->len),
                   static_cast<unsigned long>(crc));
    Serial1.write(frame->buf, frame->len);
    Serial.printf("CAM SENT %u bytes\n", static_cast<unsigned int>(frame->len));
  } else {
    Serial1.println("ERR jpeg_format_or_size");
  }
  esp_camera_fb_return(frame);
}
