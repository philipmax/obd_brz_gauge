# 可编辑车型导入包

[English](README.en.md) | 简体中文

这里提供 **18 份可直接导入固件的完整 JSON 档案**（`skygauge.vehicle/1`）：
[OBD/IMPORT/](OBD/IMPORT/) 中包含现有目录 17 份档案的 `custom.*` 副本，以及
单独的 `custom.bmw.g.gear`。这些是已有请求路径的可编辑模板，适用性需要按实车确认。
原始目录仍为 17 份；模板不会占用原始 ID，通用模板也可以独立安装。
仪表中的模板名称带 `cfg` 后缀，便于与原档案区分。

## 精简配置

这 18 份完整模板仍可直接使用，也可以删去希望沿用标准 OBD2 的信号定义。
未声明的 9 个标准信号会自动补齐；显式声明整体替换默认信号。关闭某项使用
`{"id":"obd.oil_temp","disabled":true}`，关闭全部继承使用根字段
`"defaults":"none"`。默认补齐不含挡位与传动比。此行为需要包含默认继承功能的固件；
旧固件请继续使用完整模板。最小与 BMW G 直读挡位示例见
[examples/](../examples/README.md)，详细规则见
[PID_GUIDE.md](../../docs/PID_GUIDE.md)。

## 安装

1. 从 `OBD/IMPORT/` 选对应车型的 `*.vehicle.json`，复制到 SD 卡根目录下的
   `OBD/IMPORT/`，保留文件名；也可复制整个 `OBD` 目录来安装整批模板。
2. 插卡或开机后固件自动校验、编译并安装。安装成功的输入改名为 `.ok`；失败的
   输入改名为 `.err`，原因见同名 `.err.txt` 和 `OBD/IMPORT/IMPORT.LOG`。
3. 在仪表设置的 Vehicle 页选择对应 `cfg` 档案，点 USE 后重启。
   也可在 `OBD/IMPORT/ACTIVATE.TXT` 第一行写 profile id，例如：

   ```text
   custom.bmw.g.gear
   ```

   固件先安装 JSON 再处理激活文件。激活在下次启动生效；启动时导入后若仍显示
   旧车型，再重启一次。包内没有预置激活文件，由你选择车型。

使用内置标准 OBD2 不需要导入；`custom.obd2.generic` 用于定制标准请求。
HTTP 安装/激活方法见 [车型文档](../../docs/VEHICLES.md)。

## 包含的档案

| id | name | bus.proto | origin.profile_id |
|---|---|---|---|
| `custom.alfa.giulia_20t` | `GIULIA 2.0T cfg` | 7 | `alfa.giulia_20t` |
| `custom.bmw.e` | `BMW E cfg` | 6 | `bmw.e` |
| `custom.bmw.fg` | `BMW F/G cfg` | 6 | `bmw.fg` |
| `custom.bmw.g.gear` | `BMW G gear cfg` | 6 | `bmw.fg` |
| `custom.bmw.g.obd` | `BMW G OBD cfg` | 7 | `bmw.g.obd` |
| `custom.honda.integra` | `Honda Integra cfg` | 7 | `honda.integra` |
| `custom.jeep.generic` | `jeep cfg` | 7 | `jeep.generic` |
| `custom.mazda.mx5nd` | `MX-5 ND cfg` | 0 | `mazda.mx5nd` |
| `custom.mini.jcw_f56` | `JCW F56 cfg` | 0 | `mini.jcw_f56` |
| `custom.mini.r55` | `MINI R55 cfg` | 6 | `mini.r55` |
| `custom.obd2.generic` | `OBD2 Generic cfg` | 0 | `obd2.generic` |
| `custom.porsche.997_1` | `POS 997.1 cfg` | 6 | `porsche.997_1` |
| `custom.porsche.997_2` | `POS 997.2 cfg` | 6 | `porsche.997_2` |
| `custom.subaru.zc6.can` | `ZN/C6 CAN cfg` | 6 | `subaru.zc6.can` |
| `custom.subaru.zc6.pid` | `ZN/C6 PID cfg` | 6 | `subaru.zc6.pid` |
| `custom.subaru.zd8.obd` | `ZD8 OBD cfg` | 6 | `subaru.zd8.obd` |
| `custom.subaru.zd8.pid` | `ZD8 cfg` | 6 | `subaru.zd8.pid` |
| `custom.toyota.supra_a90` | `Supra A90 cfg` | 6 | `toyota.supra_a90` |

## 请求、位解码与轮询频率

