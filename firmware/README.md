# 预编译固件

[English](README.en.md) | 简体中文

可直接烧录的固件二进制。完整烧录说明见
[docs/FLASH.md](../docs/FLASH.md)；分区权威来源是
[partitions.csv](../partitions.csv)。

| 文件 | 说明 |
|------|------|
| `release/bootloader/bootloader.bin` | Bootloader |
| `release/partition_table/partition-table.bin` | 分区表 |
| `release/ota_data_initial.bin` | OTA 数据初始分区 |
| `release/obd_brz_gauge.bin` | 应用固件 |
| `release/latest.json` | 发布清单（build tag + sha256），App 用它检测新固件 |

## 烧录地址

| 偏移 | 文件 |
|------|------|
| `0x0` | `release/bootloader/bootloader.bin` |
| `0x8000` | `release/partition_table/partition-table.bin` |
| `0xf000` | `release/ota_data_initial.bin` |
| `0x20000` | `release/obd_brz_gauge.bin` |
| `0x620000` | 自定义主题 theme.bin（可选，见 [docs/THEMES.md](../docs/THEMES.md)）|
| `0xA20000` | 开机动画 bootmedia.bin（可选）|

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0      release/bootloader/bootloader.bin \
  0x8000   release/partition_table/partition-table.bin \
  0xf000   release/ota_data_initial.bin \
  0x20000  release/obd_brz_gauge.bin
```

注意：release 二进制有变动时要重跑 `tools/gen_release.py` 更新 `latest.json`
（一键流程用 `tools/release.sh`），否则 App 会拿旧清单比对新固件。
