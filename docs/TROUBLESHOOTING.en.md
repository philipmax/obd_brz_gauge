# Troubleshooting

English | [简体中文](TROUBLESHOOTING.md)

When the gauge won't connect or shows no data, work through the layers in
order: **BLE link first, then protocol, then data.** With USB attached, watch logs via `idf.py monitor` — key tags
in [log keywords](#log-keywords).

```
Can the gauge see & join the adapter over BLE? ──no──→ Step 1
        │ yes
Protocol handshake OK, RPM readable? ──no──→ Step 2
        │ yes
Every channel you need has data? ──no──→ Step 3
        │ yes
Still misbehaving ──→ Step 4, last resorts
```

## Step 1: the BLE link

1. **Swipe up from the version page** into BLE SCAN and check whether the OBD
   device appears in the list
2. Select it; connection state is shown on the **version page** (status +
   device name)

**Device not found:**

- Make sure the adapter is powered (plugged into the OBD port / ignition ON)
- Confirm it is a **BLE (Bluetooth 4.0+)** device — old Bluetooth-Classic-only
  ELM327s cannot connect
- Power-cycle the adapter and rescan

**Won't connect / drops immediately:**

- First verify the adapter works by connecting with a phone OBD app
  (Car Scanner, Torque, …)
- If a phone app holds the connection the gauge cannot join — disable the
  app's auto-reconnect

## Step 2: protocol

### Protocol auto-detection

With the protocol set to **0 (auto)**, every connection triggers a probe:

```
init (ATZ → ATE0 → ATL0 → ATS1 → ATH0 → ATAT1 → ATST … → 01 00 capability probe)
   ↓
try protocols 1-11 in order: ATSP<n> → send 01 0C (read RPM)
   ├─ valid response → save protocol to NVS, start normal polling
   └─ 2 s timeout → next protocol
   ↓
all failed → fall back to protocol 6, log
"Protocol auto-detect FAILED, using fallback protocol 6"
```

- 2 s per protocol × 11 = 22 s worst case, usually 3–8 s
- During detection only RPM is probed; no garbage reaches the display
- Success is saved; reconnects skip detection (unless protocol is 0)
- Force re-detection: set protocol 0 → disconnect → reconnect

### Selecting a protocol manually

The firmware only builds in one generic profile, `Generic OBD2`; a
car-specific profile has to be installed from the vehicle catalog
(`vehicles/catalog/*.vehicle.json`) via the SD card's `/OBD/IMPORT` or the
phone app. Once installed, the protocol is already locked for that car and
you don't need to pick it here (most post-2010 cars are 6 or 7; the catalog's
exact lock values are in [VEHICLES.en.md](VEHICLES.en.md)). If you haven't
installed the matching profile yet and want to try protocols by hand, entry
point: **double-tap the logo page** → protocol roller → choose →
**long-press 2 s to save and reboot**.

Standard ELM327 protocol numbers (`ATSP`):

| # | Protocol | Typical cars |
|---|----------|--------------|
| 0 | Auto-detect (recommended starting point) | all |
| 1 | SAE J1850 PWM | older US cars (Ford) |
| 2 | SAE J1850 VPW | older US cars (GM) |
| 3 | ISO 9141-2 | older European cars |
| 4 | ISO 14230 KWP2000 (5 baud) | European cars |
| 5 | ISO 14230 KWP2000 (fast init) | European cars |
| 6 | ISO 15765-4 CAN 11-bit 500k | **most modern cars** (BRZ ZN/C6, BMW F/G, Porsche, …) |
| 7 | ISO 15765-4 CAN 29-bit 500k | BMW G OBD, FCA / 11th-gen Honda |
| 8 | ISO 15765-4 CAN 11-bit 250k | a few |
| 9 | ISO 15765-4 CAN 29-bit 250k | a few |
| 10 | SAE J1939 CAN | commercial vehicles |
| 11 | Auto (build on the currently detected protocol) | special cases |

