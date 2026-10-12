# Pre-built Firmware

English | [简体中文](README.md)

Ready-to-flash firmware binaries. Full flashing instructions live in
[docs/FLASH.en.md](../docs/FLASH.en.md); the authoritative partition source
is [partitions.csv](../partitions.csv).

| File | Description |
|------|-------------|
| `release/bootloader/bootloader.bin` | Bootloader |
| `release/partition_table/partition-table.bin` | Partition table |
| `release/ota_data_initial.bin` | OTA data initial partition |
| `release/obd_brz_gauge.bin` | Application firmware |
| `release/latest.json` | Release manifest (build tag + sha256) — what the app checks for updates |

## Flash offsets

| Offset | File |
|--------|------|
| `0x0` | `release/bootloader/bootloader.bin` |
| `0x8000` | `release/partition_table/partition-table.bin` |
| `0xf000` | `release/ota_data_initial.bin` |
| `0x20000` | `release/obd_brz_gauge.bin` |
| `0x620000` | custom theme theme.bin (optional, see [docs/THEMES.en.md](../docs/THEMES.en.md)) |
| `0xA20000` | boot animation bootmedia.bin (optional) |

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0      release/bootloader/bootloader.bin \
  0x8000   release/partition_table/partition-table.bin \
  0xf000   release/ota_data_initial.bin \
  0x20000  release/obd_brz_gauge.bin
```

Note: re-run `tools/gen_release.py` (or the one-shot `tools/release.sh`)
whenever the release binaries change, or the app will compare fresh firmware
against a stale `latest.json`.
