// Host unit test for include/packed_register.h and include/decoders.h.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -I tests/stubs -I include tests/test_helpers.cpp -o test_helpers
//   ./test_helpers
#include "packed_register.h"
#include "decoders.h"

#include <cstdio>
#include <string>

using esphome::modbus_controller::ModbusController;

static int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static bool last_log_starts_with(const std::string &prefix) {
  return !log_lines().empty() && log_lines().back().rfind(prefix, 0) == 0;
}

static void test_place() {
  CHECK(hp::place(0x0001, 1) == 0x0001);
  CHECK(hp::place(0x0004, 0) == 0x0000);
  CHECK(hp::place(0x0002, 5) == 0x0002);  // flag: any non-zero sets the bit
  CHECK(hp::place(0x00FF, 0x01FF) == 0x00FF);
  CHECK(hp::place(0xFF00, 12) == 0x0C00);
  CHECK(hp::place(0x0F00, 2) == 0x0200);
  CHECK(hp::place(0x000F, 1) == 0x0001);
}

static void test_capture() {
  uint16_t shadow = 0;
  bool valid = false;
  CHECK(hp::capture(shadow, valid, 4660.0f) == 4660.0f);
  CHECK(shadow == 0x1234 && valid);
}

static void test_write_packed() {
  ModbusController ctrl;
  uint16_t reg5 = 0;
  bool valid = false;

  // Refused before the first poll: the shadow's 0 would clear every other bit.
  CHECK(!hp::write_packed(&ctrl, 5, reg5, valid, 0x80, true));
  CHECK(ctrl.queue.empty());
  CHECK(last_log_starts_with("W packed_register: Refusing write: reg5 not yet read"));

  reg5 = 0x1010;
  valid = true;
  CHECK(hp::write_packed(&ctrl, 5, reg5, valid, 0x80, true));
  CHECK(ctrl.queue.size() == 1 && ctrl.queue[0].is_write && ctrl.queue[0].reg == 5 && ctrl.queue[0].value == 0x1090);
  CHECK(reg5 == 0x1090);

  // No change, no write.
  CHECK(hp::write_packed(&ctrl, 5, reg5, valid, 0x80, true));
  CHECK(ctrl.queue.size() == 1);

  CHECK(hp::write_packed(&ctrl, 5, reg5, valid, 0x1000, false));
  CHECK(ctrl.queue.size() == 2 && ctrl.queue[1].value == 0x0090 && reg5 == 0x0090);

  // Reg 272 nibbles and reg 270 bytes.
  uint16_t reg272 = 0x1234;
  CHECK(hp::write_packed(&ctrl, 272, reg272, true, 0x0F00, 2));  // already 2
  CHECK(ctrl.queue.size() == 2);
  CHECK(hp::write_packed(&ctrl, 272, reg272, true, 0x0F00, 1));
  CHECK(reg272 == 0x1134 && ctrl.queue.back().reg == 272);
  CHECK(hp::write_packed(&ctrl, 272, reg272, true, 0x000F, 0));
  CHECK(reg272 == 0x1130);

  uint16_t reg270 = 0x0C06;
  CHECK(hp::write_packed(&ctrl, 270, reg270, true, 0x00FF, 8));
  CHECK(reg270 == 0x0C08);
  CHECK(hp::write_packed(&ctrl, 270, reg270, true, 0xFF00, 3));
  CHECK(reg270 == 0x0308 && ctrl.queue.back().value == 0x0308);
}