Manual trial procedure: switch protocol → reconnect the adapter → wait 3–5 s
and see whether RPM shows a value (idle typically 600–1200). If RPM works the
protocol is right; individual missing channels are a Step-3 problem.

## Step 3: polling and parsing

### PIDs on the standard poll loop

| PID | Meaning | When missing |
|-----|---------|--------------|
| `01 0C` | RPM | 0 |
| `01 0D` | Speed | 0 |
| `01 05` | Coolant temp | 0 |
| `01 0F` | Intake air temp | 0 |
| `01 04` | Engine load | −1 |
| `01 11` | Throttle | −1 |
| `01 42` | Battery voltage | 0 |
| `01 5C` or manufacturer-specific (mode 21/22) | Oil temp | −100 (strategy is per-vehicle, see [VEHICLES.en.md](VEHICLES.en.md)) |

**Everything zero / error values**: polling never started or init failed →
back to Step 2. **Only some channels missing**: the PID isn't supported on
your car or needs a manufacturer-specific path — verify the same PID with a
phone app first; for genuinely private PIDs, dig them out with the
[PID-hunting workflow in DEVELOPMENT.en.md](DEVELOPMENT.en.md#tool-scripts)
and add a vehicle profile.

### Reading raw logs (USB required)

```
elm327_ble: Send 01 0C
elm327_ble: RAW[...]: 41 0C 12 34     ← healthy: got a response
elm327_ble: Parsed RPM: 4660
```

```
elm327_ble: Send 01 0C
elm327_ble: RAW[...]: ?               ← broken: wrong protocol / no bus reply
elm327_ble: PARSE_FAIL / TIMEOUT_AGAIN
```

## Step 4: last resorts

1. Power-cycle the whole chain: gauge → OBD adapter → ignition, then reconnect
2. Re-check settings: right vehicle profile? (protocol lock and oil-temp
   path follow the profile) Is the BLE device name the intended adapter?
3. Repeated init failures or NVS errors in the log: reflash over USB (a full
   first flash erases NVS anyway); for WiFi OOM crashes in master/slave mode,
   set Settings → MULTI-GAUGE back to standalone (takes effect after reboot)

## Log keywords

| Tag / keyword | Meaning |
|---------------|---------|
| `elm327_ble` | BLE connection, init, polling, protocol detection (`[DETECT]` prefix) |
| `vp_manager` | vehicle switching, verification period, safe mode, auto rollback |
| `sd_import` | processing results for SD card `/OBD/IMPORT` |
| `ui_storage` / `storage` | storage-page button jobs / background storage service (mirroring, resume, periodic scans) |
| `theme_lib` / `theme_mount` | theme-library write progress / resuming a theme write at boot or card insert |
| `theme_engine` | theme partition loading / fallback |
| `coredump` | core dump storage (paired with `GET /ota/coredump`) |
| `ota_wifi` | OTA WiFi hotspot mode and firmware / theme / boot-animation pushes |
| `ERROR` / `WARN` | any module |

To debug-log: `idf.py menuconfig → Component config → Log output → Default
log verbosity → Debug`, then rebuild. Filtered viewing:

```bash
idf.py monitor | grep -E "elm327|DETECT"
```

## FAQ

**Q: every value on screen is 0?**
Polling never started or init failed. Go through Steps 1–2 and check whether
the init commands get answers in the log.

**Q: connects but no fresh data?**
Wrong protocol (most modern cars are 6/7); or a phone app is holding the
adapter. Try another protocol and reconnect.

**Q: RPM/speed fine but oil temp / load missing?**
The oil-temp path is per-vehicle (standard 5C is only supported on some cars;
Toyota/Mazda/BMW use private modes) — confirm you have the right catalog
profile installed and activated (the firmware only builds in `Generic OBD2`;
a car-specific profile has to be installed from the catalog, see the next
entry). If it still fails, that car needs a new oil formula
([VEHICLES.en.md](VEHICLES.en.md)).

**Q: how do I tell a dead adapter apart?**
Install Car Scanner / Torque Pro on a phone and connect to the same adapter.
Phone can't read either → adapter or car problem; phone fine but gauge not →
open an issue with the log attached.

**Q: which protocol does the BRZ ZD8 (Gen2) use? How do I install the profile?**
Protocol 6. The firmware only builds in the generic `Generic OBD2`; the
`subaru.zd8.obd` and `subaru.zd8.pid` profiles both live in the vehicle
catalog (`vehicles/catalog/`) and have to be installed first: drop the
matching `*.vehicle.json` into the SD card's `/OBD/IMPORT` (or push it via
the phone app), then select it on the vehicle page and tap USE — or drop an
`ACTIVATE.TXT` with the profile id into `/OBD/IMPORT` at the same time to do
it in one step. Both profiles already lock protocol 6 and read oil temp via
standard PID `01 5C`.

