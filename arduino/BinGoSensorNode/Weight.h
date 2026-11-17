#pragma once

#include <Arduino.h>
#include <HX711.h>
#include <Preferences.h>
#include <esp_random.h>
#include <math.h>
#include "ItemTrigger.h"

constexpr uint8_t weightDataPin = 32;
constexpr uint8_t weightClockPin = 33;
constexpr float calibrationMassGrams = 1000.0f;
constexpr float measuredCountsPerGram = -404.067871f;  // Reference calibration for this load cell.
static_assert(calibrationMassGrams > 0 && calibrationMassGrams <= 5000, "Invalid reference mass");

HX711 scale;
Preferences weightSettings;
bool weightStorageReady = false;
int32_t zeroCounts = INT32_MIN;
float countsPerGram = measuredCountsPerGram;
int32_t weightSamples[8] = {};
uint8_t sampleCount = 0;
uint8_t sampleIndex = 0;
unsigned long lastWeightSample = 0;
unsigned long weightStartedAt = 0;
ItemTrigger itemTrigger;

inline int32_t averageWeightCounts() {
  int32_t total = 0;
  for (int32_t sample : weightSamples) total += sample;
  return total / 8;
}

inline void beginWeight() {
  scale.begin(weightDataPin, weightClockPin, 128);
  weightStartedAt = millis();
  itemTrigger.sequence = esp_random();
  weightStorageReady = weightSettings.begin("hx711-scale", false);
  const float savedFactor = weightStorageReady ? weightSettings.getFloat("factor", 0) : 0;
  const bool hasSavedFactor = isfinite(savedFactor) && savedFactor != 0;
  countsPerGram = hasSavedFactor ? savedFactor : measuredCountsPerGram;
  Serial.printf("HX711 %s factor: %.6f counts/g\n",
                hasSavedFactor ? "saved" : "reference", countsPerGram);
  Serial.printf("HX711: w read, z tare, k calibrate with %.1f g\n", calibrationMassGrams);
  Serial.println("HX711: keep the tray empty until automatic tare completes");
}

inline void updateWeight() {
  if (!scale.is_ready()) return;
  const unsigned long now = millis();
  if (now - lastWeightSample >= 500) {
    sampleCount = 0;
    sampleIndex = 0;
    itemTrigger.pause();
  }
  const long raw = scale.read();
  if (zeroCounts == INT32_MIN && now - weightStartedAt < 1000) return;
  if (raw <= -8388608L || raw >= 8388607L) {
    sampleCount = 0;
    itemTrigger.pause();
    return;
  }
  weightSamples[sampleIndex] = raw;
  sampleIndex = (sampleIndex + 1) % 8;
  if (sampleCount < 8) ++sampleCount;
  lastWeightSample = now;
  if (zeroCounts == INT32_MIN && sampleCount == 8) {
    zeroCounts = averageWeightCounts();
    Serial.println("HX711: automatic tare complete");
  }
  if (zeroCounts != INT32_MIN) {
    const float grams = (raw - zeroCounts) / countsPerGram;
    if (isfinite(grams) && fabsf(grams) <= 10000) itemTrigger.sample(grams, now);
    else itemTrigger.pause();
  }
}

inline bool weightReady() {
  return sampleCount == 8 && millis() - lastWeightSample < 1000;
}

inline int32_t readWeightGrams() {
  if (!weightReady() || zeroCounts == INT32_MIN || !isfinite(countsPerGram) ||
      countsPerGram == 0) return INT32_MIN;
  const float grams = (averageWeightCounts() - zeroCounts) / countsPerGram;
  if (!isfinite(grams) || fabsf(grams) > 10000) return INT32_MIN;
  return lroundf(grams);
}

inline bool trayIsEmpty(int32_t grams) {
  return grams != INT32_MIN && grams <= emptyTrayMaxGrams && itemTrigger.clearing &&
         millis() - itemTrigger.emptyFrom >= emptyTrayHoldMs && millis() - lastWeightSample < 500;
}

inline void weightCommand(int command) {
  if (!weightReady()) {
    Serial.println("HX711: waiting for fresh samples");
    return;
  }
  const int32_t raw = averageWeightCounts();
  if (command == 'z') {
    zeroCounts = raw;
    itemTrigger.pause();
    Serial.println("HX711: tared for this session");
  } else if (command == 'k') {
    if (zeroCounts == INT32_MIN || labs(raw - zeroCounts) < 100) {
      Serial.println("HX711: tare first, then place the known mass and wait for readings to settle");
      return;
    }
    countsPerGram = (raw - zeroCounts) / calibrationMassGrams;
    itemTrigger.pause();
    const bool saved = weightStorageReady && weightSettings.putFloat("factor", countsPerGram) == sizeof(float);
    Serial.printf("HX711 factor: %.6f counts/g\n", countsPerGram);
    Serial.println(saved ? "HX711: calibration saved" : "HX711: calibrated for this session");
  }

  const int32_t grams = readWeightGrams();
  Serial.printf("HX711 raw: %ld\n", static_cast<long>(raw));
  if (grams == INT32_MIN) Serial.println("Weight unavailable; check tare and calibration");
  else Serial.printf("Weight: %ld g\n", static_cast<long>(grams));
}
