# User Guide

English | [简体中文](USER_GUIDE.md)

For flashing and building see [FLASH.en.md](FLASH.en.md); if you cannot
connect, start with [TROUBLESHOOTING.en.md](TROUBLESHOOTING.en.md).

All gauge operations in the web flasher and vehicle community use ESP32-S3 native USB: firmware flashing, theme/animation installation, deleting the current theme, vehicle installation/activation, and reboot. No device address, hotspot or OTA mode is needed. First update to firmware supporting USB management, boot normally, and select the serial port from the web action. Use Chrome/Edge over HTTPS or localhost. CH340/CP210x can flash firmware but cannot perform application media or management operations. Community browsing, login, downloads and publishing still use the network; App WiFi OTA remains compatible.

The firmware also fixes a missed LCD SPI lock wakeup in ESP-IDF 5.5.3 that could leave boot video stuck near its last frame. Existing animations and transparent-image themes need no re-export; update the firmware.

## First boot

1. On power-up the backlight stays off until the first Sky Gauge logo frame has
   reached the LCD and a 50 ms panel-scan settling delay has elapsed; the
   configured boot animation then plays (RACE by default, can be disabled)
2. Install vehicle profiles and themes through the web UI or App; activate a
   profile on the gauge's VEHICLE page and select a theme under Settings → THEME
   - **BOOT PAGE** — pick the screen the gauge lands on after boot
3. **Swipe up from the version page** into BLE SCAN and select your
   ELM327 BLE adapter