每个请求来源显式提供 `hdr`、`expect` 和 `fail_after`；原有 `rx`、`raw`、
`cea`、多帧、取尾字节、有效范围、防抖及回退设置保持原值。
`bus.proto` / `atst` / `gap_ms` 属于整份档案；`src.hdr` 可逐个请求指定，
优先于 `bus.hdr`。模板已经展开 `src.hdr`，修改总线默认头时需同步相关来源的
`hdr`，或删掉来源的 `hdr` 让它继承 `bus.hdr`。
没有填写的总线配置展开为固件默认值：`proto: 0`、`hdr: "7E0"`、
`atst: 25`、`gap_ms: 30`。`atst` 是十进制数，每单位 4 ms。

请求信号统一使用 `rate: "round"` 和明确的 `period_ms`，以下是模板默认值：

| signal | period_ms | nominal Hz |
|---|---|---|
| `obd.rpm` | 100 | 10 |
| `obd.speed`, `obd.gear`, `obd.load`, `obd.throttle`, `obd.boost` | 200 | 5 |
| `obd.afr`, `obd.oil_pressure` | 500 | 2 |
| `obd.intake_temp`, `obd.coolant_temp`, `obd.oil_temp`, `obd.battery_voltage` | 1000 | 1 |

`period_ms` 是**最小轮询间隔**，不是保证达到的频率；所有请求仍串行运行，
实际刷新受适配器延迟、请求数、总线间隔及 CAN 监听窗口影响。允许范围
20..60000 ms，总请求频率预算也必须通过校验。纯 CAN 来源沿用 `monitor`，
比值推导信号没有请求间隔。

解码改 `rpn`，`expr` 仅作说明。例如 `[["R", 0, 16, "be"]]` 从解码数据第
0 位取 16 位无符号大端数；第 0 位是第一个字节的最高位。`be` 是顺序位编号，
不能直接抄 DBC Motorola 的锯齿位编号。CAN 来源也可直接修改 `bit` / `bits` /
`endian` / `signed`。需要按位解码的请求可使用 `R` 指令，具体规则和样例见
[PID_GUIDE](../../docs/PID_GUIDE.md) 与 [契约](../../contracts/vehicle-rpn.md)。

## BMW G 直接挡位、BMW E 与 Jeep

- `custom.bmw.g.gear`：复用 `bmw.fg` 的协议 6、`raw: "18 03 22 DA 2E"`、
  `hdr: "6F1"`、`rx: "618"`、`expect: "62DA2E"`。前缀后的第一个数据字节
  解码为 0..8，最小轮询间隔 200 ms；连续失败 1 次后回退齿比估算。
  这里的 0 不区分 P/N/R。沿用原 F/G 的齿比/尾牙/轮胎参数，估算回退前应按实车调整。
- `custom.bmw.g.obd`：沿用原档案的协议 7 和齿比估算。整份档案只有一个协议，
  不把协议 6 的原始挡位请求混进协议 7 档案。按实际车和适配器选择变体。
- `custom.bmw.e`：保留 `6F1` 请求头的 `224402` → `225822` 油温来源回退，
  包含各自的数据长度和换算公式。
- `custom.jeep.generic`：保留协议 7 与 29 位功能请求头 `18DB33F1`；
  仍是通用占位配置，齿比、尾牙和轮胎参数必须按车型修正。

这些配置来自仓库已有路径，本次生成没有新增实车验证，也不代表覆盖同底盘所有 ECU。

## 修改与重新生成

个人配置建议先复制到包外再修改；保留 schema，为新配置选择独立的 `custom.*` ID，
文件名对应 `<id>.vehicle.json`，更新时递增 `rev`。每份 JSON 必须小于等于
16384 字节。修改公式时同步 `expr` 和 `rpn`，固件实际执行 `rpn`。

```sh
python3 tools/vehicles/gen_import_pack.py
python3 tools/vehicles/gen_import_pack.py --check
cmake -S test/host/vehicle -B /tmp/obd-host/vehicle -G Ninja
cmake --build /tmp/obd-host/vehicle
/tmp/obd-host/vehicle/validate_catalog --strict vehicles/import/OBD/IMPORT
ctest --test-dir /tmp/obd-host/vehicle --output-on-failure
```

生成器以 `vehicles/catalog/` 为来源，**重新生成会覆盖本目录模板的个人修改**。
`--check` 只检查分发模板与来源是否一致；主机校验器使用固件的校验与编译实现，
检查请求、公式、资源上限及编译结果，不验证车辆是否应答。
