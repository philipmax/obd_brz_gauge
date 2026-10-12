# User Guide

English | [简体中文](USER_GUIDE.md)

For flashing and building see [FLASH.en.md](FLASH.en.md); if you cannot
connect, start with [TROUBLESHOOTING.en.md](TROUBLESHOOTING.en.md).

## First boot

1. On power-up the Sky Gauge logo appears, then the boot animation set in
   settings plays (RACE by default, can be disabled)
2. On first use open **Settings** (swipe down from the version page):
   - **VEHICLE** roller — pick your car (applies immediately, no reboot)
   - **BOOT PAGE** — pick the screen the gauge lands on after boot
3. **Swipe up from the version page** into BLE SCAN and select your
   ELM327 BLE adapter
4. Once connected the gauge initializes automatically (protocol auto-detection
   is described in [TROUBLESHOOTING.en.md](TROUBLESHOOTING.en.md#protocol-auto-detection))
   and data starts flowing

On every later power-up the gauge reconnects to the last adapter
automatically (remembers BLE name + MAC) and self-heals when data stalls —
normally **you just power on and drive**, no manual steps.

## Page navigation

The main UI is a **page carousel**; swipe left/right to move through the ring
(swipe left = next page):

```
        ┌──────────────────────────────────────┐
        │  GEAR → RPM → SPEED → TEMP → INFO →  │
        │  NEEDLE → CHART → version page → GEAR│
        └──────────────────────────────────────┘
```

| Page | Shows | Swipe-down sub-page |
|------|-------|---------------------|
| GEAR | Gear (large) + RPM | Runtime-theme gauge page (only when a partition theme with pages is loaded)|
| RPM | RPM arc + red over-rev flash | RPM warning threshold / flash toggle |
| SPEED | Speed + trip stats (runtime-only) | — |
| TEMP | Coolant / oil / intake temps (remappable rows) | 3-row data-item mapping |
| INFO | Multi-cell info panel (remappable cells) | 5-cell data-item mapping |
| NEEDLE | Needle meter (swipe down to change source) | Needle source picker |
| CHART | Chart page (brake temperature lives here) | Down: chart source config / **up: alarm thresholds** |
| Version page | Firmware build tag, BLE status and device name | see hidden entries below |

**The version page is the hub**:

- **Swipe up** → BLE SCAN (connect an OBD adapter; on slave gauges this doubles
  as the FIND MASTER scan)
- **Swipe down** → Settings
- **OTA button** → OTA update mode (app pairing, see [FLASH.en.md](FLASH.en.md#app-ota-upgrade-dual-slot-rollback))
- Swipe left/right → back to the gauge ring

**Hidden entries**:

- **Double-tap the logo page** → OBD protocol roller (**long-press 2 s** saves
  and reboots; protocol 0 = auto-detect)
- **10 rapid taps on the version page** → showroom demo mode (sweep animation)

When data times out or BLE drops, gauge pages show a **NO SIGNAL** banner.

## Settings page

| Setting | Description |
|---------|-------------|
| BOOT PAGE | Landing page after boot (TEMP / INFO / CHART / NEEDLE / GEAR / RPM / SPEED)|
| VEHICLE | The 17 vehicle profiles; switching applies immediately (affects gear detection, oil-temp strategy, protocol lock, …)|
| THEME | Compiled-in themes (default / amber / ocean plus community themes); selecting **reboots** the device to apply |
| Brightness | 10–100% |
| RACECHRONO | ON = full BLE services (RaceChrono + pairing); OFF = minimal mode (a master keeps only the pairing service and advertises it; other roles run no BLE service and do not advertise) to save memory |
| Swipe down → MULTI-GAUGE | Triple-gauge role setting (see below)|

Everything is persisted in NVS. Trip / mileage statistics accumulate per
power cycle only and reset on reboot.

## Triple-gauge cluster (ESP-NOW)

An ELM327 adapter accepts a single BLE client, so multiple gauges work as
**one master plus slaves**:

- **Master (MASTER)**: keeps the BLE + ELM327 connection and reads OBD,
  broadcasting parsed data over ESP-NOW
- **Slave (SLAVE)**: receives the broadcast and renders, zero extra OBD load
- **Standalone (default)**: ESP-NOW disabled

All gauges run the same firmware; the role is picked under
**Settings → swipe down → MULTI-GAUGE** (takes effect after reboot).

**Pairing** (real BLE pairing, not blind broadcast binding):

1. Set the master to MASTER; it advertises `SkyGauge-XXYY` (device suffix, so
   multiple cars at a track day don't interfere)
2. Set slaves to SLAVE; an unpaired slave boots straight into the
   **FIND MASTER** scan page
3. Pick the master to follow from the list — done (the master's MAC is
   remembered and auto-joined on every later boot)

The cluster also supports a **linked RPM warning**: past the threshold the
three gauges light up in position order and then all flash (toggleable as
Linked mode on the RPM warning page), plus the synchronized "RACE / AS / ONE"
boot animation.

## Boot animation

Three modes (NVS `intro_enable`):

| Mode | Effect |
|------|--------|
| OFF | Skip the animation, go straight to the gauges |
| RACE | Built-in "RACE AS ONE" animation (cluster-synchronized)|
| VIDEO | Plays a custom animation from the `bootmedia` partition (upload via the phone app, see [FLASH.en.md](FLASH.en.md#app-ota-upgrade-dual-slot-rollback))|

Custom animation requirements: 360×360 canvas, 1:1 aspect, mind the corner
clipping on the round screen. Updates are transactional on the device — an
interrupted upload never damages the existing animation.

## Updating from the phone app

The companion app (`android_app/app-debug.apk`) can push three things; each
requires the gauge to be in OTA mode first (**version page → OTA button**):

- **Firmware**: the phone joins the gauge's WiFi hotspot (`OBD-Gauge-OTA-XXXX`,
  password `88888888`) for fast transfer, written to the inactive OTA slot; the new
  firmware must pass a 15-second boot self-check before being marked valid,
  otherwise the bootloader rolls back
- **Boot animation**: trim the video in the app, encode, upload
- **Theme package**: pushes a runtime theme to the theme_0 partition
  (see [THEMES.en.md](THEMES.en.md))

The device exposes hardware and build info via `GET /ota/info`; the app
validates compatibility before flashing and refuses
mismatched hardware. Protocol details in [APP_PROTOCOL.en.md](APP_PROTOCOL.en.md).

## Optional external sensors

| Sensor | Wiring | Notes |
|--------|--------|-------|
| RS485 brake temperature | GPIO13 (TX) / GPIO12 (RX), 9600 baud, Modbus RTU | Data feeds the CHART page; brake-temp alarms throttled to one per 30 s |
| ADS1115 oil pressure | I2C, V1 Waveshare board only | Cars such as Supra A90 / BMW E read oil pressure over OBD DIDs instead |

See the matching sections of [TROUBLESHOOTING.en.md](TROUBLESHOOTING.en.md).
