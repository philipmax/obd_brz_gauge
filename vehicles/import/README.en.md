# Editable vehicle import pack

English | [简体中文](README.md)

This pack contains **18 complete JSON profiles that the firmware can import directly**
(`skygauge.vehicle/1`). [OBD/IMPORT/](OBD/IMPORT/) contains `custom.*` copies of
all 17 catalog profiles, plus the separate `custom.bmw.g.gear` profile. These are
editable templates of existing request paths; compatibility requires vehicle testing.
The original catalog still has 17 profiles. Templates use independent IDs, including
an installable generic template. Their display names end in `cfg` to distinguish them
from the original profiles.

## Sparse profiles

All 18 complete templates remain usable. You may remove signal definitions that
should use standard OBD2: 9 omitted standard signals are completed automatically,
while each explicit declaration replaces its entire default signal. Disable one
with `{"id":"obd.oil_temp","disabled":true}` or all inheritance with root
`"defaults":"none"`. Gear and drivetrain ratios are not inherited. This behavior
requires firmware with the inheritance feature; use complete templates on older
firmware. Minimal and BMW G direct gear examples are in
[examples/](../examples/README.en.md); see
[PID_GUIDE.md](../../docs/PID_GUIDE.en.md) for the full rules.

## Installation

1. Select a vehicle's `*.vehicle.json` from `OBD/IMPORT/` and copy it to
   `OBD/IMPORT/` under the SD card root, keeping its filename. You can also copy
   the entire `OBD` directory to install the full pack.
2. Inserting the card or booting triggers validation, compilation and installation.
   Successful inputs are renamed `.ok`; failed inputs become `.err`, with details
   in the matching `.err.txt` and `OBD/IMPORT/IMPORT.LOG`.
3. Select the matching `cfg` profile on the gauge's Settings → Vehicle page,
   tap USE and reboot. Alternatively, write the profile ID on the first line of
   `OBD/IMPORT/ACTIVATE.TXT`, for example:

   ```text
   custom.bmw.g.gear
   ```

   The firmware installs JSON files before processing activation. Activation takes
   effect on the next boot; if boot-time import leaves the previous vehicle active,
   reboot once more. The pack includes no activation file; you choose the vehicle.

Built-in standard OBD2 needs no import; `custom.obd2.generic` allows standard
request customization. See the [vehicle documentation](../../docs/VEHICLES.en.md)
for HTTP installation and activation.

## Included profiles

| id | name | bus.proto | origin.profile_id |
|---|---|---|---|
| `custom.alfa.giulia_20t` | `GIULIA 2.0T cfg` | 7 | `alfa.giulia_20t` |
| `custom.bmw.e` | `BMW E cfg` | 6 | `bmw.e` |
| `custom.bmw.fg` | `BMW F/G cfg` | 6 | `bmw.fg` |
| `custom.bmw.g.gear` | `BMW G gear cfg` | 6 | `bmw.fg` |
| `custom.bmw.g.obd` | `BMW G OBD cfg` | 7 | `bmw.g.obd` |
| `custom.honda.integra` | `Honda Integra cfg` | 7 | `honda.integra` |
| `custom.jeep.generic` | `jeep cfg` | 7 | `jeep.generic` |
| `custom.mazda.mx5nd` | `MX-5 ND cfg` | 0 | `mazda.mx5nd` |
| `custom.mini.jcw_f56` | `JCW F56 cfg` | 0 | `mini.jcw_f56` |
| `custom.mini.r55` | `MINI R55 cfg` | 6 | `mini.r55` |
| `custom.obd2.generic` | `OBD2 Generic cfg` | 0 | `obd2.generic` |
| `custom.porsche.997_1` | `POS 997.1 cfg` | 6 | `porsche.997_1` |
| `custom.porsche.997_2` | `POS 997.2 cfg` | 6 | `porsche.997_2` |
| `custom.subaru.zc6.can` | `ZN/C6 CAN cfg` | 6 | `subaru.zc6.can` |
| `custom.subaru.zc6.pid` | `ZN/C6 PID cfg` | 6 | `subaru.zc6.pid` |
| `custom.subaru.zd8.obd` | `ZD8 OBD cfg` | 6 | `subaru.zd8.obd` |
| `custom.subaru.zd8.pid` | `ZD8 cfg` | 6 | `subaru.zd8.pid` |
| `custom.toyota.supra_a90` | `Supra A90 cfg` | 6 | `toyota.supra_a90` |

## Requests, bit decoding and polling frequency