4. Once connected the gauge initializes automatically (protocol auto-detection
   is described in [TROUBLESHOOTING.en.md](TROUBLESHOOTING.en.md#protocol-auto-detection))
   and data starts flowing

On every later power-up the gauge reconnects to the last adapter
automatically (remembers BLE name + MAC) and self-heals when data stalls —
normally **you just power on and drive**, no manual steps.

The web vehicle community supports downloading, favoriting and editing profiles. Web device operations use ESP32-S3 native USB: install a profile, review warnings, then separately activate it for next boot and request a reboot. Install themes over web USB or App WiFi OTA, then select a registered theme under Settings → THEME. Verify vehicle compatibility and perform these operations while parked. Firmware manages the SD card in the background; users do not need to remove it or organize its files.

**Delete current theme** on the web flasher removes the current theme package and its dedicated resources under SD `/OBD/THEMES/<key>/`, then restores the built-in default after restart. The SD card must be mounted. Other themes and the shared boot animation are not deleted. This operation cannot be undone from the gauge.

## Page navigation

The main UI is a **page carousel**; swipe left/right to move through the ring
(swipe left = next page):

Images, videos and other content areas on a runtime theme page pass gestures to
the page itself, so swiping anywhere in the theme content moves to the next page
or the version page. The theme's default page reads and converts its images in
a background task while the Logo / boot animation remains visible. The boot
video replaces the Logo only after its first frame finishes transferring to the LCD;
UI updates cannot clear the startup image during playback. If the boot video ends first, the gauge returns to the Logo while the theme page finishes preparing. LVGL
creates the theme widgets after video playback and preparation finish, then
switches pages; a video decode failure or interruption immediately restores
the Logo, and a failed theme preparation falls back to the built-in page. Theme images use bulk SD reads to avoid lengthy byte-by-byte loading of transparent images; existing theme packages need no re-export.

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
| CHART | Chart page | Down: chart source config / **up: alarm thresholds** |
| Version page | Firmware build tag, BLE status and device name | see hidden entries below |

**The version page is the hub**:

- **Swipe up** → BLE SCAN (connect an OBD adapter; on slave gauges this doubles
  as the FIND MASTER scan)
- **Swipe down** → Settings
- **OTA button** → OTA update mode (app pairing, see [FLASH.en.md](FLASH.en.md#app-ota-upgrade-dual-slot-rollback))
- Swipe left/right → back to the gauge ring

**Hidden entries**:

- **Double-tap the logo page** → OBD protocol roller (**long-press 2 s** saves
  and reboots; protocol 0 = auto-detect, and detection retains that selection;
  select 0 once again if older firmware saved a nonzero protocol)
- **10 rapid taps on the version page** → showroom demo mode (sweep animation)

When data times out or BLE drops, gauge pages show a **NO SIGNAL** banner.

**Deleting a saved device on the BLE SCAN page**: tapping the ✕ next to the
saved device in the list immediately disconnects the current ELM327 link and
clears the saved name / MAC — **it does not auto-reconnect**; you must scan
and select again. On a slave (SLAVE), the same action on the **FIND MASTER**
page unbinds the master and starts a fresh scan.

## Settings page

| Setting | Description |
|---------|-------------|
| BOOT PAGE | Landing page after boot (TEMP / INFO / CHART / NEEDLE / GEAR / RPM / SPEED)|
| VEHICLE → OPEN | Open the vehicle page to review installed profiles and activate one; reboot as prompted for the change to take effect |
| THEME | Select a registered theme; the device restarts to apply its appearance |
| Brightness | 10–100% |
| RACECHRONO | ON = full BLE services (RaceChrono + pairing); OFF = minimal mode (a master keeps only the pairing service and advertises it; other roles run no BLE service and do not advertise) to save memory |
| Swipe down → MULTI-GAUGE | Triple-gauge role and boot-animation settings (see below)|

Select an installed profile on the vehicle page; install or import vehicle profiles through the web UI or App. THEME selects a theme registered in the firmware. The gauge has no SD-card management entry; firmware uses the card in the background for vehicle profiles, themes and media resources.
**Swipe down for MULTI-GAUGE and swipe left / right to return still work**.

Everything is persisted in NVS. Trip / mileage statistics accumulate per
power cycle only and reset on reboot.

## Vehicle, theme and SD storage

Install or import vehicle profiles through the web UI or App, then select and activate an installed profile from the gauge's VEHICLE page. Vehicle changes generally take effect after the device reboots; the web UI or App reports installation, activation and reboot status. If vehicle verification fails or the device enters safe mode, follow the recovery status shown in the web UI / App to restore Generic OBD2, or contact the maintainer. Select a theme from the Settings THEME roller; install theme packages through the web UI or App.

Firmware uses the SD card in the background for vehicle profiles, themes and media resources such as images, fonts and video, and handles imports and migration automatically. The gauge does not provide SD browsing, import or eject controls. Do not remove the card while the device is running or edit its files manually. When SD resources are unavailable, firmware follows its fallback behavior and uses the built-in UI and available profile.

Click **Export theme ZIP** in the web theme editor to download the theme and its resources in one ZIP. Insert an SD card,
connect the device on the web flasher page, and choose **Upload local files** in the **Theme** section to install, select, and restart automatically;
**From community** installs a community theme. Legacy BIN files are supported, optionally selected with companion JSON and `.obv` files;
a BIN alone cannot restore missing rules or videos. First update to firmware supporting this feature.
Theme upload uses ESP32-S3 native USB; users do not manage SD files on the gauge. After a complete firmware flash, reconnect before installing themes or animations.

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

TEST on the RPM warning page uses the master-generated ramp; pressing TEST again on either
master or slave restarts it. A request shortens the master's idle broadcast wait, but send batches
still wait at least 20 ms between them; the master entering OTA prevents new tests.

**Mixed-firmware compatibility**: the master only broadcasts v6 data frames
when every online slave supports v6; if even one slave is still on old
firmware it falls back to v5, so the three gauges can be upgraded one at a
time instead of all at once. Gear data only shows once every gauge is on new
firmware; vehicle-specific fields (`veh.*`) only show on a slave when it has
the same vehicle (matching id and revision) active as the master. The linked
RPM warning's control packets always stay v5 and are unaffected.

## Boot animation

Three modes, picked on the **Settings → swipe down to MULTI-GAUGE → INTRO**
roller (stored as NVS `intro_enable`):

| Mode | Effect |
|------|--------|
| OFF | Skip the animation, go straight to the gauges |
| RACE | Built-in "RACE AS ONE" animation (cluster-synchronized)|
| VIDEO | Plays shared SD boot media, independently of the selected theme |

VIDEO mode reads the shared SD directory:

1. `/OBD/BOOT/boot.obv`, then `/OBD/BOOT/boot.jpg`, then legacy `bootmedia.raw`.

Create `boot.obv` with `python3 tools/make_video.py boot INPUT -o boot.obv`.
The theme editor's Media panel manages images and videos used by pages and event rules; boot animation
is not part of a theme. The existing web uploader still writes to the shared `/OBD/BOOT/` directory.
After connecting in the web flasher, choose **From community** or **Upload local files** in the **Boot animation** section.
Local input supports common video formats (including MP4, MOV, WebM, MKV, AVI, WMV and GIF), OBV2 `.obv`, an encoded ZIP, a manifest and media BIN selected together, or an old raw image with a 4 KiB manifest header.
H.264 MP4 is decoded locally with WebCodecs; other videos load an on-demand ~31 MB single-thread FFmpeg WebAssembly core and decode locally to lossless PNG before the same Q90 baseline JPEG 4:2:0 encoding in a 360×360 canvas (aspect-preserving, no crop). Source timestamps and variable-frame intervals are retained, then the OBV2 is uploaded over USB to SD. Only inputs decodable by this FFmpeg WebAssembly build are supported; encrypted or corrupt files and browser memory exhaustion produce an error. Each frame is limited to 128 KiB, the source rate to 60 fps, and the total animation to 256 MiB. The firmware prebuffers about one second and presents frames by timestamp. Resolution is limited to the display's native 360×360. File, buffer, or frame-budget overflow is reported without silently reducing quality or frame rate. For local USB import, the source stays local; `.obv` files can also be uploaded directly. Frames larger than 48 KiB require firmware with the 128 KiB frame limit; existing smaller-frame animations remain compatible.
The boot-animation community **Upload** action uses the same 360×360 / Q90 encoding, retains source frame rate by default and supports crop, zoom, trim and an explicit frame rate. Community publishing uploads the source video as its preview.
If playback temporarily falls behind, firmware skips expired intermediate frames to catch up on the timeline, retains the first and last frames and no longer ends early after consecutive late frames.
After validation, the page restarts the device into application mode
and uploads to SD over ESP32-S3 native USB Serial/JTAG. A successful upload becomes the active animation
and deletes other old-format shared boot animations. Transfer interruption, SD write-sync failure, or validation failure preserves the current animation; old-file cleanup failure is reported as an error.
External CH340 / CP210x serial adapters can flash firmware but do not support this animation upload.
An inserted, mounted SD card with enough free space is required. The phone app can still upload over
WiFi OTA. With no card or no usable files, the custom animation is skipped. Legacy raw animation duration is derived from its actual frame count and FPS, ignoring potentially stale manifest duration metadata.
When there is no theme page and no saved OBD adapter or master, the scan page is created during boot animation and scanning starts only after the page is shown.
Use a 360×360, 1:1 video canvas and allow for
circular corner clipping. Old Flash animation is never played directly. Uploads use a verified
SD temporary file before installation; FAT file replacement has a short window and is not
power-loss-atomic.

## Updating from the phone app

The companion app (`android_app/app-debug.apk`) supports the transfers below. Before Wi-Fi operations,
enter OTA mode (**version page → OTA button**) and join the gauge hotspot:

- **Firmware**: the phone joins the gauge's WiFi hotspot (`OBD-Gauge-OTA-XXXX`,
  password `88888888`) for fast transfer, written to the inactive OTA slot
- **Boot animation**: trim the video in the app, encode, upload
- **Theme package**: pushes a runtime theme to the SD library and saves its selection
  (see [THEMES.en.md](THEMES.en.md))

Theme and boot-animation installation, vehicle installation/activation and theme deletion try Wi-Fi first. Android automatically falls back to native USB on Wi-Fi failure; iOS supports Wi-Fi only. Android checks USB management capability first; new firmware can stop OTA reception and hand over automatically, while older firmware requires exiting OTA mode. If OTA is starting or stopping, the operation asks you to retry later; wait for it to finish before retrying or swiping to exit. Failed theme/animation installation preserves current files; firmware automatically reboots on success. Vehicle installation and activation remain separate actions. If deletion or activation succeeds but reboot is unconfirmed, the App prompts a manual restart without replaying the operation.

The device exposes hardware and build info via `GET /ota/info`; the app
validates compatibility before flashing firmware and refuses
mismatched hardware. Protocol details in [APP_PROTOCOL.en.md](APP_PROTOCOL.en.md).

**Firmware verification and rollback**: after a new firmware boots, a
background task waits 15 seconds and only then calls
`esp_ota_mark_app_valid_cancel_rollback()` to mark the current slot valid. If
the new firmware reboots or crashes within those 15 seconds (never marking
itself valid), the bootloader automatically boots the previous slot on the
next startup — an automatic rollback with no manual steps. Leaving OTA mode
(swiping away) also triggers a reboot by itself.

## Optional external sensors

| Sensor | Wiring | Notes |
|--------|--------|-------|
| ADS1115 oil pressure | I2C, V1 Waveshare board only | Not started when the active vehicle profile reads oil pressure over OBD — the OBD value wins; oil-pressure alarms throttled to one per 30 s |

## Clear installed vehicle profiles

On the web USB flasher, choose “Clear installed vehicle profiles” in the vehicle section, review the scope and check the confirmation box. This clears all installed profiles and SD mirrors, then restarts with builtin `OBD2 Generic`. Insert the SD card and upgrade to firmware supporting `vehicle.clear`. A failed clear is not reported as successful; a failed restart can be retried separately. Themes, animations and vehicle templates in CATALOG / IMPORT / EXPORT are retained.

## Video decode ahead and double buffering

Playback retains sequential SD read-ahead, a PSRAM compressed-frame ring and a persistent JPEG decoder. Fullscreen mode decodes the next frame's first MCU strip or whole image ahead of time and starts panel transfer at its source deadline. Two internal DMA strips alternate by parity; submission waits for the previous transfer and never overwrites an in-flight buffer. Composite themes retain front/back frame buffers and LVGL handoff. OBV2 also decodes ahead, retaining compressed frames and shifting the timeline on pause; expired frames follow existing skip rules. Decode timing excludes deadline sleep. Actual FPS still requires device measurement; host tests do not establish a fixed hardware frame rate.