**Q: do multiple gauges interfere over protocols?**
No. Each gauge stores its protocol in its own NVS.

**Q: card inserted but the gauge shows `NO CARD` or `CARD ERROR`?**
The storage page (swipe up from Settings) only supports **FAT16 / FAT32** —
an exFAT or unformatted card shows `CARD ERROR` with the hint "Use FAT32";
the device never formats the card automatically, so reformat it to FAT32 on
a computer. Seeing `NO SD SLOT` is normal — only the V1 Waveshare board has
a card slot, V2/V3 boards do not.

**Q: an SD import (vehicle profile / theme) didn't take effect?**
After tapping SCAN on the storage page (or waiting for the automatic scan at
boot / card insert), check the results in `/OBD/IMPORT`: successful files are
renamed to `<filename>.ok`, failed ones to `<filename>.err` with a matching
`<filename>.err.txt` — first line is the failure reason, followed by one
line per validator issue. You can also check the summary in
`/OBD/IMPORT.LOG`, or the "Import log" panel on the OTA mode web page.
Note that installing a `*.vehicle.json` **never auto-activates it** — you
also need an `ACTIVATE.TXT` or to tap USE on the vehicle page manually.

**Q: storage page says "Theme write failed" or "Busy, try again" / "theme library busy"?**
"Busy, try again": another SD / theme job is already running (an import, a
config mirror, another theme write) — wait for it to finish and retry.
"Theme write failed: <reason>": something is wrong with the theme package
itself (corrupt, larger than the theme_0 partition, unsupported schema
version); a failed write automatically restores the previous theme, so
normal use is unaffected. "theme library busy" also shows up when pushing a
theme or boot animation from the OTA web page — that path shares the same
lock with the storage page's theme write, so wait for the SD-side write to
finish before pushing from the app.

**Q: the VEHICLE button on the vehicle page shows `SAFE`, or the gauge keeps running Generic and won't switch vehicles?**
That's **safe mode**: after 3 abnormal resets in a row (crash / watchdog),
the next boot is forced to run only `Generic OBD2`, no pending vehicle
switch is promoted, and the SD card only processes `RESET.TXT`. Running
normally for 60 seconds clears the crash counter and recovers
automatically. To get out immediately: tap **GENERIC** on the vehicle page,
or drop a `RESET.TXT` file into `/OBD/IMPORT` on the SD card. If the vehicle
page instead shows `FAIL`, that's a different thing — a vehicle failed
verification on 3 boots in a row and was auto rolled back; it is not safe
mode, and the reason is in the vehicle page's error area.

**Q: how do I retrieve a core dump?**
Enter OTA mode, join the `OBD-Gauge-OTA-XXXX` hotspot with a phone, open
`http://192.168.4.1/` in a browser, and click Download in the "Core dump"
panel — or call `GET /ota/coredump` directly (needs the `X-OTA-Token` from
`GET /ota/discover` first). With no core dump stored it returns
**204 No Content** (the web page shows "No core dump stored."), not 404.
Clear it afterwards with Erase or `POST /ota/coredump/erase`.

---

Further help: [GitHub Issues](https://github.com/steveEcode/obd_brz_gauge/issues) —
attach an `idf.py monitor` log plus your car and adapter model.
