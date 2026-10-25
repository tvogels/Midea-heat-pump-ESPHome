// Host stub: a ModbusController that only records the queued commands.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace esphome {
namespace modbus_controller {

enum class ModbusRegisterType { HOLDING };

class ModbusController;

using ReadHandler = std::function<void(ModbusRegisterType, uint16_t, const std::vector<uint8_t> &)>;

struct ModbusCommandItem {
  bool is_write;
  uint16_t reg;
  uint16_t value;  // write: the value; read: the register count
  ReadHandler on_data;

  static ModbusCommandItem create_write_single_command(ModbusController *, uint16_t reg, uint16_t value) {
    return {true, reg, value, nullptr};
  }
  static ModbusCommandItem create_read_command(ModbusController *, ModbusRegisterType, uint16_t reg, uint16_t count,
                                               ReadHandler &&handler) {
    return {false, reg, count, std::move(handler)};
  }
};

class ModbusController {
 public:
  void queue_command(const ModbusCommandItem &command) { queue.push_back(command); }
  std::vector<ModbusCommandItem> queue;
};

}  // namespace modbus_controller
}  // namespace esphome
