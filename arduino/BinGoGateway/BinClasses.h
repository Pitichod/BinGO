#pragma once

#include <Arduino.h>
#include <initializer_list>

// Use the exact class names returned by YOLO. A class belongs in one bin only.
constexpr std::initializer_list<const char *> binClasses[] = {
    {"plastic"},  // Bin 1
    {},                      // Bin 2
    {"general","metal", "paper","organic"},     // Bin 3
    {},                      // Bin 4
};
 
inline uint8_t binForClass(const char *name) {
  uint8_t match = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    for (const char *label : binClasses[i]) {
      if (strcmp(name, label) != 0) continue;
      if (match && match != i + 1) {
        Serial.printf("Sort: class %s is listed in multiple bins\n", name);
        return 0;
      }
      match = i + 1;
    }
  }
  return match;
}
