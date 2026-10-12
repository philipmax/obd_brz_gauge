# 烧录与升级

[English](FLASH.en.md) | 简体中文

## 分区表

16MB Flash，权威来源 [partitions.csv](../partitions.csv)（layout v2）：

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| nvs | 0x9000 | 24 KB | 用户设置、BT/Wi-Fi 绑定 |
| otadata | 0xF000 | 8 KB | OTA 槽位选择 |
| nvs_app | 0x11000 | 60 KB | 应用专属 NVS（网页刷机不会覆盖） |
| ota_0 | 0x20000 | 3 MB | 固件槽 A |
| ota_1 | 0x320000 | 3 MB | 固件槽 B |
| theme_0 | 0x620000 | 4 MB | 当前主题缓存／收件箱 |
| bootmedia | 0xA20000 | 4.75 MB | 开机动画缓存／收件箱 |
| coredump | 0xEE0000 | 128 KB | 崩溃转储 |
| config | 0xF00000 | 1 MB | LittleFS 配置区 |

> **本版本分区表已变更**，从旧分区布局升级时**必须使用网页 complete 模式全量刷写一次**。
> OTA 本身无法修改分区表。刷写完成后，后续固件更新可通过单固件 OTA 进行。
> **禁止使用 `esptool.py erase_flash`**——这会擦除 nvs 中的用户设置和蓝牙绑定信息。

## USB 全量烧录

预编译固件在 [firmware/release/](../firmware/release/)，需要
[esptool.py](https://github.com/espressif/esptool)（`pip install esptool`）：

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0      firmware/release/bootloader/bootloader.bin \
  0x8000   firmware/release/partition_table/partition-table.bin \
  0xf000   firmware/release/ota_data_initial.bin \
  0x20000  firmware/release/obd_brz_gauge.bin
```

- 可选追加主题：`0x620000 your_theme.bin`
- 可选追加开机动画：`0xA20000 bootmedia.bin`

## App OTA 升级（双槽回滚）

配套手机 App 可以推送固件、开机动画和主题包，
操作入口在仪表 **版本页 → OTA 按钮**。升级固件时：

1. App 取 `latest.json`，用 `GET /ota/info` 读设备清单比对硬件兼容性，不匹配会拒绝刷写
2. 新固件写入**当前未运行的 OTA 槽**（`esp_ota_ops`）
3. 设备重启进新槽，**开机 15 秒自检**通过才标记有效；
   新固件在早期启动崩溃 → bootloader 自动回滚到旧槽

开机动画和主题包收进 flash 前先在内存校验完整性，传输中断不会破坏现有内容。
传输期间 BLE 与 ESP-NOW 暂停让出 CPU。

WiFi 传输的完整协议（HTTP 端点、请求头）见
[APP_PROTOCOL.md](APP_PROTOCOL.md)。
