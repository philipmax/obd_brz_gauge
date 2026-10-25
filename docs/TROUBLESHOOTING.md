# 故障排查

[English](TROUBLESHOOTING.en.md) | 简体中文

连不上、没数据时按下面的分层流程逐级排查：**先确认蓝牙链路，再确认协议，最后看数据**。带 USB 连接时用 `idf.py monitor` 看日志，关键字见
[日志关键字](#日志关键字)。

```
BLE 能扫到并连上？ ──否──→ 第一步
        │ 是
协议握手成功、能读到 RPM？ ──否──→ 第二步
        │ 是
所有需要的通道都有数？ ──否──→ 第三步
        │ 是
仍然异常 ──→ 第四步 综合手段
```

## 第一步：BLE 物理连接

1. **版本页上滑**进入 BLE SCAN 页面，检查 OBD 设备是否出现在扫描列表
2. 选中连接；连接状态显示在**版本页**（连接状态 + 设备名）

**扫不到设备：**

- 确认 OBD 设备已上电（插在 OBD 口 / 点火 ON）
- 确认设备是 **BLE（蓝牙 4.0+）**，老款只支持蓝牙 Classic 的 ELM327 连不上
- 重启 OBD 设备再扫

**连不上 / 连上即断：**

- 先用手机 OBD App（Car Scanner、Torque 等）连同一设备验证其工作正常
- 适配器被手机 App 占用时仪表无法连接 —— 关掉手机 App 的自动重连

## 第二步：协议

### 协议自动检测

协议设为 **0（自动）**，且没有当前会话或 NVS 中可复用的协议时，仪表会自动探测：

```
初始化（ATZ → ATE0 → ATL0 → ATS1 → ATH0 → ATAT1 → ATST … → 01 00 能力探测）
   ↓
依次尝试协议 1–12：ATSP<n> → 发 01 0C（读转速）测试
   ├─ 有效响应 → 保存协议到 NVS，进入正常轮询
   └─ 2 秒超时 → 下一个协议
   ↓
全部失败 → 回退协议 6，日志 "Protocol auto-detect FAILED, using fallback protocol 6"
```

- 每协议 2 秒 × 12 个 = 最多约 24 秒，通常 3–8 秒内命中
- 检测期间只测 RPM，不会把异常数据刷进显示
- 检测成功自动保存；后续重连优先复用已检测协议，避免每次等待完整探测
- 强制重检：协议设为 0 → 断开 → 重连

### 手动选择协议

固件只内置 `Generic OBD2` 这一个通用配置；具体车型的车型配置要从车型目录
（`vehicles/catalog/*.vehicle.json`）通过 SD 卡 `/OBD/IMPORT` 或手机 App 安装，
安装后协议已经按该车型锁定好，不需要在这里手动选（多数 2010 后的车是 6 或 7，
车型目录里的具体锁定值见 [VEHICLES.md](VEHICLES.md)）。还没装对应车型、想先手动试
协议的，入口：**Logo 页双击**进协议滚轮 → 选择 → **长按 2 秒保存并重启**。

标准 ELM327 协议编号（`ATSP`）：

| 编号 | 协议 | 常见车型 |
|-----|------|---------|
| 0 | 自动检测（推荐起点）| 所有 |
| 1 | SAE J1850 PWM | 老美国车（Ford 等）|
| 2 | SAE J1850 VPW | 老美国车（GM 等）|
| 3 | ISO 9141-2 | 老欧洲车 |
| 4 | ISO 14230 KWP2000（5 baud）| 欧洲车 |
| 5 | ISO 14230 KWP2000（fast init）| 欧洲车 |
| 6 | ISO 15765-4 CAN 11 位 500k | **多数现代车**（BRZ ZN/C6、BMW F/G、保时捷等）|
| 7 | ISO 15765-4 CAN 29 位 500k | BMW G OBD、FCA / 本田 11 代 |
| 8 | ISO 15765-4 CAN 11 位 250k | 少数 |
| 9 | ISO 15765-4 CAN 29 位 250k | 少数 |
| 10 | SAE J1939 CAN | 商用车 |
| 11 | 用户自定义 CAN 协议 1 | 特殊适配器 |
| 12 | 用户自定义 CAN 协议 2 | 特殊适配器 |

手动逐个试的方法：切换协议 → 重连 OBD 设备 → 等 3–5 秒看 RPM 是否出数
（怠速一般 600–1200）。RPM 有数说明协议对了，个别通道没数属于第三步的问题。

## 第三步：数据轮询与解析

### 标准轮询的 PID

| PID | 含义 | 无数据时的表现 |
|-----|------|---------------|
| `01 0C` | RPM | 0 |
| `01 0D` | 车速 | 0 |
| `01 05` | 水温 | 0 |
| `01 0F` | 进气温度 | 0 |
| `01 04` | 负荷 | −1 |
| `01 11` | 节气门 | −1 |
| `01 42` | 电压 | 0 |
| `01 5C` 或厂商私有（21/22 模式）| 油温 | −100（策略因车型而异，见 [VEHICLES.md](VEHICLES.md)）|

**全部为 0 / 错误值**：轮询未开始或适配器初始化失败 → 回到第二步。
**只有部分通道没数**：该 PID 在你的车上不被支持或需要厂商私有路径 ——
先用手机 App 验证同一 PID；确实是私有 PID 的，参考
[DEVELOPMENT.md 的 PID 挖掘流程](DEVELOPMENT.md#工具脚本)挖出来再加车型配置。

### 看原始日志（需要 USB）

```
elm327_ble: Send 01 0C
elm327_ble: RAW[...]: 41 0C 12 34     ← 正常：收到响应
elm327_ble: Parsed RPM: 4660
```

```
elm327_ble: Send 01 0C
elm327_ble: RAW[...]: ?               ← 异常：协议不对 / 总线无应答
elm327_ble: PARSE_FAIL / TIMEOUT_AGAIN
```

## 第四步：综合手段

1. 断电重启整条链路：仪表 → OBD 设备 → 车辆点火，再重新连接
2. 检查设置：车型选对了吗（协议锁定 / 油温路径都随车型变）、BLE 设备名是否是目标适配器
3. 日志里反复初始化失败或 NVS 报错：USB 重刷固件（首次烧录本就会擦 NVS）；
   主从模式 WiFi OOM 崩溃时，在设置页 → MULTI-GAUGE 把角色改回独立模式（重启生效）

## 日志关键字

| TAG / 关键字 | 含义 |
|-------------|------|
| `elm327_ble` | BLE 连接、初始化、轮询、协议检测（`[DETECT]` 前缀）|
| `vp_manager` | 车型切换、验证期、安全模式、自动回滚 |
| `sd_import` | SD 卡 `/OBD/IMPORT` 导入处理结果 |
| `ui_storage` / `storage` | 存储页按钮任务 / 后台存储服务（镜像、恢复、定时扫描）|
| `theme_lib` / `theme_mount` | 主题库写入进度 / 开机或插卡时恢复主题写入 |
| `theme_engine` | 主题分区加载 / 回退 |
| `coredump` | 崩溃转储保存（配合 `GET /ota/coredump`）|
| `ota_wifi` | OTA WiFi 热点模式与固件 / 主题 / 开机动画推送 |
| `ERROR` / `WARN` | 任何模块的错误 |

日志级别调试：`idf.py menuconfig → Component config → Log output → Default log verbosity → Debug`
后重编。过滤着看：

```bash
idf.py monitor | grep -E "elm327|DETECT"
```

## 常见问题

**Q：屏幕所有参数都是 0？**
轮询未开始或初始化失败。走第一步、第二步，看日志里初始化命令是否有应答。

**Q：能连接但收不到新数据？**
协议不对（多数现代车是 6/7）；或适配器被手机 App 抢占。换协议重连试试。

**Q：转速车速正常，油温 / 负荷没数？**
油温路径因车型而异（标准 5C 只有部分车支持，丰田/马自达/BMW 走私有模式），
确认已经安装并激活了对应的车型配置（固件只内置 `Generic OBD2`，具体车型要从目录
装，见下一条）；仍不行说明该车需要新的油温公式（[VEHICLES.md](VEHICLES.md)）。

**Q：怎么判断是适配器坏了？**
手机装 Car Scanner / Torque Pro 连同一适配器。手机也读不到 → 适配器或车的问题；
手机正常而仪表不行 → 提交 issue 附日志。

**Q：BRZ ZD8（二代）用什么协议？怎么装车型？**
协议 6。固件只内置通用的 `Generic OBD2`，`subaru.zd8.obd` / `subaru.zd8.pid`
两个车型配置都在车型目录里（`vehicles/catalog/`），需要先安装：把对应的
`*.vehicle.json` 放进 SD 卡 `/OBD/IMPORT`（或用手机 App 推送），再在车型页选中
并 USE 激活，或者在 `/OBD/IMPORT` 里同时放一个写了车型 id 的 `ACTIVATE.TXT` 一步
到位。两个配置都已锁定协议 6，用标准 PID `01 5C` 读油温。

**Q：多台仪表协议会互相影响吗？**
不会。协议存在各表自己的 NVS 里，互不影响。

**Q：插了 SD 卡但仪表显示 `NO CARD` 或 `CARD ERROR`？**
存储页（设置页上滑）只支持 **FAT16 / FAT32**，exFAT 或未格式化的卡会显示
`CARD ERROR`，提示 "Use FAT32"；设备从不会自动格式化卡，需要在电脑上重新格式化
成 FAT32。显示 `NO SD SLOT` 是正常的——只有 V1 微雪板有卡槽，V2/V3 板没有。

**Q：SD 卡导入车型 / 主题没有生效？**
存储页点 SCAN 或等开机 / 插卡自动扫描后，去 `/OBD/IMPORT` 看处理结果：成功的文件
被改名为 `<文件名>.ok`，失败的是 `<文件名>.err` 并多出一个同名 `.err.txt`，第一行
是失败原因、后面是逐条校验问题；也可以直接看 `/OBD/IMPORT.LOG` 的汇总记录，或者
在 OTA 模式网页（`http://192.168.4.1/`）的 "Import log" 面板里查看。注意
`*.vehicle.json` 安装后**不会自动激活**，需要额外的 `ACTIVATE.TXT` 或去车型页手动
USE。

**Q：存储页提示 "Theme write failed" 或 "Busy, try again" / "theme library busy"？**
"Busy, try again"：有另一个 SD / 主题任务正在跑（导入、镜像、另一次主题写入），
等它结束再试。"Theme write failed: <原因>"：主题包本身有问题（损坏、超出
theme_0 分区大小、schema 版本不支持），写入失败会自动恢复原来的主题，不影响正常使用。
"theme library busy" 也会出现在 OTA 网页推送主题 / 开机动画时——这两类写入和存储页
的主题写入共用同一把锁，等 SD 侧的写入结束再从 App 推送。

**Q：车型页 VEHICLE 按钮显示 `SAFE`，或者仪表一直跑 Generic，不切车型？**
这是**安全模式**：固件连续异常重启（崩溃 / 看门狗）达到 3 次，下次开机会强制只跑
`Generic OBD2`，任何待生效的车型切换都不会被促升，SD 卡也只处理 `RESET.TXT`。
正常运行满 60 秒崩溃计数会清零、自动恢复。想立刻脱困：车型页点 **GENERIC**，或把
`RESET.TXT` 放进 SD 卡 `/OBD/IMPORT`。如果车型页显示 `FAIL`，说明是某个车型连续 3
次开机验证不过被自动回滚，不是安全模式，具体原因看车型页报错区。

**Q：怎么拿到崩溃日志（core dump）？**
进 OTA 模式，手机连 `OBD-Gauge-OTA-XXXX` 热点，浏览器打开 `http://192.168.4.1/`，
"Core dump" 面板里点 Download；也可以直接发 HTTP 请求
`GET /ota/coredump`（需要先 `GET /ota/discover` 拿 `X-OTA-Token`）。没有存储的崩溃
时返回 **204 No Content**（网页显示 "No core dump stored."），不是 404。
用完可以点 Erase 或 `POST /ota/coredump/erase` 清掉。

---

更多求助：[GitHub Issues](https://github.com/steveEcode/obd_brz_gauge/issues)，
附上 `idf.py monitor` 日志与车型 / 适配器型号。
