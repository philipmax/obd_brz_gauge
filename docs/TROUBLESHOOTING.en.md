# Troubleshooting

English | [简体中文](TROUBLESHOOTING.md)

When the gauge won't connect or shows no data, work through the layers in
order: **BLE link first, then protocol, then data, then the standalone
peripherals.** With USB attached, watch logs via `idf.py monitor` — key tags
in [log keywords](#log-keywords).

```
Can the gauge see & join the adapter over BLE? ──no──→ Step 1
        │ yes
Protocol handshake OK, RPM readable? ──no──→ Step 2
        │ yes
Every channel you need has data? ──no──→ Step 3
        │ yes
RS485 brake temperature OK? ──no──→ Step 4
        │ yes
Still misbehaving ──→ Step 5, last resorts
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

If you know the car's protocol, lock it directly (the built-in profiles' lock
values are in the [profile table](VEHICLES.en.md#built-in-profile-table);
most post-2010 cars are 6 or 7). Entry point: **double-tap the logo page**
→ protocol roller → choose → **long-press 2 s to save and reboot**.

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

## Step 4: RS485 brake temperature (independent link)

RS485 goes over UART, not Bluetooth — troubleshoot it separately:

1. **Wiring**: GPIO13 (TX) / GPIO12 (RX), fixed 9600 baud, Modbus RTU. The
   DE/RE direction pin is disabled by default (`-1`, auto-direction
   modules); pins are changeable under
   `idf.py menuconfig → OBD DSP Configuration`
2. **Quick check**: attach a USB-TTL adapter to the PC and run

```bash
python3 main/python_quick_rs485_check.py --port /dev/cu.usbserial-XXXX --baud 9600 --tries 5
```

The script sends a standard Modbus read (addr=1, func=03, reg=0x0000) and
prints the sensor's reply. PC reads fine but the gauge doesn't → check wiring
and common ground; PC can't read either → sensor power / wiring.

Note: there is no brake-temperature status page in the UI — the values feed
the **CHART page**; the log tag is `brake_temp`.

## Step 5: last resorts

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
| `brake_temp` | RS485 brake temperature (500 ms per-read timeout) |
| `vehicle_profile` | profile switching, gear-range rebuild |
| `theme_engine` | theme partition loading / fallback |
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
Toyota/Mazda/BMW use private modes) — confirm the vehicle profile matches.
If it still fails, that car needs a new oil formula ([VEHICLES.en.md](VEHICLES.en.md)).

**Q: how do I tell a dead adapter apart?**
Install Car Scanner / Torque Pro on a phone and connect to the same adapter.
Phone can't read either → adapter or car problem; phone fine but gauge not →
open an issue with the log attached.

**Q: which protocol does the BRZ ZD8 (Gen2) use?**
Protocol 6. The built-in `ZD8 OBD` / `ZD8` profiles already lock protocol 6
and read oil temp via standard PID 5C — just pick the profile, no manual
trial needed.

**Q: do multiple gauges interfere over protocols?**
No. Each gauge stores its protocol in its own NVS.

---

Further help: [GitHub Issues](https://github.com/steveEcode/obd_brz_gauge/issues) —
attach an `idf.py monitor` log plus your car and adapter model.
