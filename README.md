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
| `set_supervised_t1s(t1s, lease_s)` | First call: fresh read of regs 2–5, snapshot of WC-zone-1 bit (reg 5 `0x1000`) and T1S zone 1 (reg 2 low byte) persisted to flash, then WC zone 1 off (only if it was on) and T1S z1 = `t1s`. Later calls: renew the lease; write T1S only if the value changed. |
| `renew_lease(lease_s)` | Renew only. No Modbus traffic, no flash write. |
| `release_supervision()` | Restore now. |

- `t1s` must be within 20–40 °C (UFH cap) and within the unit's T1S heating limits (regs 203/204) when read; otherwise rejected (see *Last Error*). `lease_s` ≥ 10, capped at 7200 s. Lease timing uses wrap-safe `millis()` arithmetic.
- On lease expiry, release, or boot with a pending snapshot (30 s after boot): restore T1S z1, then WC zone 1 on (only if it was on).
- Every write is a masked read-modify-write from a fresh read of that single register (the wired controller shares the bus), is skipped if the bits already match (EEPROM wear), and is verified by read-back after 3 s, up to 3 attempts. Only reg 2 low byte and reg 5 bit 12 are ever written. Register 0 (power/DHW) and regs 6/7/8/212 are never touched.
- Flash is written twice per supervision session (snapshot on start, clear after a verified restore). Renewals never write flash.
- Entities: *T1S Supervision Active*, *T1S Lease Remaining*, *T1S Supervision Last Error*, *T1S Supervision Restore Count*.

Homage pattern: call `set_supervised_t1s(target, 600)` every ~2 min (or `renew_lease(600)`). If Homage dies, the curve comes back within ≤10 min.

Failure modes:
- Snapshot read fails → supervision is not started and nothing is written.
- Takeover write fails → whatever was applied is rolled back.
- Restore fails → snapshot is kept and restore is retried every 60 s (also across reboots). *Supervision Active* stays on.
- Someone changes T1S/WC on the wired controller during a lease → the restore still writes the snapshot values (the snapshot wins).
- `api.reboot_timeout` is left at the default 15 min. A reboot mid-lease simply restores. Consider `0s` once Homage is the only API client.

## ESPHome Midea Heatpump Controller

Together with my son [Sven](https://svenar.nl) who is an electronics student, we created an ESPHome compatible heatpump controller for the Midea heatpump (and clones). This controller can be purchased from https://shop.svenar.nl

<img src="pictures/heatpump_controller_board_v3.png" width="450">
<img src="pictures/heatpump_controller_enclosure_v3.png" width="450">
