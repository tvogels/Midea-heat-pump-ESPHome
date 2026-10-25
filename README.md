# Midea heat pump (and clones) ESPHome

The Midea heat pump and clones like [Airwell](https://www.airwell.com/en/), [Artel](https://www.artelgroup.com/products-heat-pumps/), [Ferroli](https://www.ferroli.com/int/products/hydronic-heat-pumps-cooling-heating-dhw), [Kaisai](https://www.kaisaisystems.nl), [Inventor](https://www.inventorairconditioner.com/heat-pumps-split-monoblock/split-type-heat-pumps/matrix-split-type), [Kaysun](https://www.kaysun.es/en/products-kaysun/aquantia/), [YORK](https://www.pompycieplayork.pl/produkty/) can be managed using the Modbus protocol. This project contains configuration file(s) which can be placed on an ESPHome enabled device to communicatie using modbus with the heat pump.

<span style="color: red;">**See below for a ready-made ESPHome compatible heatpump controller!!**</span>


<a href="https://www.buymeacoffee.com/Mosibi" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/default-yellow.png" alt="Buy Me A Coffee" height="41" width="174"></a>

## Disclaimer

The information provided for the heat pump is intended for informational purposes only. Its use is entirely at your own risk. We do not take any responsibility for any damage or loss caused by the use of information taken from this project. We strongly recommend that you consult with a professional HVAC technician before making any changes to your heat pump configuration or installation. By using the information from this project, you acknowledge that you understand and accept these terms and conditions.

## Connect a ESPHome board to the heat pump

Connecting a device to the heat pump is not that difficult, but keep the disclaimer in mind. Before you start, disconnect the power from the indoor and the outdoor unit. Never ever open up the heat pump with the power switched on!!

A modbus device has two wires, A+ and B- and optional a ground/earth connection. The modbus device needs to be connected to the "wired controller", this is the display on the model shown.

![Indoor Unit](pictures/Midea-M-Thermal-Arctic.png)

Detach the front panel of the indoor unit (slide it up) and on the backside you will find a panel that holds the display (wired controller). Open that panel, be warned that the screws used are not alway of the best quality, so use a good fitting screw driver and be gentle.

When the panel of the display is removed, the back of the display is visible, which is a printed circuit board (PCB). On that board, search for the 'earth' connection (letter E) and connections H1 and H2. Attach the earth wire of your modbus device to the earth connection, the B- wire to H1 and A+ wire to H2.

![Wired Controller](pictures/wired_controller.png)

When all is connected, place the back panel of the display back and put back the front panel of the indoor unit.

## Configuration

In the `models` directory you will find multiple yaml files for specific models and more generic ones. Place the content of the model file which is the best fit for your heat pump in your ESPHome device and change the `uart` and `modbus_controller` settings to your needs. The `substitutions` section can be used to change the entities name as they apear in Home Assistant. In the [homeassistant](homeassistant) directory I placed and example dashboard and some example automations.

*If you are not able to find a good fit in the models directory for your heat pump, then try the `R32-generic.yaml`, that will probably work.*

## Supervised T1S lease (site-zwwg)

`models/R290-airwell-wellea-m-ht-zwwg-lease.yaml` wraps the zwwg model (unchanged) and adds an on-device safety net for an external supervisor (Homage) that sets the Zone 1 supply temperature directly. Logic: `models/t1s_lease.h`. Flashing the plain zwwg model leaves behaviour unchanged.

Native API actions (callable with aioesphomeapi, no Home Assistant needed):

| Action | Effect |
| --- | --- |
| `set_supervised_t1s(t1s, lease_s)` | First call: fresh read of regs 2–5, snapshot of WC-zone-1 bit (reg 5 `0x1000`) and T1S zone 1 (reg 2 low byte) persisted to flash, then WC zone 1 off (only if it was on), then T1S z1 = `t1s`. Later calls: renew the lease and set the *target*; the target is written subject to the rate limit below. |
| `renew_lease(lease_s)` | Renew only. No Modbus traffic, no flash write. |
| `release_supervision()` | Restore now. When the restore has halted (see below), re-arms one more retry series, at most once per hour. |

`set_supervised_t1s` and `renew_lease` are rejected (see *Last Error*) while a release is pending (an op is still in flight before the restore) and while a restore is owed. *Last Error* is cleared after a verified takeover or a verified restore.

- `t1s` must be within 20–40 °C (UFH cap) and within the unit's T1S heating limits (regs 203/204) when read; otherwise rejected (see *Last Error*). `lease_s` ≥ 10, capped at 7200 s. Lease timing uses wrap-safe `millis()` arithmetic.
- Every write is a masked read-modify-write from a fresh read of that single register (the wired controller shares the bus), is skipped if the bits already match, and is verified by read-back after 3 s, up to 3 attempts (15 s response timeout). The stock shadow globals are only updated from real reads, never optimistically. Only reg 2 low byte and reg 5 bit 12 are ever written. Registers 0/6/7/8/212 are never written.
- State is a single mode: `idle → takeover → supervising → restoring (→ restore_backoff → restore_halted)`, shown by *T1S Supervision Mode*.

### Heat-pump EEPROM writes

The heat pump stores reg 2/5 in EEPROM. Writes per session:

- Takeover: 1 (WC off, only if it was on) + 1 (T1S, only if different).
- Restore: 1 (WC on, only if it was on) + 1 (T1S back, only if different).
- So ≥4 per session when WC was on and T1S changes (2 if WC was already off), plus one per mid-lease T1S change.
- Mid-lease T1S writes are rate limited to one per `t1s_min_write_interval_s` (substitution, default 900 s = 15 min). The latest target is always the one written; requests equal to the applied value write nothing. Worst case 4/h = 96/day ≈ 35k/year. Homage duty planning with ~1 compressor start/h needs about 2 T1S changes/h ≈ 48/day ≈ 17.5k/year, plus session writes. With a typical ~100k-cycle EEPROM rating, keep the interval ≥15 min unless the unit's real endurance is known.
- The same interval also gates the takeover T1S write, across sessions: `release → set` or `drift → set` churn cannot bypass it. A gated takeover write is deferred (T1S stays as is) and written at the next slot while the lease runs. Restore writes are never gated but count as the last T1S write. Homage must still not churn sessions: each takeover/restore pair costs up to 2 reg 5 writes, which are not rate limited.
- Mid-lease T1S write not verified (all read-backs timed out): both the last verified value and the target are accepted as expected until a real read decides, so a landed write is not mistaken for a user change.
- *T1S Heat Pump Write Count* counts every reg 2/5 write made by this code since boot.

ESP flash (NVS) is separate: written twice per session (snapshot on start, clear after a verified restore or an adopted external change). Renewals never write flash or Modbus.

### Failure modes and retries

- Snapshot read fails → supervision is not started and nothing is written.
- Takeover write fails → whatever was applied is rolled back.
- Mid-lease T1S write fails → the lease is kept, *Last Error* is set, and the write is retried at the next rate-limit slot.
- Restore: WC is restored first, then T1S. The ops are independent: a failed op does not block the other. Only failed ops are retried, with exponential backoff (60 s, 120 s, … capped at 1 h). Each retry cycle *does* write again (≤3 attempts per failed op). After 8 failed cycles (~2 h) the ESP stops writing, enters `restore_halted`, keeps the snapshot pending (restored again after reboot) and sets a persistent *Last Error*. A snapshot T1S outside 5–65 °C is not written back.
- External change during a lease (HA *Weather Compensation Zone 1* switch or T1S number, or the wired controller): each stock 10 s poll is compared with the expected state (WC z1 off, T1S z1 = last verified value), after a 30 s grace period following our own I/O. On drift the ESP adopts the user's change and supervision ends; *Last Error* / *T1S External Change Count* report it:
  - WC zone 1 switched on: everything is adopted, the snapshot is cleared, nothing is written.
  - T1S only: the user's T1S is adopted (never written back), but WC zone 1 is still restored if it was on before the lease (we turned it off, not the user). The snapshot stays pending with "skip T1S" until that WC restore is verified.

  Homage must treat this as "user took over" and not immediately call `set_supervised_t1s` again.
- Known race (inherent): if HA or the wired controller writes T1S between our fresh read and our write (one Modbus round trip during takeover or a mid-lease write), their value is overwritten and no drift is reported.
- DHW observation: transitions of the reg 0 DHW bit (`0x0004`, read from the stock poll, never written) within 30 s of a lease write are logged and counted in *T1S DHW Drops After Lease Write*.
- `api.reboot_timeout` is left at the default 15 min. A reboot mid-lease simply restores. Consider `0s` once Homage is the only API client.

Entities: *T1S Supervision Active*, *T1S Supervision Mode*, *T1S Lease Remaining*, *T1S Supervision Last Error*, *T1S Supervision Restore Count*, *T1S Heat Pump Write Count*, *T1S External Change Count*, *T1S DHW Drops After Lease Write*. Counters are since boot. All status entities also publish immediately on transitions.

Homage pattern: call `set_supervised_t1s(target, 600)` every ~2 min (or `renew_lease(600)`). If Homage dies, the curve comes back within ≤10 min.

## ESPHome Midea Heatpump Controller

Together with my son [Sven](https://svenar.nl) who is an electronics student, we created an ESPHome compatible heatpump controller for the Midea heatpump (and clones). This controller can be purchased from https://shop.svenar.nl

<img src="pictures/heatpump_controller_board_v3.png" width="450">
<img src="pictures/heatpump_controller_enclosure_v3.png" width="450">
