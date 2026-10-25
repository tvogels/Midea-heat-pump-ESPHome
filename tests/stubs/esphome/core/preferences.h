// Host stub: preferences that keep one saved blob in memory.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome {

class ESPPreferenceObject {
 public:
  template<typename T> bool save(const T *src) {
    data_.assign(reinterpret_cast<const uint8_t *>(src), reinterpret_cast<const uint8_t *>(src) + sizeof(T));
    return true;
  }
  template<typename T> bool load(T *dest) {
    if (data_.size() != sizeof(T)) return false;
    std::memcpy(dest, data_.data(), sizeof(T));
    return true;
  }

 private:
  std::vector<uint8_t> data_;
};

class ESPPreferences {
 public:
  template<typename T> ESPPreferenceObject make_preference(uint32_t /*type*/, bool /*in_flash*/) { return {}; }
  bool sync() { return true; }
};

inline ESPPreferences *global_preferences = new ESPPreferences();

}  // namespace esphome
