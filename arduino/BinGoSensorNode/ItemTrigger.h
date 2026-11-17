#pragma once

#include <stdint.h>

// All readings in the settling window must stay within this span.
constexpr float itemMinGrams = 10;
constexpr float itemStableSpanGrams = 4;
constexpr uint32_t itemSettleMs = 1200;
constexpr float emptyTrayMaxGrams = 5;
constexpr uint32_t emptyTrayHoldMs = 400;

struct ItemTrigger {
  uint32_t sequence = 0;
  uint32_t itemId = 0;
  bool settling = false;
  bool clearing = false;
  bool stable = false;
  uint32_t settledFrom = 0;
  uint32_t emptyFrom = 0;
  float lowGrams = 0;
  float highGrams = 0;

  void pause() {
    settling = clearing = stable = false;
  }

  void sample(float grams, uint32_t now) {
    if (grams <= emptyTrayMaxGrams) {
      settling = stable = false;
      if (!clearing) {
        clearing = true;
        emptyFrom = now;
      }
      if (now - emptyFrom >= emptyTrayHoldMs) itemId = 0;
      return;
    }
    clearing = false;
    if (grams < itemMinGrams) {
      settling = stable = false;
      return;
    }

    if (!settling || grams < highGrams - itemStableSpanGrams ||
        grams > lowGrams + itemStableSpanGrams) {
      settling = true;
      stable = false;
      settledFrom = now;
      lowGrams = highGrams = grams;
      return;
    }
    if (grams < lowGrams) lowGrams = grams;
    if (grams > highGrams) highGrams = grams;
    stable = now - settledFrom >= itemSettleMs;
    if (stable && !itemId) {
      if (++sequence == 0) ++sequence;
      itemId = sequence;
    }
  }

  uint32_t readyItem() const {
    return stable ? itemId : 0;
  }
};
