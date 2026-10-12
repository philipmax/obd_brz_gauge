# Flash & Update

English | [简体中文](FLASH.md)

## Partition table

16 MB flash; the authoritative source is [partitions.csv](../partitions.csv) (layout v2):

| Partition | Offset | Size | Purpose |
|-----------|--------|------|---------|
| nvs | 0x9000 | 24 KB | User settings, BT/Wi-Fi bindings |
| otadata | 0xF000 | 8 KB | OTA slot selection |
| nvs_app | 0x11000 | 60 KB | App-private NVS (web flasher never touches this) |
| ota_0 | 0x20000 | 3 MB | Firmware slot A |
| ota_1 | 0x320000 | 3 MB | Firmware slot B |
| theme_0 | 0x620000 | 4 MB | Current theme cache / inbox |
| bootmedia | 0xA20000 | 4.75 MB | Boot animation cache / inbox |
| coredump | 0xEE0000 | 128 KB | Crash dump |
| config | 0xF00000 | 1 MB | LittleFS config area |

> **This release changes the partition layout.** Upgrading from the previous
> layout requires **one full reflash using the web flasher's complete mode**.
> OTA cannot rewrite the partition table. After that initial reflash, subsequent
> firmware updates can use single-firmware OTA as usual.
> **Do not use `esptool.py erase_flash`** — it erases user settings and
> Bluetooth pairings stored in nvs.

## USB full flash

Pre-built firmware is in [firmware/release/](../firmware/release/); you need
[esptool.py](https://github.com/espressif/esptool) (`pip install esptool`):

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0      firmware/release/bootloader/bootloader.bin \
  0x8000   firmware/release/partition_table/partition-table.bin \
  0xf000   firmware/release/ota_data_initial.bin \
  0x20000  firmware/release/obd_brz_gauge.bin
```

- Optionally append a theme: `0x620000 your_theme.bin`
- Optionally append a boot animation: `0xA20000 bootmedia.bin`

## App OTA upgrade (dual-slot rollback)

The companion phone app can push firmware, boot animations, and theme packages;
the entry point on the device is **version page → OTA button**. For a firmware
update:

1. The app fetches `latest.json` and reads the device manifest via
   `GET /ota/info` to check hardware compatibility; a mismatch aborts the flash
2. The new firmware is written to the **currently inactive OTA slot**
   (`esp_ota_ops`)
3. The device reboots into the new slot; the slot is marked valid only after a
   **15-second boot self-check** — if the new firmware crashes early, the
   bootloader rolls back to the previous slot

Boot animations and theme packages are integrity-checked in memory before flash
is touched, so an interrupted transfer never corrupts existing content. BLE and
ESP-NOW are paused during the transfer to free the CPU.

The full WiFi transfer protocol (HTTP endpoints, headers) is in
[APP_PROTOCOL.en.md](APP_PROTOCOL.en.md).
