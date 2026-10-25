# Supervised T1S lease

`packages/t1s-lease.yaml` (logic in `include/t1s_lease.h`) is an on-device safety net for an external supervisor (Homage) that sets the zone 1 water temperature (T1S) directly. The supervisor holds a *lease* and must keep renewing it. If it stops (crash, network loss, ESP reboot), the ESP restores the heat pump's own settings by itself.

It is opt-in. To enable it, uncomment this line in `heatpump.yaml`:

```yaml
  # - !include packages/t1s-lease.yaml
```

Without it, the firmware behaves exactly as before.

## API actions

Native ESPHome API actions, callable with aioesphomeapi (no Home Assistant needed):

| Action | Effect |
| --- | --- |
| `set_supervised_t1s(t1s, lease_s)` | **First call:** fresh read of reg 5 and reg 11. The weather-compensation bit for zone 1 (reg 5 `0x1000`) and T1S zone 1 (reg 11) are snapshotted to ESP flash. Then WC zone 1 is turned off (only if it was on) and T1S is set to `t1s`. **Later calls:** renew the lease and set the *target*. The target is written subject to the rate limit below. |
| `renew_lease(lease_s)` | Renew only. No Modbus traffic, no flash write. |
| `release_supervision()` | Restore now. If the restore has halted (see below), this re-arms one more retry series, at most once per hour. |

`set_supervised_t1s` and `renew_lease` are rejected (see *Last Error*) while a release is pending (an operation is still in flight before the restore) and while a restore is owed. *Last Error* clears after a verified takeover or a verified restore.

Homage pattern: call `set_supervised_t1s(target, 600)` every ~2 min (or `renew_lease(600)`). If Homage dies, the curve is back within 10 min.

## Register contract

- `t1s` must be within 20–40 °C (UFH cap) and within the unit's T1S heating limits (regs 203/204, once read). Otherwise it is rejected. `lease_s` ≥ 10, capped at 7200 s. Lease timing uses wrap-safe `millis()` arithmetic.
- T1S zone 1 is written to the single-purpose **reg 11** (whole value, FC06), not by read-modify-write of the packed reg 2. Reg 11 mirrors the low byte of reg 2 (verified on this unit; the V.E mapping table recommends regs 11/12 for control). There is no single-purpose register for the weather-compensation bit, so reg 5 bit 12 stays a masked read-modify-write.
- Every write starts from a fresh read of that one register (the wired controller shares the bus), is skipped if the value/bits already match, and is verified by reading the same register back after 3 s, up to 3 attempts (15 s response timeout). The shadow globals are only updated from real reads, never optimistically (a real reg 11 read also updates the low byte of the reg 2 shadow).
- Only reg 11 and reg 5 bit 12 are ever written. Regs 0/2/6/7/8/212 are never written.
- A snapshot reg 11 outside 5–65 °C aborts the takeover before anything is written.
- State is a single mode, shown by *T1S Supervision Mode*: `idle → takeover → supervising → restoring (→ restore_backoff → restore_halted)`.

## Heat pump write budget

We assume the heat pump keeps reg 11 (= reg 2 low byte) and reg 5 across power cycles, so we treat every write as a write to its non-volatile memory. Where it is stored and how many writes it survives is **not documented** (fault code EE mentions a hydraulic-module EEPROM). The figures below assume ~100k write cycles, which is typical for EEPROM. That number is an assumption, not a spec.

Writes per session:

- Takeover: 1 (WC off, only if it was on) + 1 (T1S, only if different).
- Restore: 1 (WC on, only if it was on) + 1 (T1S back, only if different).
- So up to 4 per session when WC was on and T1S changes (2 if WC was already off), plus one per mid-lease T1S change.

Rate limiting:

- Mid-lease T1S writes are limited to one per `t1s_min_write_interval_s` (substitution in `packages/t1s-lease.yaml`, default 900 s). The latest target is always the one written. A request equal to the applied value writes nothing.
  - Worst case 4/h = 96/day ≈ 35k/year.
  - Homage duty planning with ~1 compressor start/h needs about 2 T1S changes/h ≈ 48/day ≈ 17.5k/year, plus session writes.
  - Keep the interval ≥ 15 min unless the unit's real endurance is known.
- The same interval also gates the takeover T1S write, across sessions, so `release → set` or `drift → set` churn cannot bypass it. A gated takeover write is deferred (T1S stays as is) and written at the next slot while the lease runs.
- Restore writes are never gated, but they count as the last T1S write.
- Homage must still not churn sessions: each takeover/restore pair costs up to 2 reg 5 writes, which are not rate limited.
- If a mid-lease T1S write is not verified (all read-backs timed out), both the last verified value and the target are accepted as expected until a real read decides. A write that did land is then not mistaken for a user change.
- *T1S Heat Pump Write Count* counts every reg 11/5 write this code made since boot.

ESP flash (NVS) is separate: it is written twice per session (snapshot on start, clear after a verified restore or an adopted external change). Renewals never write flash or Modbus.

## Failure modes and retries

- **Snapshot read fails:** supervision does not start and nothing is written.
- **Takeover write fails:** whatever was applied is rolled back.
- **Mid-lease T1S write fails:** the lease is kept, *Last Error* is set, and the write is retried at the next rate-limit slot.
- **Restore:** WC is restored first, then T1S. The two operations are independent, so a failed one does not block the other.
  - Only failed operations are retried, with exponential backoff (60 s, 120 s, … capped at 1 h). Each retry cycle *does* write again (≤ 3 attempts per failed operation).
  - After 8 failed cycles (~2 h) the ESP stops writing and enters `restore_halted`. It keeps the snapshot pending (it is restored again after a reboot) and sets a persistent *Last Error*.
  - A snapshot T1S outside 5–65 °C is not written back.
- **External change during a lease** (the HA *Weather Compensation Zone 1* switch or T1S number, or the wired controller):
  - Each 10 s poll (T1S via the reg 2 low byte, WC via reg 5) is compared with the expected state: WC zone 1 off, T1S zone 1 = last verified value. There is a 30 s grace period after our own I/O.
  - On drift the ESP adopts the user's change and supervision ends. *Last Error* and *T1S External Change Count* report it.
  - WC zone 1 switched on: everything is adopted, the snapshot is cleared, nothing is written.
  - T1S only: the user's T1S is adopted (never written back). WC zone 1 is still restored if it was on before the lease, because we turned it off, not the user. The snapshot stays pending with "skip T1S" until that WC restore is verified.
  - Homage must treat this as "user took over" and must not immediately call `set_supervised_t1s` again.
- **Known race (inherent):** if HA or the wired controller writes T1S between our fresh read and our write (one Modbus round trip, during takeover or a mid-lease write), their value is overwritten and no drift is reported.
- **DHW observation:** transitions of the reg 0 DHW bit (`0x0004`, read from the regular poll, never written) within 30 s of a lease write are logged and counted in *T1S DHW Drops After Lease Write*.
- **Reboot:** `api.reboot_timeout` is left at the default 15 min. A reboot mid-lease simply restores. Consider `0s` once Homage is the only API client.

## Entities

*T1S Supervision Active*, *T1S Supervision Mode*, *T1S Lease Remaining*, *T1S Supervision Last Error*, *T1S Supervision Restore Count*, *T1S Heat Pump Write Count*, *T1S External Change Count*, *T1S DHW Drops After Lease Write*.

Counters count since boot. All status entities also publish immediately on transitions.
