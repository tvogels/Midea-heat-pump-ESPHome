// packed_register.h — safe writes to the heat pump's holding registers.
//
// Several Midea registers pack independent settings into one 16-bit word
// (reg 0 power bits, reg 5 function bits, reg 210/211/274 parameter bits,
// reg 270/272 byte and nibble fields). Modbus can only write the whole word,
// so changing one setting is a read-modify-write against a *shadow*: a global
// that every poll of the register refreshes (see capture()).
//
// write_packed() rules:
//   * refuse to write until the shadow holds a value really read from the
//     heat pump (the initial 0 would clobber all other bits);
//   * touch only the bits under the mask;
//   * skip the write when nothing changes (spares the heat pump's EEPROM);
//   * update the shadow right away, so a second change before the next poll
//     builds on the first one.
//
// Registers 11/15/16/18 are single-purpose copies of fields inside regs 0/2/6
// (V.E notes under reg 21). Writing those needs no read-modify-write.
// write_verified() refuses until the register was read once, skips unchanged
// values, writes, and reads the same register back. The read-back updates the
// shadow of the packed register the entity displays.
//
// Whole-register settings (most installer values) don't use these helpers;
// ESPHome writes them directly.
#pragma once

#include "esphome/core/log.h"
#include "esphome/components/modbus_controller/modbus_controller.h"

#include <cstdint>
#include <vector>

namespace hp {

using esphome::modbus_controller::ModbusCommandItem;
using esphome::modbus_controller::ModbusController;
using esphome::modbus_controller::ModbusRegisterType;

// `value` positioned under `mask`. A single-bit mask holds a flag (any
// non-zero value sets it); a wider mask holds a number, e.g. mask 0x0F00
// with value 2 gives 0x0200.
inline uint16_t place(uint16_t mask, uint16_t value) {
  if ((mask & (mask - 1)) == 0) return value ? mask : 0;
  return (uint16_t) (value << __builtin_ctz(mask)) & mask;
}

// Poll lambda for a packed register: remember the raw word and mark the
// shadow as real. Returns x unchanged.
template<typename T> T capture(uint16_t &shadow, bool &valid, T x) {
  shadow = (uint16_t) x;
  valid = true;
  return x;
}

// Set the bits under `mask` in packed register `reg` to `value` (see place()).
// Returns false when the write was refused because the shadow is not valid.
inline bool write_packed(ModbusController *ctrl, uint16_t reg, uint16_t &shadow, bool valid, uint16_t mask,
                         uint16_t value) {
  if (!valid) {
    ESP_LOGW("packed_register", "Refusing write: reg%u not yet read from heatpump, would clobber other bits", reg);
    return false;
  }
  uint16_t new_value = (shadow & ~mask) | place(mask, value);
  if (new_value == shadow) return true;
  shadow = new_value;
  ctrl->queue_command(ModbusCommandItem::create_write_single_command(ctrl, reg, new_value));
  return true;
}

// Write `value` to single-purpose register `reg` and verify it with a
// read-back queued behind the write. `label` names the register in the log.
//   cache/cache_valid    this register's own poll shadow; the write is
//                        skipped when it already holds `value` and refused
//                        before the first poll.
//   packed/packed_valid  the packed register that mirrors this one, with the
//   packed_mask          mirrored field; it is synced from the read-back so it
//                        stays consistent until its next poll.
inline void write_verified(ModbusController *ctrl, uint16_t reg, const char *label, uint16_t value, uint16_t &cache,
                           bool &cache_valid, uint16_t &packed, bool &packed_valid, uint16_t packed_mask) {
  if (cache_valid && cache == value) {
    ESP_LOGD("single_reg", "reg %u already %u, no write", reg, value);
    return;
  }
  if (!cache_valid) {
    ESP_LOGW("single_reg", "Refusing write to reg %u (%s): reg %u not yet read from heatpump", reg, label, reg);
    return;
  }
  ESP_LOGI("single_reg", "Write reg %u (%s) = %u", reg, label, value);
  ctrl->queue_command(ModbusCommandItem::create_write_single_command(ctrl, reg, value));
  // The references point at ESPHome globals, which live for the whole program.
  ctrl->queue_command(ModbusCommandItem::create_read_command(
      ctrl, ModbusRegisterType::HOLDING, reg, 1,
      [reg, value, &cache, &cache_valid, &packed, &packed_valid, packed_mask](
          ModbusRegisterType, uint16_t, const std::vector<uint8_t> &data) {
        if (data.size() < 2) return;
        uint16_t rb = (uint16_t(data[0]) << 8) | data[1];
        if (rb != value)
          ESP_LOGW("single_reg", "Read-back reg %u = %u, expected %u", reg, rb, value);
        else
          ESP_LOGI("single_reg", "Read-back reg %u = %u OK", reg, rb);
        cache = rb;
        cache_valid = true;
        if (packed_valid) packed = (packed & ~packed_mask) | place(packed_mask, rb);
      }));
}

}  // namespace hp
