# Development Guide

English | [简体中文](DEVELOPMENT.md)

Vehicles: [VEHICLES.en.md](VEHICLES.en.md) · Theming: [THEMES.en.md](THEMES.en.md) ·
Flash & update: [FLASH.en.md](FLASH.en.md) · App protocol: [APP_PROTOCOL.en.md](APP_PROTOCOL.en.md).

## Code architecture

```
main/
├── app_main.c            # Entry: hardware init, LVGL lock, BLE/RS485/ESP-NOW tasks, OTA self-check
│
├── app_obd_dsp/          # Application layer (board-agnostic logic)
│   ├── obd_data_cache    # OBD data cache (the only data plane between UI and acquisition)
│   ├── vehicle_profiles  # 17 vehicle profiles + gear-range math (see VEHICLES.en.md)
│   ├── vehicle_custom_config.h  # CAN rules / oil formulas / UDS header override tables
│   ├── ota_wifi_server   # WiFi SoftAP HTTP OTA service (all /ota/* endpoints)
│   ├── device_identity   # Device manifest JSON (GET /ota/info, hardware + build_tag)
│   ├── boot_media_mount / boot_block_player  # bootmedia partition mount & boot-animation playback
│   └── theme_mount       # theme_0 partition mount helpers
│
├── bsp_obd_dsp/          # Board support package (porting a new board happens here)
│   ├── elm327_ble_client # ELM327 BLE client + single-thread poll state machine + protocol auto-detect
│   ├── espnow_link       # Triple-gauge ESP-NOW link (MASTER/SLAVE/STANDALONE)
│   ├── racechrono_ble_diy # RaceChrono DIY BLE GATT service + pairing service
│   ├── gauge_pair_ble_client # slave FIND MASTER scan (shares the page with the OBD scan)
│   ├── nvs_storage       # NVS persistence for user config and stats (new fields append at the END only)
│   ├── ads1115_oil_pressure # ADS1115 I2C oil-pressure ADC (V1 board; unused by cars with an OBD DID)
│   ├── rs485_brake_temp  # RS485 Modbus RTU brake-temperature task (paused during OTA)
│   ├── lcd_driver/       # ST77916 QSPI LCD (V2/V3 boards use different init sequences)
│   ├── touch_driver/     # CST816 touch (I2C)
│   ├── exio/             # TCA9554 IO expander (V1 board only)
│   └── i2c_driver/       # I2C master driver
│
├── export_path/          # LVGL UI (SquareLine export + hand-written extensions)
│   ├── ui.c              # page creation order, gesture navigation (carousel), linked-warning logic
│   ├── screens/          # 24 screens: Logo/Main/Gear/ThemeGauge/Rpm/Speed/ODBProtocal/
│   │                     #   EasterEgg(version)/BLEScan/OTAMode/Temp/TempCustom/
│   │                     #   OilPressure(chart)/ChartConfig/ChartAlarm/Info/InfoCustom/
│   │                     #   Settings/OilWarn/RpmWarn/Needle/NeedleConfig/MultiGauge/Intro
│   ├── ui_ext.c          # hand-written extensions (sweep, showroom, boot animation) — survives SquareLine re-exports
│   ├── ui_disp_item.c    # display-item enum (the data sources mappable onto TEMP/INFO pages)
│   └── ui_theme*         # compiled-theme runtime (active theme, NVS, accessors; generated files: THEMES.en.md)
│
└── theme_engine/         # runtime theme engine (theme_0 partition → pages/colors/artwork/data binding)
```

### Data flow

```
ELM327 BLE adapter ──BLE──→ elm327_ble_client (single-thread poll: standard PIDs / manufacturer oil temp / gear DID / ATMA*)
                                   │
                                   ▼
                            obd_data_cache ←── rs485_brake_temp (brake temperature)
                                   │      ←── ads1115_oil_pressure (oil pressure, V1 board)
                 ┌─────────────────┼──────────────────┐
                 ▼                 ▼                  ▼
           LVGL page refresh   theme_update_data()   espnow_link (master broadcast)
           (built-in gauges)   (runtime theme pages)       │
                                                          ▼
                                              slave espnow_link receive → direct render
```

*ATMA CAN monitoring is enabled only for the `ZN/C6 CAN` profile; everything
else stays on OBD polling to avoid dual-path adapter contention.

**The ELM327 poll loop is a single-threaded state machine**: init →
(optional) protocol auto-detect → poll loop. Never introduce parallel
requests when touching it — one adapter serves one request stream at a time.

## Build configuration

Environment: ESP-IDF 5.5.3 + Python toolchain. Component dependencies
(`main/idf_component.yml`): `lvgl/lvgl`, `espressif/esp_lcd_touch`,
`espressif/button`, `espressif/knob` — pulled into `managed_components/` on
the first build.

```bash
idf.py set-target esp32s3
idf.py menuconfig     # OBD DSP Configuration
idf.py build
idf.py -p PORT flash monitor
```

`menuconfig → OBD DSP Configuration`:

| Option | Values | Notes |
|--------|--------|-------|
| Hardware version | **V1** (default) | Waveshare ESP32-S3-Touch-LCD-1.85: ST77916 v2 init + TCA9554 + ADS1115 |
| | V2 | New board A: ST77916 **v1** init, direct GPIO, no TCA9554 / ADS1115 |
| | V3 | New board B: ST77916 v2 init, direct GPIO, no TCA9554 / ADS1115 |
| RS485 pins | TX 13 / RX 12 / DE-RE −1 | Brake-temperature module; avoid GPIO43/44 for TX (onboard USB-UART) |

The build runs `tools/gen_themes.py` at CMake configure time to generate the
theme table, and injects the git branch / commit count / short hash as
`OBD_GAUGE_BUILD_TAG` (this is the build tag shown in the device manifest and
on the version page — so commit *before* releasing, see
[release workflow](#release-workflow)).

## Porting a new board

1. Add a hardware-version option in `Kconfig.projbuild` (if LCD / touch /
   IO expander differ)
2. Focus on `bsp_obd_dsp/`: `lcd_driver/` (init sequence, QSPI parameters),
   `touch_driver/`, `exio/` (IO expander present?), pin definitions
3. Resolution / color-depth macros live in `ST77916.h`; the UI assumes
   360×360, so a different screen means touching `export_path/`
4. Nothing vehicle-related needs to change — all car logic lives in
   `app_obd_dsp/`

## Tool scripts

| Script | Purpose |
|--------|---------|
| `tools/gen_themes.py` | compiled-theme codegen (runs from CMake; `--check` for CI) |
| `tools/theme_packer/pack_theme.py` | packs a runtime theme into a 4 MB theme.bin |
| `tools/gen_release.py` | build/ artifacts → firmware/release/ + latest.json |
| `tools/release.sh` | one-shot release (commit → build → gen_release → push) |
| `tools/make_boot_block.py` | encodes video into boot_block.bin/txt (ffmpeg + Pillow; auto _v2 when frames > 65535) |
| `tools/convert_rpm_flash.py` | 3 PNGs → RPM warning flash images (imgRpmFlash1..3.c) |
| `tools/fake_elm327.py` | fake ELM327 TCP server: logs every app request, answers unknown PIDs positively |
| `tools/one_shot.py` | one-shot PID hunting: starts the fake server → you record once → pids_full.csv |
| `tools/analyze_proble.py` | correlates injected signals with Car Scanner recordings to recover PID mappings and formulas |
| `tools/parse_carscanner_backup.py` | parses a CarScanner backup directory into a per-car PID CSV |
| `main/python_quick_rs485_check.py` | direct RS485/Modbus self-check (troubleshooting) |

### PID-hunting workflow (find private PIDs without a car)

To discover private PIDs (oil temp, oil pressure, gear, …) for a new car,
reverse-record what a phone app sends through the fake adapter (Car Scanner
ships per-car profiles):

```bash
python3 tools/one_shot.py
# 1. starts fake_elm327.py --probe prbs --tick 2 (injects known pseudo-signals)
# 2. on the phone, connect Car Scanner to the WiFi ELM327 (your IP:35000),
#    pick the target car profile, record a session and export the CSV
# 3. paste the CSV path; the server stops and the analysis runs → pids_full.csv
```

How it works: the fake server answers every unknown PID positively (so the
app never marks it unsupported and asks its whole profile), and each injected
signal corresponds to one real app request — pairing them in order recovers
which PID drives which gauge and its decode formula. Feed the results into a
vehicle profile per [VEHICLES.en.md](VEHICLES.en.md#adding-a-vehicle).

## Release workflow

`tools/release.sh` is the one-shot flow: activate the ESP-IDF environment
(eim) → commit sources (**commit first** — `count` is the git commit count) →
`idf.py build` → `tools/gen_release.py` copies `build/` artifacts into
`firmware/release/` and rewrites `latest.json` (sha256/size per file) →
commit and push.

Minimal app-side release layout:

```text
/releases/
  latest.json
  firmware/    obd_brz_gauge.bin · partition-table.bin · bootloader.bin · ota_data_initial.bin
  bootmedia/   bootmedia.bin
```

Re-run `gen_release.py` whenever release binaries change, or the app will
compare fresh firmware against a stale manifest.

## Commit conventions

- Record behavior and flashing changes in [CHANGELOG.en.md](../CHANGELOG.en.md)
- New fields in NVS `nvs_user_cfg_t` / theme structs are **appended at the
  end only** (on-device NVS compatibility)
- Never hand-commit `ui_theme_generated.c` / `theme_assets/` (generated files)

### Bilingual documentation rules

- Docs are maintained in pairs: `X.md` (Chinese, the **source of truth**) ↔
  `X.en.md` (English translation). Every fact change goes into the Chinese
  file first, then the English file follows in the same commit
- Structure is mirrored 1:1 (sections, tables, code blocks) so the two stay
  easy to diff against each other; commands, code, JSON, log output and table
  data are never translated
- Inside English docs, links to other documents point at their `.en.md`
  versions; links to code and partition files are unchanged
- Every paired file carries a language-switch line under its title
  (`English | [简体中文](X.md)`); when adding a new document, create both
  languages together
