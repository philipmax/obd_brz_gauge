# PID / Signal Definition Guide

English | [简体中文](PID_GUIDE.md)

This is a tutorial-style walkthrough: how to turn a PID/CAN bit you discovered
into a working signal definition in a vehicle profile. The full authoritative
field rules live in [contracts/vehicle-rpn.md](../contracts/vehicle-rpn.md)
(the source of truth is the device-side validator `main/vehicle/vp_validate.c`
/`vp_rpn.c`); here we just walk through real catalog examples you can copy and
adapt. For the install/activate flow, see [VEHICLES.md](VEHICLES.en.md).

The web editor at `/vehicles/editor` opens in basic mode: choose standard OBD2, local JSON or a community profile, then select and edit collection data.
New profiles list the 10 built-in standard items and PIDs at one-second intervals. Change intervals, PIDs, request headers, reply addresses and formulas, or add other PIDs.
Editing formulas or response byte counts also generates and validates the firmware’s executable `rpn`. Imports retain their scheduling policies; editing inherited items writes complete definitions, and “Import standard OBD2 data” only fills gaps without overwriting custom or disabled items.
Edit raw manufacturer frames, CAN bits, LUTs and fallback sources in advanced mode; basic operations preserve all their fields. This standard list matches the firmware’s built-in data rather than every SAE PID.

## 1. The overall model

A vehicle profile (`vehicles/catalog/*.vehicle.json`) has a `signals` array.
Each entry is a signal (e.g. `obd.oil_temp`) with 1..4 `src` entries (sources,
tried in order as fallbacks). Each source runs an `rpn` (reverse-polish
expression) that converts raw bytes into an engineering value. Device-side
evaluation order:

```
response arrives → bytes selected per src.len/multiframe → run rpn
→ filter by src.valid → hold anti-jitter → fail_after N times rotates to
next src → signal-level post/valid → convert to internal fixed-point unit
→ filter (e.g. oil-temp hysteresis) → written to the signal table
```

