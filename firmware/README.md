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
| `release/bootmedia.bin` | 开机动画（raw 格式：[manifest 补 0xFF 到 4KB][bin]）|
| `release/latest.json` | 发布清单（build tag + sha256），App 用它检测新固件 |

## 烧录地址

| 偏移 | 文件 |
|------|------|
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

自定义主题与动画安装到 SD，不作为 USB 烧录项。`release/bootmedia.bin`
可复制为 SD `/OBD/BOOT/bootmedia.raw` 作旧格式回退；主题包放 `/OBD/THEMES/<key>/theme.bin`，
主题视频与主题开机媒体也放在主题目录（`boot.obv` 或 `boot.jpg`）。旧网页上传流程仍写入
`/OBD/BOOT/`，仅作为当前主题没有有效开机媒体时的兼容回退。无卡可使用内置默认界面。

注意：release 二进制有变动时要重跑 `tools/gen_release.py` 更新 `latest.json`
（一键流程用 `tools/release.sh`），否则 App 会拿旧清单比对新固件。
