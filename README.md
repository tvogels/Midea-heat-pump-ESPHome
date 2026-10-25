# Heat pump controller (zwwg)

ESPHome firmware that connects our **Airwell Wellea M HT R290** monobloc (a Midea OEM unit) to Home Assistant over Modbus RTU. It runs on a **LilyGO T-CAN485** (ESP32 + RS485) wired to the heat pump's wired controller.

The site: one zone of underfloor heating in a cement screed (**max 40 °C water**), a DHW tank, no electric backup heaters.

This is a trimmed fork of [Mosibi/Midea-heat-pump-ESPHome](https://github.com/Mosibi/Midea-heat-pump-ESPHome), which supports many models through a generator. We keep one model, written out as plain ESPHome YAML.

> **Disclaimer.** Writing Modbus registers changes how the heat pump runs. Everything here is used at your own risk. Disconnect power from the indoor and outdoor unit before opening anything.

## Layout

```
heatpump.yaml                 entry point: lists the packages below
packages/
  hardware.yaml               board, Wi-Fi, API/OTA, UART, Modbus bus
  controls.yaml               day-to-day controls: power, mode, setpoints, curve (regs 0-18)
  telemetry.yaml              temperatures, compressor, fan, pressures, flow (regs 100-142, 191-192, 703)
  energy.yaml                 energy counters, capacity, COP (regs 143-186)
  status.yaml                 status bits, load outputs, faults, versions (regs 101, 124-131, 187-200, 612-613)
  installer-settings.yaml     installer menu: limits, parameters, curve points (regs 201-279)
  t1s-lease.yaml              opt-in: supervised T1S lease for an external controller
include/
  packed_register.h           safe register writes used by the YAML lambdas
  decoders.h                  fault table and value-to-text decoders
  t1s_lease.h                 lease state machine (only with t1s-lease.yaml)
tests/                        host unit test for include/*.h (+ ESPHome stubs, CI secrets)
docs/
  registers.md                which registers we read and write, and how
  t1s-lease.md                the lease contract
  entity-migration.*          entity renames, for carrying over InfluxDB history
homeassistant/                example dashboard and automations
```

Each package is a normal ESPHome config fragment; ESPHome merges them. There is no generator and no inheritance: what you see is what gets compiled. Register numbers in comments are decimal and refer to the Midea 120L Modbus mapping table V.E.

## Build and flash

Needs [uv](https://docs.astral.sh/uv/). Create `secrets.yaml` next to `heatpump.yaml` (it is git-ignored):

```yaml
wifi_ssid: "..."
wifi_password: "..."
heatpump_fallback_ap_password: "..."
heatpump_ota_password: "..."
heatpump_api_encryption_key: "..."   # 32 random bytes, base64
```

Then:

```sh
uvx --from esphome==2026.6.5 esphome run heatpump.yaml                         # compile + flash (USB or OTA)
uvx --from esphome==2026.6.5 esphome logs heatpump.yaml --device heatpump.local
```

Check before pushing (CI runs the same):

```sh
uvx --from esphome==2026.6.5 esphome config heatpump.yaml > /dev/null
g++ -std=c++17 -Wall -Wextra -Werror -I tests/stubs -I include tests/test_helpers.cpp -o /tmp/t && /tmp/t
```

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs the helper test and compiles the firmware with and without the lease package, using the dummy secrets in `tests/secrets.ci.yaml`.

## Wiring

On the back of the wired controller (the display on the indoor unit), connect the T-CAN485 RS485 terminals: **A+ to H2**, **B- to H1**, ground to **E**. The heat pump is Modbus slave 1 at 9600 baud; every register is polled every 10 s.

![Wired controller](pictures/wired_controller.png)

## How writes work

Many Midea registers pack several settings into one 16-bit word (for example reg 5 holds ECO, silent mode, holiday and weather compensation bits). Modbus can only write a whole word. So an entity that changes one of these fields does a read-modify-write against a *shadow*, a global refreshed by each poll. `hp::write_packed` in `include/packed_register.h` does this for every packed field:

- No write before the register has been read once; the initial 0 would clear every other bit.
- Only the bits of that setting change.
- No write when nothing changes. The heat pump probably stores these settings in non-volatile memory of unknown endurance, so needless writes are avoided.

For the day-to-day controls the heat pump offers single-purpose registers that hold just one field. We write those instead with `hp::write_verified`: it refuses until that register was read once, skips unchanged values, and reads the same register back after writing. The entity itself still shows the field from the packed register:

| Entity | Writes (and reads back) | Entity state from |
| --- | --- | --- |
| Set Water Temperature T1S Zone 1 | reg 11 | reg 2 low byte |
| Power DHW T5S | reg 15 | reg 0 bit 2 |
| Water Flow Temperature Control Zone 1 | reg 16 | reg 0 bit 1 |
| Weather Compensation Curve Zone 1 | reg 18 | reg 6 low byte |

All other settings (mostly installer values) are whole registers that ESPHome writes as-is when you change the entity. [docs/registers.md](docs/registers.md) lists every register the firmware can write.

## Safety limit

The zone 1 heating controls we expose are capped at **40 °C**: *Set Water Temperature T1S Zone 1* (reg 11) and the curve points T1SetH1/H2 (regs 265/266). The screed and floor covering set this limit. Don't raise it without checking them.

This is a limit on those inputs, not an interlock. The floor-drying program (*Highest Drying Temperature*, reg 258) still allows 55 °C, and the lease restores whatever T1S the heat pump had before it took over.

## Supervised T1S lease (opt-in)

`packages/t1s-lease.yaml` lets an external supervisor (Homage) set the zone 1 water target directly for a limited time. If the supervisor goes away, the ESP restores the heat pump's own settings by itself. Enable it by uncommenting its line in `heatpump.yaml`. See [docs/t1s-lease.md](docs/t1s-lease.md).

## Adding a register

1. Find it in the V.E mapping table and pick the package by topic.
2. Add a `modbus_controller` entity with the decimal `address`, and a one-line comment with the register number and scaling.
3. Is it a field inside a packed register? Read it from that register's shadow global, and write it with `hp::write_packed(...)`; copy a neighbouring entity.
4. Run the checks above.

## Credits

Based on the work of Mosibi and contributors to [Midea-heat-pump-ESPHome](https://github.com/Mosibi/Midea-heat-pump-ESPHome) (Apache-2.0, see [LICENSE](LICENSE)).
