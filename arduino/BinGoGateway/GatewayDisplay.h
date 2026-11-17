#pragma once

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

LiquidCrystal_I2C *gatewayLcd = nullptr;
QueueHandle_t lcdPages = nullptr;
std::atomic<const char *> lcdHomeStatus{"Joining Wi-Fi..."};

// Status labels are string literals so the display task can read them later.
inline void setLcdHomeStatus(const char *status) {
  lcdHomeStatus.store(status);
}

struct LcdPage {
  char rows[2][17];
};

inline void drawLcd(const char *top, const char *bottom) {
  const char *rows[] = {top, bottom};
  for (uint8_t i = 0; i < 2; ++i) {
    char line[17];
    snprintf(line, sizeof(line), "%-16.16s", rows[i]);
    gatewayLcd->setCursor(0, i);
    gatewayLcd->print(line);
  }
}

inline void lcdTask(void *) {
  LcdPage page;
  for (;;) {
    const char *shownStatus = nullptr;
    do {
      const char *status = lcdHomeStatus.load();
      if (status != shownStatus) {
        char centered[17];
        snprintf(centered, sizeof(centered), "%*s%.16s",
                 (16 - static_cast<int>(strnlen(status, 16))) / 2, "", status);
        drawLcd("     Bingo", centered);
        shownStatus = status;
      }
    } while (xQueueReceive(lcdPages, &page, pdMS_TO_TICKS(250)) != pdTRUE);
    do {
      drawLcd(page.rows[0], page.rows[1]);
    } while (xQueueReceive(lcdPages, &page, pdMS_TO_TICKS(10000)) == pdTRUE);
  }
}

inline void showLcd(const char *top, const char *bottom) {
  if (!lcdPages) return;
  LcdPage page;
  snprintf(page.rows[0], sizeof(page.rows[0]), "%.16s", top);
  snprintf(page.rows[1], sizeof(page.rows[1]), "%.16s", bottom);
  xQueueOverwrite(lcdPages, &page);
}

inline void beginLcd() {
  constexpr uint8_t addresses[] = {0x27, 0x3f};
  for (uint8_t address : addresses) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() != 0) continue;
    static LiquidCrystal_I2C lcd(address, 16, 2);
    lcd.begin(16, 2);
    lcd.backlight();
    gatewayLcd = &lcd;
    lcdPages = xQueueCreate(1, sizeof(LcdPage));
    if (!lcdPages || xTaskCreate(lcdTask, "lcd", 3072, nullptr, 1, nullptr) != pdPASS) {
      if (lcdPages) vQueueDelete(lcdPages);
      lcdPages = nullptr;
      Serial.println("LCD: display task could not start");
      return;
    }
    Serial.printf("LCD ready at 0x%02X\n", static_cast<unsigned>(address));
    return;
  }
  Serial.println("LCD: no device at 0x27 or 0x3F");
}

inline void showMeasurements(int32_t grams, const uint16_t *distanceMm) {
  char top[17];
  if (grams == INT32_MIN) snprintf(top, sizeof(top), "Weight: --");
  else snprintf(top, sizeof(top), "Weight: %ld g", static_cast<long>(grams));

  char cm[2][5];
  for (uint8_t i = 0; i < 2; ++i) {
    const uint16_t mm = distanceMm[i];
    if (mm == 0 || mm > 4000) snprintf(cm[i], sizeof(cm[i]), "--");
    else snprintf(cm[i], sizeof(cm[i]), "%u", static_cast<unsigned>((mm + 5) / 10));
  }
  char bottom[17];
  snprintf(bottom, sizeof(bottom), "1:%s 2:%s cm", cm[0], cm[1]);
  showLcd(top, bottom);
}