(Full pipeline: [vehicle-rpn.md §3 求值管线](../contracts/vehicle-rpn.md#3-求值管线) —
`contracts/` specs stay Chinese-only, same as elsewhere in this guide)

### 1.1 Inherit standard OBD2 by default

Only vehicle-specific signals need declarations. The root `defaults` field defaults to
`"obd2"`; omitted standard signals inherit the built-in Generic OBD2 requests, formulas,
units, filters and polling policies. The smallest importable profile can omit both
`bus` and `signals`:

```json
{
  "schema": "skygauge.vehicle/1",
  "id": "custom.mycar",
  "rev": 1,
  "name": "My Car"
}
```

These 10 standard signals are added:

| 信号 / Signal | PID |
|---|---|
| `obd.rpm` | `010C` |
| `obd.intake_temp` | `010F` |
| `obd.speed` | `010D` |
| `obd.coolant_temp` | `0105` |
| `obd.load` | `0104` |
| `obd.throttle` | `0111` |
| `obd.oil_temp` | `015C` |
| `obd.battery_voltage` | `0142` |
| `obd.afr` | `0144` |
| `obd.boost` | `010B` |

An explicit signal **replaces the entire default definition** for its id. Fields are
not merged, and a standard PID is not added as an automatic failure fallback. For
example, a custom `obd.oil_temp` polls only the oil-temperature paths written in its
`src` array. An id alone or only a `period_ms` change is insufficient: a complete
`src` / `derive` definition is still required. Omission uses the default; to explicitly
disable a signal, add this entry to `signals`:

```json
{"id": "obd.oil_temp", "disabled": true}
```

Disabled entries allow only `id`, `disabled` and `notes`, and are excluded from polling.
For fully custom requests or pure CAN monitoring, set root `"defaults": "none"`;
`signals` must then be a nonempty array. When `bus.proto` is 0 or omitted, a manually selected device protocol takes precedence. With `0 - Automatic`, detection results are cached only for the current session and do not overwrite the NVS selection. Sustained decode failures trigger another probe; an explicit `bus.proto` remains locked. Existing nonzero settings saved by older firmware cannot be distinguished from manual choices: select `0 - Automatic` once after upgrading. Without `bus`, the other
defaults are header `7E0`, `ATST=0x19` and a 30 ms request gap. Inherited standard requests use the
vehicle's `bus.hdr`; special headers remain configurable through each `src.hdr`.
On J1850 / ISO 9141-2 / ISO KWP2000 (protocols 1–5), ordinary OBD requests retain the ELM327's native addressing. CAN-style `bus.hdr` / `src.hdr` values such as `7E0` do not cause `ATSH` commands; `ATH0` hides response headers. Protocols 7/9 map standard OBD request headers `7DF` and `7E0`–`7E7` to their 29-bit functional/physical equivalents; existing 29-bit headers are retained. CAN responses support compact `ATH1 + ATS0` output and 29-bit headers printed as separate bytes.
An imported `ATSP0` does not overwrite an already selected concrete protocol. After adapter auto mode (`ATSP0` / `ATSPAh`), `ATDPN` confirms the actual protocol before polling. A rejected protocol-selection command cannot make a reply on the previous protocol count as a successful candidate. On protocols 7/9, standard receive filters `7E8`–`7EF` are also mapped to 29-bit addresses; native 29-bit physical requests support Mode 01 functional fallback. Ordinary request/setup timeouts also interrupt and wait for `>`; BLE write failures reconnect before another command can extend a partial line.
Drivetrain ratios and ratio-derived `obd.gear` are not inherited: declare a direct gear
source or supply the vehicle's own `drivetrain`. Automatic completion does not imply
ECU support for a PID; unavailable values still follow existing failure rules and
show `--`. Resource limits, polling rates and signal dependencies are validated
after completion.

[Sparse examples](../vehicles/examples/README.en.md) include a minimal standard OBD2
profile and a BMW G profile that declares only direct gear. Installation works the
same way and requires firmware with this inheritance feature.

## 2. The simplest case: a standard OBD2 PID (coolant temperature)

Coolant temperature in `subaru.zc6.pid.vehicle.json` — standard Mode 01 PID
`05`, 1-byte response, "offset by 40" encoding (0..255 maps to -40..215°C):

```json
{
  "id": "obd.coolant_temp",
  "src": [
    {
      "req": "0105",
      "len": 1,
      "expr": "A-40",
      "rpn": [["A", 0], ["k", 40], "-"]
    }
  ]
}
```

- `req`: compact hex, `01` (Mode 01) + `05` (PID)
- `len: 1`: the formula may address 1 data byte (`d[0]`, after stripping the
  `41 05` response prefix)
- `expr`: the author-facing creation form — **a comment only**; the device
  only ever executes `rpn`
- `rpn`: `["A", 0]` pushes byte 0, `["k", 40]` pushes the constant 40, `"-"`
  subtracts

Compile `expr` → `rpn` with `tools/vehicles/expr2rpn.py`; don't hand-write
`rpn` unless the expression is this trivial (simple enough to transcribe by
eye, as above).

## 3. Big-endian bit fields + unit scaling: battery voltage, AFR

`obd.battery_voltage` (Mode 01 PID `42`, 2-byte big-endian, unit mV):

```json
{
  "id": "obd.battery_voltage",
  "unit": "mV",
  "src": [
    { "req": "0142", "len": 2, "expr": "A*256+B", "rpn": [["R", 0, 16, "be"]] }
  ]
}
```

`["R", 0, 16, "be"]` reads a 16-bit big-endian field directly — equivalent to
`A*256+B` but in one instruction instead of two. `unit: "mV"` must be one of
the units `obd.battery_voltage` allows in `profile_units` in
`contracts/signals.json` (it allows `V`/`mV`, defaulting to `V`).

`obd.afr` (Mode 01 PID `44`, commanded air-fuel ratio, with extra range
validation):

```json
{
  "id": "obd.afr",
  "src": [
    {
      "req": "0144", "len": 2, "valid": [8, 22],
      "expr": "(A*256+B)*14.7/32768",
      "rpn": [["R", 0, 16, "be"], ["k", 14.7], "*", ["k", 32768], "/"]
    }
  ]
}
```

`src.valid: [8, 22]` is source-level validation — a result outside the 8..22
AFR plausibility range is treated as a failed sample, so an obviously bogus
instantaneous reading never gets displayed.

## 4. Variable response length: Mode 21 multiframe + byte-from-end + hold

The BRZ (ZN/C6 PID profile) reads oil temperature via the Toyota/Subaru
private Mode 21 PID `01`. The response is multi-frame ISO-TP, and the
temperature byte's position drifts between car/firmware versions — so instead
of indexing from the front, it's addressed as the **5th byte from the end**:

```json
{
  "id": "obd.oil_temp",
  "valid": [-20, 255],
  "filter": "temp_hyst",
  "src": [
    {
      "req": "2101",
      "multiframe": true,
      "byte_from_end": 5,
      "valid": [-10, 255],
      "hold": { "delta": 8, "max_hold": 30, "gap_ms": 3000 },
      "expr": "X-40",
      "rpn": [["X"], ["k", 40], "-"]
    }
  ]
}
```

- `multiframe: true` + `byte_from_end: 5`: the response may span multiple
  frames; the formula only sees the **5th byte from the end**. It can only
  use `["X"]` (= this selected byte), not `["A", i]`
- `hold`: anti-jitter — if the new value differs from the last accepted value
  by ≤8°C it's accepted immediately; otherwise the old value is held, for up
  to 30 consecutive samples or until 3000ms pass without an update, at which
  point the new value is forced through (ported from the legacy firmware's
  `elm327_ble_client.c` oil-temp debounce logic)
- Two layers of `valid`: source-level `[-10,255]` filters obviously bad
  samples; signal-level `[-20,255]` is the final display range
Temperature hysteresis accepts -20 to 255 and passes CAN readings directly. Other readings are smoothed below a difference of 6; differences from 6 to under 20 require four identical readings, and differences of 20 or more require three. Negative results are rounded to the nearest integer.

- `filter: "temp_hyst"`: one more hysteresis pass after conversion to the
  internal unit

## 5. CAN broadcast monitoring: read bits directly, no request needed

`subaru.zc6.can.vehicle.json` (the ZN/C6 CAN profile) listens to broadcast
frames via `ATMA`; coolant temp, oil temp and throttle are all extracted
directly off the bus, with no request sent:

```json
{
  "requires": { "elm": ["ATMA"] },
  "monitor": { "mode": "probe", "every_ms": 500, "window_ms": 120, "stale_ms": 15000 },
  "signals": [
    {
      "id": "obd.coolant_temp",
      "src": [{ "can": "360", "bit": 24, "bits": 8, "expr": "X-40", "rpn": [["X"], ["k", 40], "-"]} ]
    },
    {
      "id": "obd.throttle",
      "src": [{ "can": "140", "bit": 48, "bits": 8, "expr": "X*100/255", "rpn": [["X"], ["k", 100], "*", ["k", 255], "/"]} ]
    }
  ]
}
```

- The top-level `monitor` block declares the listen window (requires
  `requires.elm: ["ATMA"]`); `mode: "probe"` means periodic listen windows,
  not holding the bus continuously
- `can: "360"`: 11-bit CAN ID; `bit`/`bits`: little-endian bit-field start and
  width (`endian` defaults to `le`, matching the current
  `can_extract_bits_le`)
- CAN sources also use `["X"]` for their own extracted field — the formula
  syntax is identical to request sources

## 6. Direct gear read (extended-addressing UDS DID) + fallback on failure

BMW F/G reads the EGS gear DID directly instead of ratio-estimating:

```json
{
  "id": "obd.gear",
  "fallback": "ratio",
  "src": [
    {
      "raw": "18 03 22 DA 2E",
      "hdr": "6F1",
      "rx": "618",
      "expect": "62DA2E",
      "len": 1,
      "valid": [0, 8],
      "fail_after": 1,
      "expr": "A",
      "rpn": [["A", 0]]
    }
  ]
}
```

- `raw`: the raw CAN payload (extended addressing: target `18`, PCI `03`,
  service `22`, DID `DA2E`)
- `hdr`/`rx`: this request overrides the send header / receive filter just
  for this call; the firmware restores the defaults afterward
- `expect`: the positive-response prefix, confirming this is the matching
  response and not another poll crossing wires
- `fail_after: 1`: switch to the next source after 1 consecutive failure —
  there's only one source here, so failure triggers `fallback: "ratio"`,
  falling back to ratio estimation (next section)

## 7. Ratio-estimated gear: `derive: "ratio"`

When there's no readable gear DID and no CAN-broadcast gear, derive it from
drivetrain ratios plus RPM/speed:

```json
{
  "drivetrain": { "fd": 4.1, "tire_m": 0.314, "gears": [3.626, 2.188, 1.541, 1.213, 1, 0.767], "tol": 0.15 },
  "signals": [ { "id": "obd.gear", "derive": "ratio" } ]
}
```

A `derive: "ratio"` signal **cannot** also have `src`; the profile needs both
`obd.rpm` and `obd.speed`. `fd` (final drive ratio), `tire_m` (tire rolling
circumference in meters), `gears` (per-gear ratios, ≤8 gears), and `tol`
(tolerance) all live in the top-level `drivetrain` block.

The difference between `derive: "ratio"` and `fallback: "ratio"`: the former
means "this car only has the ratio-estimation path" and cannot carry `src`;
the latter means "try a direct read first, fall back to ratio only on
failure" — `src` and `drivetrain` coexist.

## 8. Lookup tables `luts` (non-linear mapping)

No profile in the catalog currently uses `luts`, but the field is supported —
for example, if a gear DID returns a status code rather than the gear number
itself, a lookup table can map it:

```json
"luts": [[[0, 0], [1, 6, 1], [7, -1]]],
...
"rpn": [["A", 0], ["lut", 0]]
```

Each table has ≤16 entries; each entry is either `[key, out]` (exact match) or
`[lo, hi, out]` (range match), matched in order, first hit wins; no match
means an invalid sample. Full syntax: [vehicle-rpn.md §2.4 查找表](../contracts/vehicle-rpn.md#24-查找表-luts).

## 9. Which signal ids exist, and their units

`obd.*` is a fixed registry ([contracts/signals.json](../contracts/signals.json))
— you cannot invent new names. Vehicle-private data uses `veh.*` (≤32 per
profile, `^veh\.[a-z0-9_]{1,24}$`). The device currently accepts only the
signal fields listed below; `decimals` is not implemented and is rejected as
an unknown field during import.

| `obd.*` id | default unit | allowed units |
|---|---|---|
| `obd.rpm` | rpm | rpm |
| `obd.speed` | km/h | km/h, mph |
| `obd.boost` | bar | bar, 0.1bar, kPa_rel, psi |
| `obd.coolant_temp` | C | C |
| `obd.oil_pressure` | bar | bar, 0.1bar, kPa, hPa, psi |
| `obd.gear` | gear | gear |
| `obd.battery_voltage` | V | V, mV |
| `obd.oil_temp` | C | C |
| `obd.afr` | AFR | AFR, lambda |
| `obd.throttle` | % | % |
| `obd.intake_temp` | C | C |
| `obd.load` | % | % |

Each signal allows up to 4 `src` entries; a profile allows up to 48 signals
total (of which `veh.*` custom signals account for ≤32).

## 10. What commands are allowed: service / addressing whitelist

A profile cannot write raw AT commands directly (except the restricted
`bus.init` whitelist) — only structured fields, which the firmware assembles
into commands:

| Field | Allowed |
|---|---|
| Service number (in `req` or a raw frame's service byte) | `01`, `09`, `21` (1-byte parameter each), `22` (2-byte DID) |
| Send header `hdr`/`bus.hdr` | Functional addressing `7DF`/`18DB33F1`; physical addressing `7E0`-`7E7`, `600`-`6FF`, `18DAxxF1` |
| Receive filter `rx` | `7E8`-`7EF`, `600`-`6FF`, `18DAF1xx` |
| CAN listen id `can` | 11-bit ≤7FF, or 29-bit ≤1FFFFFFF |
| Raw frame `raw` | 1..8 bytes; the firmware tries parsing it as both normal and extended addressing, and the service number must still be in the whitelist |

Full whitelist, the `bus.init` AT-command allowlist, and each rejection's
error code: [vehicle-rpn.md §4-5 服务号与寻址白名单](../contracts/vehicle-rpn.md#4-服务号与寻址白名单).

## 11. Where validation errors surface

When a profile fails validation on install, error codes (`E_SCHEMA`,
`E_SIGNAL`, `E_SERVICE`, `E_HDR`, `E_RPN_OP`, `E_RPN_STACK`, `E_RPN_INDEX`,
`E_RPN_CONST`, `E_LIMIT`, `E_SIZE`, `E_NAME`, `E_RATE`, etc. — full list in
[vehicle-rpn.md §7 错误码](../contracts/vehicle-rpn.md#7-错误码)) show up in:

- SD card install: `OBD/IMPORT/IMPORT.LOG` and the matching `.err.txt`
- App / device API install: the 422 response body of
  `POST /api/v1/vehicles` — `{errors:[{code,path,msg}]}`
- Installed but activation failed: `GET /api/v1/vehicles`'s `last_error`

## 12. Hunting an unknown PID

You don't need a real car for this. The repo ships a fake ELM327
(`tools/fake_elm327.py`) that, paired with the phone's Car Scanner app, runs
through `tools/one_shot.py`: it automatically compares the pseudo-signals it
injects against the order of requests the app actually sends, reverse
engineering which PID drives which gauge and how to decode it. See
[DEVELOPMENT.en.md's tool scripts](DEVELOPMENT.en.md#tool-scripts) for
details. Once you've found it, write the `rpn` formula using this guide's
examples, then install it per
[VEHICLES.en.md's "Adding or editing a vehicle"](VEHICLES.en.md#adding-or-editing-a-vehicle)
section for device-side validation.

## 13. The easiest way to draft a new profile

Don't start from scratch — copy a catalog profile that shares the target
car's platform/protocol and edit the fields (e.g. a new Ford can start from
`obd2.generic`; a new BMW can start from `bmw.e.vehicle.json` or
`bmw.fg.vehicle.json`). After editing, use the import validation step
(section 11) to confirm there are no errors, then drive a few laps to check
the derived gear against `drivetrain`'s ratios/tire size.

## 14. Editing a complete import template: headers, bits and frequency

[vehicles/import/](../vehicles/import/README.en.md) provides 18 complete templates
that can be installed directly. Start with `custom.obd2.generic` for standard OBD2;
use `custom.jeep.generic` or `custom.bmw.e` for special addressing examples. BMW G
direct gear uses `custom.bmw.g.gear` (the existing F/G protocol 6 request); the
protocol 7 `custom.bmw.g.obd` remains a separate variant.

This example specifies the request, header, response prefix, data length,
16-bit decoding and minimum polling interval together:

```json
{
  "id": "obd.battery_voltage",
  "unit": "mV",
  "rate": "round",
  "period_ms": 1000,
  "src": [{
    "req": "0142",
    "hdr": "7E0",
    "expect": "4142",
    "len": 2,
    "fail_after": 5,
    "expr": "A*256+B",
    "rpn": [["R", 0, 16, "be"]]
  }]
}
```

After `expect` matches, `len` / `rpn` operate on data after the response prefix.
`R` supports a start bit, width and big/little endian; append `"s"` for signed
values. Big-endian bits start at the first byte's most significant bit and
increase sequentially, unlike DBC Motorola sawtooth numbering. `expr` is not
executed; update `rpn` to change decoding. CAN sources can directly change
`bit` / `bits` / `endian` / `signed`.

`period_ms` accepts 20..60000 and specifies a minimum interval; it cannot be used
with `rate: "every"`. Templates use `round` for requested signals: RPM 100 ms;
speed/gear/load/throttle/boost 200 ms; AFR/oil pressure 500 ms;
temperatures/voltage 1000 ms. Firmware sends requests one at a time, so actual
frequency depends on response time. A `rate: "every"` signal is polled before
each regular slot to reduce the wait for fast signals such as RPM; it is still
limited by BLE/ELM round trips, `gap_ms` and other fast signals. If a Mode 01
request receives NRC `7F 01 11` or `7F 01 12` on a physical CAN header, firmware
retries it on functional header `7DF` and keeps that fallback for the connection.
Set protocol, ATST timeout and request gaps in `bus`; a profile has one protocol.

At runtime, only standard `obd.*` signals bound or displayed on the current gauge
page are polled. RPM is always polled so its warning flash can work on other pages.
Ratio-derived gear also polls speed. Profile `veh.*` signals are not filtered by
the local page; the master keeps collecting and broadcasting them to slave gauges.

Copy a template outside the pack before editing. Match the filename when changing
the ID and increment `rev` on updates. Check personal files with the existing
firmware validator:

```sh
/tmp/obd-host/vehicle/validate_catalog --strict /path/to/custom.vehicle.json
```

Build commands and import steps are in the pack README. The template generator's
`--check` checks distributed files against catalog sources; it is not a validator
for personalized templates.

## Boost and high temperature readings

Builtin Generic and profiles omitting `obd.boost` now inherit `010B`, estimating relative pressure as `MAP - 100 kPa` and preserving negative values; 35 kPa corresponds to approximately -0.65 bar, rounded to -0.7 bar on the builtin 0.1 bar display. Needle and chart ranges start at -1.0 bar. The vehicle table lists explicit boost definitions; profiles without one can inherit generic MAP. The vehicle must support the PID; missing or stale data still displays `--`. The fixed 100 kPa reference is affected by altitude, weather and sensor location. Pressure after the turbo but before the throttle is not manifold vacuum, and diesel idle does not guarantee negative pressure. Accurate gauge pressure requires the vehicle's atmospheric reference or a validated proprietary PID.

Coolant and oil signal limits and builtin gauge scales now extend to 255°C; oil hysteresis and ESP-NOW v5 receive limits are extended too. Large temperature jumps still require consecutive confirmation. Standard single-byte `0105` / `015C` with `A-40` can only represent up to 215°C; 255°C requires a capable source formula and cannot be created by raising limits. Catalog profiles are updated to rev 3 with matching import templates. Firmware does not overwrite explicit old `valid` bounds or negative-pressure clamps in installed profiles; reimport the updated profile and check custom formula limits.

## RPM and speed acquisition scheduling

Firmware enables independent RPM/speed priority scheduling with minimum request intervals of 50 / 100 ms when unspecified. Explicit `period_ms` remains authoritative; new import templates use 50 / 100 ms. Deadlines use actual send times, inserting due samples between complete requests while retaining background service in each slot. Before/after commands, multiframe transactions and prompt waits remain uninterrupted. Consecutive failures back off for 1 / 2 / 4 / 8 / 16 seconds; success and reconnect reset the backoff. The speed cache retains actual samples and leaves display smoothing to the UI, avoiding extra delay in gear calculation and linked gauges. Only demanded page/gear signals are requested; RPM remains enabled. These are minimum intervals, not guaranteed 20 / 10 Hz rates: adapter, protocol, timeouts and request count determine throughput.
