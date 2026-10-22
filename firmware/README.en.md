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
| `release/bootmedia.bin` | Boot animation (raw layout: [manifest padded with 0xFF to 4 KB][bin]) |
| `release/latest.json` | Release manifest (build tag + sha256) — what the app checks for updates |

## Flash offsets

| Offset | File |
|--------|------|
| `0x0` | `release/bootloader/bootloader.bin` |
| `0x8000` | `release/partition_table/partition-table.bin` |
| `0xf000` | `release/ota_data_initial.bin` |
| `0x20000` | `release/obd_brz_gauge.bin` |

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0      release/bootloader/bootloader.bin \
  0x8000   release/partition_table/partition-table.bin \
  0xf000   release/ota_data_initial.bin \
  0x20000  release/obd_brz_gauge.bin
```

Custom themes and animations install to SD and are not USB flash entries. Copy
`release/bootmedia.bin` as SD `/OBD/BOOT/bootmedia.raw`; place packages at
`/OBD/THEMES/<key>/theme.bin`, with theme videos and `boot.obv` alongside.
The built-in default UI works without a card.

Note: re-run `tools/gen_release.py` (or the one-shot `tools/release.sh`)
whenever the release binaries change, or the app will compare fresh firmware
against a stale `latest.json`.