Each request source explicitly supplies `hdr`, `expect` and `fail_after`. Existing
`rx`, `raw`, `cea`, multiframe, byte-from-end, valid ranges, hold and fallback settings
are preserved. `bus.proto` / `atst` / `gap_ms` apply to the whole profile;
`src.hdr` can be set per request and overrides `bus.hdr`. Templates supply
`src.hdr` explicitly: when changing the bus header, update the relevant source
headers too, or remove each source header to inherit `bus.hdr`. Missing bus fields
expand to firmware defaults:
`proto: 0`, `hdr: "7E0"`, `atst: 25`, `gap_ms: 30`. `atst` is a decimal number
with 4 ms units.

Requested signals use `rate: "round"` with explicit `period_ms`. Template defaults:

| signal | period_ms | nominal Hz |
|---|---|---|
| `obd.rpm` | 100 | 10 |
| `obd.speed`, `obd.gear`, `obd.load`, `obd.throttle`, `obd.boost` | 200 | 5 |
| `obd.afr`, `obd.oil_pressure` | 500 | 2 |
| `obd.intake_temp`, `obd.coolant_temp`, `obd.oil_temp`, `obd.battery_voltage` | 1000 | 1 |

`period_ms` is a **minimum polling interval**, not a guaranteed frequency. Requests
still run serially; adapter latency, request count, bus gaps and CAN monitoring
windows affect refresh rates. The allowed range is 20..60000 ms; the aggregate
request-rate budget must also pass validation. CAN-only sources retain `monitor`;
ratio-derived signals have no request interval.

Edit `rpn` to change decoding; `expr` is informational. For example,
`[["R", 0, 16, "be"]]` extracts an unsigned 16-bit big-endian value at bit 0 of
the decoded data; bit 0 is the first byte's most significant bit. `be` uses sequential
bit numbering; DBC Motorola sawtooth indices cannot be copied directly. CAN sources
can also set `bit` / `bits` / `endian` / `signed`. Requested data can use `R` instructions
for bit extraction; see [PID_GUIDE](../../docs/PID_GUIDE.en.md) and the
[contract](../../contracts/vehicle-rpn.md) for rules and examples.

## BMW G direct gear, BMW E and Jeep

- `custom.bmw.g.gear` reuses `bmw.fg` protocol 6, `raw: "18 03 22 DA 2E"`,
  `hdr: "6F1"`, `rx: "618"`, and `expect: "62DA2E"`. The first data byte after
  the prefix decodes to 0..8, with a minimum 200 ms polling interval and ratio
  fallback after 1 consecutive failure. Value 0 does not distinguish P/N/R.
  Gear ratios, final drive and tire parameters come from F/G; adjust them for
  the vehicle before relying on ratio fallback.
- `custom.bmw.g.obd` retains the original protocol 7 and ratio estimation. A
  profile has one protocol; the protocol 6 raw gear request is kept in its own
  variant. Select the variant for the actual vehicle and adapter.
- `custom.bmw.e` preserves the `224402` → `225822` oil-temperature source fallback
  using header `6F1`, including their data lengths and conversion formulas.
- `custom.jeep.generic` preserves protocol 7 and the 29-bit functional request
  header `18DB33F1`. It remains a generic placeholder; adjust gear ratios, final
  drive and tire parameters for the vehicle.

These profiles reuse existing repository paths. Generation adds no vehicle testing
and does not establish compatibility with every ECU on a chassis.

## Editing and regeneration

Copy a template outside the pack before personalizing it. Keep the schema, choose
an independent `custom.*` ID for a new profile, use the matching `<id>.vehicle.json`
filename, and increment `rev` when updating. JSON files must be at most 16384 bytes.
Update both `expr` and `rpn` when changing a formula; the firmware executes `rpn`.

```sh
python3 tools/vehicles/gen_import_pack.py
python3 tools/vehicles/gen_import_pack.py --check
cmake -S test/host/vehicle -B /tmp/obd-host/vehicle -G Ninja
cmake --build /tmp/obd-host/vehicle
/tmp/obd-host/vehicle/validate_catalog --strict vehicles/import/OBD/IMPORT
ctest --test-dir /tmp/obd-host/vehicle --output-on-failure
```

The generator reads `vehicles/catalog/`. **Regeneration overwrites personal edits
to this directory's templates.** `--check` only checks distributed templates against
their sources. The host validator uses the firmware's validation and compilation
implementations to check requests, formulas, resource limits and compiled results;
it does not check whether a vehicle responds.
