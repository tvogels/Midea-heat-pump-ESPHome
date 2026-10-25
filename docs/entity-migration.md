# Entity migration (zwwg site model)

`entity-migration.csv` lists every entity whose Home Assistant entity_id, display name or scaling changed in
PR #6 (upstream Mosibi 10.0/10.1 renames and unit fixes), PR #4 (pruning, reg 128 bit 6/7 relabel) and PR #5 (names/scaling). It covers every entity_id change of the full stack #6 → #4 → #5 against site-zwwg 4929f0e (generated-model diff, checked by script). Use it to carry old InfluxDB history over to the new names.

Columns:
- `domain`: the HA domain (`sensor`, `binary_sensor`, `switch`, `select`, `number`). In InfluxDB this is the `domain` tag. `switch->select` marks a domain change (Silent Mode Level, #6); its `transform` maps off→`Level 1`, on→`Level 2`.
  Switches, selects and numbers are not recorded in the InfluxDB bucket (checked); their rows matter for HA dashboards/automations only.
- `old_entity_id` / `new_entity_id`: the object_id **without** the domain prefix. In InfluxDB this is the `entity_id` tag. An empty `new_entity_id` means the entity was removed.
- `register`, `bit`: the Modbus holding register and bit (blank = whole register).
- `transform`: what to apply to the old `value` before writing it under the new id.
  - `identity`: copy the value unchanged.
  - `x*0.1`: multiply by 0.1. Reg 133 is sent as actual×10.
  - `u16->s16 …`: old data was decoded unsigned, so negative values (defrost) appear as ~652–655 after the ×0.01 scale. Map `v > 327.67 ? v - 655.36 : v`.
  - `delete: …`: do not import. The values are a sentinel (reg 111 = 25 means unavailable).
- `valid_from_notes`: which deploy introduces the change. Rows apply to data **before** that deploy.

## Applying to InfluxDB (v2 / Flux)

Rules:
- Always work **one entity at a time and one month at a time**. Never run a delete or rewrite across the whole bucket or across all entities.
- Do a dry run first: run the query without `to()` and compare counts and min/max.
- Write the converted data under the *new* entity_id. Keep the old series until you have checked the result, then delete it per entity and month if you want to.

Example: rescale reg 133 for September 2026.

```flux
from(bucket: "homeassistant")
  |> range(start: 2026-09-01T00:00:00Z, stop: 2026-10-01T00:00:00Z)
  |> filter(fn: (r) => r.domain == "sensor" and r.entity_id == "heatpump_controller_dc_bus_current" and r._field == "value")
  |> map(fn: (r) => ({r with _value: r._value * 0.1}))
  // |> to(bucket: "homeassistant")   // uncomment after dry run
```

When the id changes, set the new tag as well, e.g.
`|> map(fn: (r) => ({r with entity_id: "heatpump_controller_hydraulic_module_capacity", _value: if r._value > 327.67 then r._value - 655.36 else r._value}))`.
Copy the `friendly_name` tag over to the new display name too, if you use that tag.

Deleting old series (per entity, per month only):

```sh
influx delete --bucket homeassistant \
  --start 2026-09-01T00:00:00Z --stop 2026-10-01T00:00:00Z \
  --predicate '_measurement="A" AND entity_id="heatpump_controller_dc_bus_current"'
```

(Use the actual `_measurement` of the series. HA writes the unit, e.g. `A`, `kWh`, `°C`, or `units`/`state` for unitless entities. For entities whose unit changed (reg 123 kWh→kW from #6, 139 kW→none, reg 10 hr→none; number reg 271 min→hr from #6), the new series also lands under a new measurement.)
