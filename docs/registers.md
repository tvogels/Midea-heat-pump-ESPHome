# Register map

All registers are Modbus **holding** registers on slave 1, numbered in decimal as in the Midea 120L Modbus mapping table V.E. Everything is read with FC03 every 10 s and written with FC06 (single register).

## What is read where

| Package | Registers |
| --- | --- |
| `controls.yaml` | 0–7, 10, 11, 15, 16, 18 |
| `telemetry.yaml` | 100, 102–140, 191–192, 703 |
| `energy.yaml` | 143–186 |
| `status.yaml` | 101, 124–131, 187–190, 198–200, 612–613 |
| `installer-settings.yaml` | 201–279 |

## What can be written, and how

There are four write styles:

- **direct**: ESPHome writes the entity value (after scaling) as the whole register.
- **packed**: one field inside a shared register. `hp::write_packed` does a read-modify-write on the shadow global: it refuses until the register was read once, changes only the masked bits, and skips the write if nothing changes.
- **verified**: a single-purpose register. `hp::write_verified` refuses until the readback was read once, skips unchanged values, writes, then reads the register back.
- **custom**: a hand-written `write_lambda`.

### Day-to-day (`controls.yaml`)

| Reg | Mask | Style | Entity |
| --- | --- | --- | --- |
| 0 | `0x0001` | packed | Room Temperature Control |
| 1 | | direct | Operational Mode (Auto 1, Cool 2, Heat 3) |
| 3 | | direct | Air Temperature Ts (×2) |
| 4 | | direct | Set DHW Tank Temperature T5s |
| 5 | `0x0010` | packed | Disinfect |
| 5 | `0x0040` | packed | Silent Mode |
| 5 | `0x0080` | packed | Silent Mode Level |
| 5 | `0x0100` | packed | Holiday Home |
| 5 | `0x0400` | packed | ECO Mode |
| 5 | `0x1000` | packed | Weather Compensation Zone 1 |
| 7 | | custom | Forced Water Tank Heating (writes 1 = start, 2 = stop) |
| 11 | | verified | Set Water Temperature T1S Zone 1 (5–40 °C) |
| 15 | | verified | Power DHW T5S |
| 16 | | verified | Water Flow Temperature Control Zone 1 |
| 18 | | verified | Weather Compensation Curve Zone 1 (1–9) |

Regs 11/15/16/18 are single-purpose copies of fields in the packed regs 2/0/0/6. The V.E table recommends them for control. They are read back from the packed registers:

| Writes | Read back from |
| --- | --- |
| reg 11 | reg 2 low byte (`0x00FF`) |
| reg 15 | reg 0 bit `0x0004` |
| reg 16 | reg 0 bit `0x0002` |
| reg 18 | reg 6 low byte (`0x00FF`) |

The write also updates that packed shadow, so the entity is right before the next poll. The *… Readback (reg NN)* diagnostic sensors show the raw single-purpose registers.

### Installer settings (`installer-settings.yaml`)

Packed:

| Reg | Mask | Entity |
| --- | --- | --- |
| 210 | `0x0001` | Parameter Setting 1 Heating And Cooling First Or Water First |
| 210 | `0x0002` | … Dual Room Thermostat Supported |
| 210 | `0x0004` | … Room Thermostat Mode Set |
| 210 | `0x0008` | … Supports Room Thermostat |
| 210 | `0x0010` | … Supports Room Temperature Sensor Ta |
| 210 | `0x0020` | … PUMPI silent mode |
| 210 | `0x0080` | … Enable Heating |
| 210 | `0x0200` | … Enable Cooling |
| 210 | `0x2000` | … Supports Disinfection |
| 210 | `0x8000` | … Enable Water Heating |
| 211 | `0x0004` | Parameter Setting 2 Ta Sensor Position |
| 211 | `0x0080` | … Smart Grid |
| 211 | `0x0100` | … M1M2 Function |
| 211 | `0x0800` | … Piping Length Selection |
| 211 | `0x2000` | … Enable Temperature Collection Kit |
| 270 | `0x00FF` | t_T4 FRESH_H (×2) |
| 270 | `0xFF00` | t_T4 FRESH_C (×2) |
| 272 | `0x000F` | Zone 1 End Heating Mode Emission Type |
| 272 | `0x0F00` | Zone 1 End Cooling Mode Emission Type |
| 274 | `0x0001` | EnSwitchPDC |

Direct, one whole register each: 212, 213, 215, 216, 220–222, 224–232, 241–246, 255–269, 271, 279. Notable ones:

- 261–268: weather curve points. T1SetH1/H2 (265/266) are capped at 40 °C.
- 269: Power Input Limitation Type.
- 258 Highest Drying Temperature (screed drying program) still allows up to 55 °C. Only use the drying program with the floor's limits in mind.

### Lease (`t1s-lease.yaml`, opt-in)

Writes only reg 11 (whole value) and reg 5 bit `0x1000`, each with a fresh read first and a read-back after. See [t1s-lease.md](t1s-lease.md).

## Adding a writable field to a packed register

```yaml
switch:
  - platform: template
    name: "Some Option"
    entity_category: config
    restore_mode: DISABLED
    optimistic: true
    lambda: "return (id(reg5) & 0x200) == 0x200;"
    on_turn_on:
      - lambda: hp::write_packed(heatpump, 5, id(reg5), id(reg5_valid), 0x200, true);
    on_turn_off:
      - lambda: hp::write_packed(heatpump, 5, id(reg5), id(reg5_valid), 0x200, false);
```

The register must already be read into its shadow (`regN` / `regN_valid` globals, set by a `modbus_controller` sensor with `hp::capture`). If it isn't, add the globals and that sensor first; reg 0 in `controls.yaml` is the example.