static void test_write_verified() {
  ModbusController ctrl;
  uint16_t reg16 = 0, reg0 = 0x0005;
  bool reg16_valid = false, reg0_valid = true;

  hp::write_verified(&ctrl, 16, "power zone 1", 1, reg16, reg16_valid, reg0, reg0_valid, 0x2);
  CHECK(ctrl.queue.empty());
  CHECK(last_log_starts_with("W single_reg: Refusing write to reg 16 (power zone 1)"));

  reg16_valid = true;
  hp::write_verified(&ctrl, 16, "power zone 1", 0, reg16, reg16_valid, reg0, reg0_valid, 0x2);
  CHECK(ctrl.queue.empty());  // already 0
  CHECK(last_log_starts_with("D single_reg: reg 16 already 0, no write"));

  hp::write_verified(&ctrl, 16, "power zone 1", 1, reg16, reg16_valid, reg0, reg0_valid, 0x2);
  CHECK(ctrl.queue.size() == 2);
  CHECK(ctrl.queue[0].is_write && ctrl.queue[0].reg == 16 && ctrl.queue[0].value == 1);
  CHECK(!ctrl.queue[1].is_write && ctrl.queue[1].reg == 16 && ctrl.queue[1].value == 1);
  CHECK(reg16 == 0);  // only the read-back updates the cache

  ctrl.queue[1].on_data(esphome::modbus_controller::ModbusRegisterType::HOLDING, 16, {0x00, 0x01});
  CHECK(reg16 == 1 && reg0 == 0x0007);
  CHECK(last_log_starts_with("I single_reg: Read-back reg 16 = 1 OK"));

  // Read-back disagrees: cache and mirror follow the heat pump, not the request.
  ctrl.queue[1].on_data(esphome::modbus_controller::ModbusRegisterType::HOLDING, 16, {0x00, 0x00});
  CHECK(reg16 == 0 && reg0 == 0x0005);
  CHECK(last_log_starts_with("W single_reg: Read-back reg 16 = 0, expected 1"));

  // Short reply is ignored.
  ctrl.queue[1].on_data(esphome::modbus_controller::ModbusRegisterType::HOLDING, 16, {0x01});
  CHECK(reg16 == 0);

  // Reg 11 mirrors into the low byte of reg 2 (zone 2 in the high byte stays).
  ModbusController ctrl2;
  uint16_t reg11 = 35, reg2 = 0x1E23;
  bool reg11_valid = true, reg2_valid = true;
  hp::write_verified(&ctrl2, 11, "T1S zone 1", 33.0f, reg11, reg11_valid, reg2, reg2_valid, 0x00FF);
  CHECK(ctrl2.queue.size() == 2 && ctrl2.queue[0].value == 33);
  ctrl2.queue[1].on_data(esphome::modbus_controller::ModbusRegisterType::HOLDING, 11, {0x00, 0x21});
  CHECK(reg11 == 33 && reg2 == 0x1E21);

  // Packed shadow not read yet: leave it alone.
  reg2_valid = false;
  reg2 = 0;
  ctrl2.queue[1].on_data(esphome::modbus_controller::ModbusRegisterType::HOLDING, 11, {0x00, 0x22});
  CHECK(reg11 == 34 && reg2 == 0);
}

static void test_decoders() {
  CHECK(hp::fault_code(0) == "OK");
  CHECK(hp::fault_code(1) == "E0");
  CHECK(hp::fault_code(143) == "L9");
  CHECK(hp::fault_code(16) == "16");  // in range, no table entry
  CHECK(hp::fault_code(144) == "Unknown");
  CHECK(hp::fault_code(-1) == "Unknown");
  CHECK(hp::fault_description(0) == "OK");
  CHECK(hp::fault_description(20) == "Low pressure switch protection");
  CHECK(hp::fault_description(16) == "16");

  CHECK(hp::operating_mode(3) == "Heating");
  CHECK(hp::operating_mode(7) == "Unknown: 7");
  CHECK(hp::heat_pump_operation_mode(5) == "DHW");
  CHECK(hp::heat_pump_operation_mode(1) == "Invalid");
  CHECK(hp::protocol_version(46) == "V4.6");
  CHECK(hp::protocol_version(0x7FFF) == "Unknown");
  CHECK(hp::hydraulic_submodel(9) == "R290-M");
  CHECK(hp::hydraulic_submodel(11) == "Unknown");
  CHECK(hp::emission_type(0) == "Underfloor Heating");
  CHECK(hp::emission_type(3) == "Unknown: 3");

  CHECK(hp::active_state("OFF", true, true, false, false) == "Inactive");
  CHECK(hp::active_state("Heating", false, true, false, false) == "Idle");
  CHECK(hp::active_state("Heating", true, true, true, true) == "Defrosting");
  CHECK(hp::active_state("Heating", true, false, true, true) == "Oil return");
  CHECK(hp::active_state("Heating", true, false, false, true) == "DHW");
  CHECK(hp::active_state("Cooling", true, false, false, false) == "Cooling");
  CHECK(hp::active_state_code("Inactive") == 0);
  CHECK(hp::active_state_code("Oil return") == 6);
  CHECK(hp::active_state_code("Unknown: 4") == 99);
}

int main() {
  test_place();
  test_capture();
  test_write_packed();
  test_write_verified();
  test_decoders();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
