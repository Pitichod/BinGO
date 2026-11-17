#pragma once

#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>

constexpr uint8_t dhtDataPin = 23;
constexpr uint8_t lightAddress = 0x23;
constexpr unsigned long environmentReadMs = 2500;

DHT gatewayDht(dhtDataPin, DHT11);
bool lightReady = false;
float temperatureC = NAN;
float humidityPercent = NAN;
float lightLux = NAN;
unsigned long lastEnvironmentRead = 0;

inline void beginEnvironmentSensors(bool i2cReady) {
  gatewayDht.begin();
  if (!i2cReady) return;
  Wire.beginTransmission(lightAddress);
  Wire.write(0x10);  // BH1750 continuous high-resolution mode.
  lightReady = Wire.endTransmission() == 0;
  Serial.println(lightReady ? "BH1750 ready at 0x23" : "BH1750: no reply at 0x23");
}

inline void updateEnvironmentSensors() {
  if (millis() - lastEnvironmentRead < environmentReadMs) return;
  lastEnvironmentRead = millis();
  humidityPercent = gatewayDht.readHumidity();
  temperatureC = gatewayDht.readTemperature();

  lightLux = NAN;
  if (lightReady && Wire.requestFrom(lightAddress, size_t{2}) == 2) {
    const uint16_t high = Wire.read();
    const uint16_t low = Wire.read();
    lightLux = ((high << 8) | low) / 1.2f;
  }
}
