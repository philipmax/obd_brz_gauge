# App 对接协议

[English](APP_PROTOCOL.en.md) | 简体中文

设备在 OTA 模式下暴露的 WiFi HTTP API。用户侧的烧录与升级操作见
[FLASH.md](FLASH.md)。

## OTA 模式下设备暴露什么

仪表 **版本页 → OTA 按钮** 进入 OTA 模式后：

- WiFi SoftAP：SSID `OBD-Gauge-OTA-XXXX`（MAC 后两字节）、密码 `88888888`、
  IP `192.168.4.1`、端口 80
- 每次会话生成随机 16 位 hex token，用于 HTTP 鉴权

进入 OTA 模式会彻底释放蓝牙，设备不提供任何 BLE OTA / 清单服务；离开 OTA 模式即重启。

## 设备清单（`GET /ota/info`）

App 刷写前用它做硬件匹配校验：

- 免鉴权，OTA 模式下可用
- 载荷：UTF-8 JSON（上限 512 字节，只返回 App 实际使用的字段）

```json
{
  "device": {
    "board": "Waveshare ESP32-S3-Touch-LCD-1.85",
    "variant": "obd_brz_gauge",
    "lcd": "ST77916",
    "screen": { "w": 360, "h": 360, "bpp": 16 },
    "flash_mb": 16,
    "psram_mb": 8,
    "ota_slots": 2,
    "bootmedia_slots": 1,
    "bootmedia_format": 1
  },
  "firmware": {
    "build_tag": "main-124-abcdef123456",
    "branch": "main",
    "count": 124
  }
}
```

`build_tag` 格式为 `<分支>-<提交数>-<短哈希>`，编译时由 `main/CMakeLists.txt` 从 git
注入。App 用 `firmware.count` 与 `latest.json` 比较判断新旧。
硬件字段与所选 release 不一致时 App 应拒绝刷写。

## WiFi OTA HTTP API

App 连上设备 SoftAP 后走 HTTP（所有端点均支持 OPTIONS 预检）。先调免鉴权的
`GET /ota/discover` 取本次会话的 token：

```json
{"ssid":"OBD-Gauge-OTA-ABCD","password":"88888888","ip":"192.168.4.1","token":"a1b2c3d4e5f6a7b8","port":80}
```

**公共请求头**：`X-OTA-Token: <GET /ota/discover 返回的 16 位 hex>`（`/ota/discover`、`/ota/info` 免鉴权）

| 端点 | 方法 | 说明 |
|------|------|------|
| `/ota/discover` | GET | 局域网发现，返回 SoftAP 信息与 token |
| `/ota/info` | GET | 设备清单 JSON（格式见上）|
| `/ota/status` | GET | `{"state":"ready\|receiving\|done\|error","received":N,"expected":M}` |
| `/ota/firmware` | POST | 固件上传 |
| `/ota/bootmedia/prepare` | POST | 开机动画预检 |
| `/ota/bootmedia` | POST | 开机动画上传 |
| `/ota/theme/prepare` | POST | 主题预检（返回挂载状态，不擦除）|
| `/ota/theme` | POST | 主题分块上传 |
| `/ota/theme/erase` | POST | 擦除主题分区，下次开机回退默认主题 |
| `/ota/coredump` | GET | 下载 coredump 分区原始二进制；分区为空时返回 204 |
| `/ota/coredump/erase` | POST | 擦除 coredump 分区 |

各上传端点的专有请求头：

- `/ota/firmware`：`X-OTA-SHA256`（64 位 hex）、`X-OTA-Size`（字节），Body 为固件二进制
- `/ota/bootmedia`：`X-OTA-SHA256`、`X-OTA-Size`（manifest+bin 总大小）、
  `X-OTA-Manifest-Size`，Body 为 `[manifest][bin]` 拼接
- `/ota/theme`：`X-OTA-SHA256`、`X-OTA-Size`（≤4MB）、`X-OTA-Offset`（块偏移）、
  `X-Last`（`1`=最后一块），Body 为 `theme.bin` 分块数据

**安全性**：token 每会话随机；SoftAP WPA2-PSK；全部负载 SHA256 校验。
主题打包格式见 [THEMES.md](THEMES.md)。

## 开机动画编辑规则（App 端）

- 画布 360×360，比例锁 1:1；建议圆形预览遮罩（圆屏四角裁切）
- 手机端先裁切起止时间，再编码上传
- 编码包必须放得进 bootmedia 分区（5.875 MB）
- 设备端只保留一个生效动画槽；App 可存多个草稿，只上传选中的那个
