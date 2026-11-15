#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

namespace Mpu6050Diagnostic {
constexpr int kSda = 23;
constexpr int kScl = 27;
constexpr uint8_t kWhoAmI = 0x75;
constexpr uint8_t kPowerManagement = 0x6B;
constexpr uint8_t kAccelOut = 0x3B;
constexpr unsigned long kSampleMs = 20;
constexpr unsigned long kFrameMs = 100;
constexpr int kCalibrationSamples = 50;
constexpr float kDegreesPerRadian = 180.0f / PI;
constexpr int kPlotWidth = 17;
constexpr int kPlotHeight = 7;

uint8_t address = 0;
unsigned long lastSampleAt = 0;
unsigned long lastFrameAt = 0;
int calibrationSamples = 0;
float gyroZBias = 0.0f;
float gravity[3] = {};
float yaw = 0.0f;
bool yawNeedsZero = false;

bool readRegisters(uint8_t device, uint8_t reg, uint8_t *data, size_t count) {
  Wire.beginTransmission(device);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(device, count) != count) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

uint8_t detect() {
  for (const uint8_t device : {0x68, 0x69}) {
    uint8_t id = 0;
    if (!readRegisters(device, kWhoAmI, &id, 1) || id != 0x68) {
      continue;
    }
    Wire.beginTransmission(device);
    Wire.write(kPowerManagement);
    Wire.write(0x01);  // Wake up and use the X-gyro clock.
    if (Wire.endTransmission() == 0) {
      delay(100);
      return device;
    }
  }
  return 0;
}

int16_t readAxis(const uint8_t *data) {
  return static_cast<int16_t>((uint16_t{data[0]} << 8) | data[1]);
}

void showStatus(const char *status) {
  Serial.printf("\x1b[HBinGo MPU-6050 | SDA 23 / SCL 27\x1b[K\r\n\r\n%s\x1b[K\r\n\x1b[J", status);
}

void drawPlot(char (&plot)[kPlotHeight][kPlotWidth + 1], float angle, bool heading) {
  constexpr int centerX = kPlotWidth / 2;
  constexpr int centerY = kPlotHeight / 2;
  for (int row = 0; row < kPlotHeight; ++row) {
    memset(plot[row], ' ', kPlotWidth);
    plot[row][kPlotWidth] = '\0';
  }
  for (int col = 0; col < kPlotWidth; ++col) {
    plot[centerY][col] = '.';
  }
  plot[centerY][centerX] = '+';
  if (!isfinite(angle)) {
    return;
  }

  const float radians = angle / kDegreesPerRadian;
  const float x = heading ? -sinf(radians) : cosf(radians);
  const float y = heading ? cosf(radians) : sinf(radians);
  for (int point = heading ? 0 : -8; point <= 8; ++point) {
    const int col = centerX + lroundf(point * x);
    const int row = centerY - lroundf(point * y * centerY / 8);
    plot[row][col] = heading && point == 8 ? 'o' : '#';
  }
}

void drawFrame(float roll, float pitch, bool yawValid, bool level) {
  char front[kPlotHeight][kPlotWidth + 1];
  char side[kPlotHeight][kPlotWidth + 1];
  char top[kPlotHeight][kPlotWidth + 1];
  drawPlot(front, roll, false);
  drawPlot(side, pitch, false);
  drawPlot(top, yawValid ? yaw : NAN, true);

  Serial.print("\x1b[HBinGo MPU-6050 | Live tray orientation\r\n\r\n"
               " FRONT / ROLL      SIDE / PITCH      TOP / YAW\x1b[K\r\n");
  for (int row = 0; row < kPlotHeight; ++row) {
    Serial.printf("%s  %s  %s\r\n", front[row], side[row], top[row]);
  }
  Serial.printf("%+7.1f deg       %+7.1f deg       ", roll, pitch);
  if (yawValid) {
    Serial.printf("%+7.1f deg", yaw);
  } else {
    Serial.print("    ---");
  }
  Serial.print("\x1b[K\r\n\r\n");
  if (!level) {
    Serial.print("Level the tray to measure yaw.");
  } else if (!yawValid) {
    Serial.print("Press Z to set the yaw reference.");
  } else {
    Serial.print("Yaw is relative and drifts. Press Z to zero it.");
  }
  Serial.print("\x1b[K\r\n\x1b[J");
}

void begin() {
  Serial.print("\x1b[2J");
  Wire.begin(kSda, kScl, 100000);
  Wire.setTimeOut(20);
}

void update() {
  const unsigned long now = millis();
  if (address == 0) {
    if (now - lastSampleAt >= 500) {
      lastSampleAt = now;
      address = detect();
      calibrationSamples = 0;
      gyroZBias = 0.0f;
      yaw = 0.0f;
      yawNeedsZero = false;
      showStatus(address == 0 ? "MPU-6050 not found. Check power, SDA and SCL."
                             : "Keep the tray still for 1 second to calibrate.");
    }
    delay(1);
    return;
  }

  if (now - lastSampleAt < kSampleMs) {
    delay(1);
    return;
  }
  const float seconds = (now - lastSampleAt) / 1000.0f;
  lastSampleAt = now;
  uint8_t data[14];
  if (!readRegisters(address, kAccelOut, data, sizeof(data))) {
    address = 0;
    showStatus("MPU-6050 read failed. Reconnecting...");
    return;
  }

  for (int axis = 0; axis < 3; ++axis) {
    const float acceleration = readAxis(data + axis * 2);
    gravity[axis] = calibrationSamples == 0 ? acceleration
                                           : 0.8f * gravity[axis] + 0.2f * acceleration;
  }
  const float gyroZ = readAxis(data + 12) / 131.0f;  // Default gyro range is +/-250 deg/s.
  if (calibrationSamples < kCalibrationSamples) {
    gyroZBias += gyroZ / kCalibrationSamples;
    ++calibrationSamples;
    return;
  }

  const float roll = atan2f(gravity[1], gravity[2]) * kDegreesPerRadian;
  const float pitch = atan2f(-gravity[0], hypotf(gravity[1], gravity[2])) * kDegreesPerRadian;
  const bool level = fabsf(roll) <= 15.0f && fabsf(pitch) <= 15.0f;
  if (!level) {
    yawNeedsZero = true;
  }
  if (!yawNeedsZero) {
    yaw = remainderf(yaw + (gyroZ - gyroZBias) * seconds, 360.0f);
  }
  while (Serial.available()) {
    const int key = Serial.read();
    if (level && (key == 'z' || key == 'Z')) {
      yaw = 0.0f;
      yawNeedsZero = false;
    }
  }
  if (now - lastFrameAt >= kFrameMs) {
    lastFrameAt = now;
    drawFrame(roll, pitch, !yawNeedsZero, level);
  }
}
}  // namespace Mpu6050Diagnostic
