// decoders.h — turn raw heat pump register values into readable text.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace hp {

// Fault numbers as reported in regs 124 (current fault) and 125-127 (fault
// history), with the code shown on the wired controller and its meaning.
struct Fault {
  uint8_t number;
  const char *code;
  const char *description;
};

static const Fault FAULTS[] = {
    {  0, "OK", "OK"},
    {  1, "E0", "Water flow fault(E8 displayed 3 times)"},
    {  2, "E1", "Phase loss or neutral wire and live wire are connected reversely(only for three phase unit)"},
    {  3, "E2", "Communication fault between controller and hydraulic module"},
    {  4, "E3", "Final outlet water temperature sensor(T1) fault"},
    {  5, "E4", "Water tank temperature sensor(T5) fault"},
    {  6, "E5", "The condenser outlet refrigerant temperature sensor(T3) fault"},
    {  7, "E6", "The ambient temperature sensor(T4) fault"},
    {  8, "E7", "Buffer tank up temperature sensor(Tbt1) fault"},
    {  9, "E8", "Water flow failure"},
    { 10, "E9", "Suction temperature sensor (Th) fault"},
    { 11, "EA", "Discharge temperature sensor (Tp) fault"},
    { 12, "Eb", "Solar temperature sensor(Tsolar) fault"},
    { 13, "Ec", "Buffer tank low temperature sensor(Tbt2) fault"},
    { 14, "Ed", "Inlet water temperature sensor(Tw_in) malfunction"},
    { 15, "EE", "Hydraulic module EEprom failure"},
    { 20, "P0", "Low pressure switch protection"},
    { 21, "P1", "High pressure switch protection"},
    { 23, "P3", "Compressor overcurrent protection"},
    { 24, "P4", "High discharge temperature protection"},
    { 25, "P5", "|Tw_out - Tw_in| value too big protection"},
    { 26, "P6", "Inverter module protection"},
    { 31, "Pb", "Anti-freeze mode"},
    { 33, "Pd", "High temperature protection of refrigerant outlet temperature of condenser"},
    { 38, "PP", "Tw_out - Tw_in unusual protection"},
    { 39, "H0", "Communication fault between main board PCB B and main control board of hydraulic module"},
    { 40, "H1", "Communication fault between inverter module PCB A and main control board PCB B"},
    { 41, "H2", "Refrigerant liquid temperature sensor(T2) fault"},
    { 42, "H3", "Refrigerant gas temperature sensor(T2B) fault"},
    { 43, "H4", "Three times P6(L0/L1) protection"},
    { 44, "H5", "Room temo. sensor (Ta) fault"},
    { 45, "H6", "DC fan motor fault"},
    { 46, "H7", "Voltage protection"},
    { 47, "H8", "Pressure sensor fault"},
    { 48, "H9", "Outlet water for zone 2 temperature sensor(Tw2) fault"},
    { 49, "HA", "Outlet water temperature sensor(Tw_out) fault"},
    { 50, "Hb", "3 times PP protection and Tw_out<7℃"},
    { 52, "Hd", "Communication fault between hydraulic module parallel"},
    { 53, "HE", "Communication error between main board and thermostat transfer board"},
    { 54, "HF", "Inverter module board EE PROM fault"},
    { 55, "HH", "H6 display 10 times in 2 hours"},
    { 57, "HP", "Low pressure protection (Pe<0.6) occurred 3 times in 1 hour"},
    { 65, "C7", "Transducer module temperature too high protection"},
    {112, "bH", "PED PCB fault"},
    {116, "F1", "Low DC generatrix voltage protection"},
    {134, "L0", "Module protection"},
    {135, "L1", "DC generatrix low voltage protection"},
    {136, "L2", "DC generatrix high voltage protection"},
    {138, "L4", "MCE fault"},
    {139, "L5", "Zero speed protection"},
    {141, "L7", "Phase sequence fault"},
    {142, "L8", "Speed difference > 15Hz protection between the front and the back clock"},
    {143, "L9", "Speed difference > 15Hz protection between the real and the setting speed"},
};

// Numbers 0-143 without a table entry are returned as the number itself.
inline std::string fault_lookup(int number, bool description) {
  if (number < 0 || number > 143) return "Unknown";
  for (const Fault &f : FAULTS)
    if (f.number == number) return description ? f.description : f.code;
  return std::to_string(number);
}
inline std::string fault_code(int number) { return fault_lookup(number, false); }
inline std::string fault_description(int number) { return fault_lookup(number, true); }

// Reg 101: current operating mode.
inline std::string operating_mode(uint16_t value) {
  switch (value) {
    case 0: return "OFF";
    case 2: return "Cooling";
    case 3: return "Heating";
    case 5: return "DHW Heating";
    default: return "Unknown: " + std::to_string(value);
  }
}

// Reg 199: heat pump operation mode.
inline std::string heat_pump_operation_mode(uint16_t value) {
  switch (value) {
    case 0: return "Off";
    case 2: return "Cooling";
    case 3: return "Heating";
    case 5: return "DHW";
    default: return "Invalid";
  }
}

// Reg 187: Modbus protocol version, 46 = V4.6.
inline std::string protocol_version(uint16_t value) {
  if (value == 0 || value == 0x7FFF) return "Unknown";
  char buf[16];
  snprintf(buf, sizeof(buf), "V%d.%d", value / 10, value % 10);
  return buf;
}

// Reg 190: hydraulic module sub-model.
inline std::string hydraulic_submodel(uint16_t value) {
  static const char *const NAMES[] = {"R32-P",    "Aqua",  "C-R32-P", "R290-A", "R290-N", "C-R290-A",
                                      "C-R290-N", "R32-A", "C-R32-A", "R290-M", "R32-H"};
  if (value < sizeof(NAMES) / sizeof(NAMES[0])) return NAMES[value];
  return "Unknown";
}

// Reg 272 nibble: emitter type of a zone (heating or cooling end).
inline std::string emission_type(uint16_t nibble) {
  switch (nibble) {
    case 0: return "Underfloor Heating";
    case 1: return "Fan Coil Unit";
    case 2: return "Radiator";
    default: return "Unknown: " + std::to_string(nibble);
  }
}

// What the heat pump is doing right now, combined from several registers.
//   mode         "Operating Mode" text (reg 101)
//   running      load output "RUN" (reg 129)
//   defrosting   status bit "Defrosting" (reg 128)
//   oil_return   status bit "Oil return" (reg 128)
//   sv1          load output "SV1", 3-way valve to the DHW tank (reg 129)
inline std::string active_state(const std::string &mode, bool running, bool defrosting, bool oil_return, bool sv1) {
  if (mode == "OFF") return "Inactive";
  // On but not running: e.g. between heating cycles or preparing a DHW run.
  if (!running) return "Idle";
  if (defrosting) return "Defrosting";
  if (oil_return) return "Oil return";
  if (sv1) return "DHW";
  return mode;  // "Heating" or "Cooling"
}

// Numeric code of active_state(), for graphs. 99 = unknown.
inline int active_state_code(const std::string &state) {
  static const char *const STATES[] = {"Inactive", "Heating", "Cooling", "DHW", "Defrosting", "Idle", "Oil return"};
  for (int i = 0; i < (int) (sizeof(STATES) / sizeof(STATES[0])); i++)
    if (state == STATES[i]) return i;
  return 99;
}

}  // namespace hp
