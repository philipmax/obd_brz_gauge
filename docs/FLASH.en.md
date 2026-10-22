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
| theme_0 | 0x620000 | 4 MB | Legacy theme migration source / reserved |
| bootmedia | 0xA20000 | 4.75 MB | Legacy animation migration source / reserved |
| coredump | 0xEE0000 | 128 KB | Crash dump |
| config | 0xF00000 | 1 MB | LittleFS config area |

Custom resources are read only from SD; built-in firmware UI still boots without a card.
The two legacy media partitions total 8.75 MiB and remain migration/reserved space;
this change does not reassign them.

> The SD resource change preserves layout v2. Devices on an older partition
> layout require **one full reflash**.
> OTA cannot rewrite the partition table. After that initial reflash, subsequent
> firmware updates can use single-firmware OTA as usual.
> **Do not use `esptool.py erase_flash`** — it erases user settings and
> Bluetooth pairings stored in nvs.

After that one full reflash, the firmware runs a one-shot migration
(`first_boot_migration`) on its first boot: Bluetooth pairings and user
settings are kept; the old vehicle selection is restored from
`/OBD/CATALOG/<id>.vehicle.json` when that file is found, otherwise the gauge
runs `obd2.generic` and retries automatically once a card with that file is
inserted; the old theme and the old boot animation are moved to the SD
card's theme library / `/OBD/BOOT/` rather than discarded.

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

Copy custom resources to SD: `/OBD/THEMES/<key>/theme.bin` and ordinary theme videos;
independent boot media belongs at `/OBD/BOOT/boot.obv` or `/OBD/BOOT/boot.jpg`.
Release `bootmedia.bin` is a legacy SD asset: copy as `/OBD/BOOT/bootmedia.raw`; do not flash it.

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

Boot animations and themes stream to SD temporary files and are SHA-verified before
installation; a card is required. Transfer does not replace existing resources.
FAT installation has a short replacement window and is not a power-loss-atomic rename. BLE and
ESP-NOW are paused during the transfer to free the CPU.

The full WiFi transfer protocol (HTTP endpoints, headers) is in
[APP_PROTOCOL.en.md](APP_PROTOCOL.en.md).
