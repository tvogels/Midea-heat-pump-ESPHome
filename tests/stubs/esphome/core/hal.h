// Host stub: millis() returns a clock the tests can set.
#pragma once

#include <cstdint>

namespace esphome {

inline uint32_t &fake_millis() {
  static uint32_t now = 0;
  return now;
}

inline uint32_t millis() { return fake_millis(); }

}  // namespace esphome
