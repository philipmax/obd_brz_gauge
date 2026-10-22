# 车型 golden 基线（旧固件行为）

`<id>.json` 记录**重构前固件**（`4b056c0`，`tools/vehicles/dump_legacy.py` 的 `LEGACY_REV`）对每个旧车型
发出的 ELM327 初始化序列和轮询请求，一个旧车型一个文件，`<id>` 与 `vehicles/catalog/<id>.vehicle.json` 相同。

- 生成：`python3 tools/vehicles/legacy_golden.py`；校验是否过期：`python3 tools/vehicles/legacy_golden.py --check`
- 生成器按旧 `elm327_ble_client.c` 的逻辑（`do_elm_init`、12 槽轮询、机油温度两条路径）独立模拟旧表，
  **不复用** `legacy_to_v1.py` 的转换代码，所以转换出错会表现为不一致
- 有意的行为差异在生成器的 `DEVIATIONS` 里显式改写，并写进 `notes`
- 使用方：`test_golden`（编译后的 profile 对比，见下）；veh-runtime 的引擎测试（实际发出的命令对比）

## 格式

`bmw.fg.json` 节选（`requests` 只留两条，`notes` 省略）：

```json
{
  "id": "bmw.fg",
  "legacy": {"rev": "4b056c0", "idx": 6, "name": "BMW F/G"},
  "init": ["ATZ", "ATE0", "ATL0", "ATS1", "ATH0", "ATAT1", "ATST0F", "ATSP6", "ATSH7DF"],
  "gap_ms": 1,
  "requests": [
    {"sig": "obd.rpm", "src": 0, "hdr": "7DF", "tx": "010C", "class": "fast"},
    {"sig": "obd.gear", "src": 0, "hdr": "6F1", "tx": "180322DA2E", "class": "round", "raw": true, "rx": "618"}
  ],
  "requires": [],
  "monitor": null,
  "can": [],
  "drivetrain": {"fd": 2.813, "tire_m": 0.33, "gears": [5.25, 3.36, 2.172, 1.72, 1.316, 1, 0.822, 0.64], "tol": 0.09},
  "notes": ["..."]
}
```

| 字段 | 含义 |
|------|------|
| `legacy.idx` | 旧 `s_profiles[]` 下标（NVS 里存的旧车型号），与 profile 的 `origin.legacy_idx` 相同 |
| `init` | 规范化 AT 命令（去空格、去 CR、大写）：固定 `ATZ ATE0 ATL0 ATS1 ATH0 ATAT1`，`ATST%02X`，`ATSP<十六进制一位>`，`ATSH<hdr>`。不含其后的 3 次 `0100` 预热（旧固件和引擎都发）和引擎的 `ATRV` |
| `gap_ms` | 槽间隔（旧 `poll_gap_ms`，未设为 30） |
| `requests` | 每个请求源一条，按旧轮询槽顺序（= profile 的信号顺序）：`010C 010F 010D 0105 0104 0111 机油 0142 010B 0144 机油压力 档位`，CAN 已提供的信号没有请求 |
| `requests[].sig` / `src` | 信号 key / 该信号源链里的位置（0 = 首选，后面是连续失败后的回退） |
| `requests[].hdr` | 发这条请求时的发送头：11 位三位十六进制，29 位八位 |
| `requests[].tx` | 紧凑十六进制请求（`raw: true` 时是 `ATCAF0` 下原样发送的 CAN 载荷） |
| `requests[].class` | `fast` = 每个槽之后都插一次（`rate: "every"`）；`round` = 每轮一次；`slow` / `on_demand` 预留 |
| `requests[].raw` / `rx` / `multi` | 可选：原始帧；发送前 `ATCRA <rx>`、之后 `ATCRA`；多帧响应。缺省为 false / 无 |
| `requires` | 需要的 ELM 能力（`ATMA`） |
| `monitor` | CAN 广播监听（`probe`：`every_ms` 一次窗口、窗口 `window_ms`、`stale_ms` 无帧则失效），无则 `null` |
| `can` | CAN 广播规则 `{sig, id, bit, bits}`，比较时不看顺序 |
| `drivetrain` | 终传比、滚动半径、各档速比、容差（无档位表时 `gears` 为空、`tol` 为 0） |
| `notes` | 与旧行为的有意差异，人读 |

## 比较规则

`test_golden`（`test/host/vehicle/test_golden.c`，ctest 名 `vehicle_golden`）用固件默认配置编译
`vehicles/catalog/<id>.vehicle.json`，然后要求：

1. `init` 完全相等：固定六条 + `ATST<p->atst>` `ATSP<p->proto>` `ATSH<p->hdr>` + profile 的 `bus.init`
2. `requests` 按顺序逐条相等：遍历信号、再遍历它的请求源，取请求表里的 `hdr`/`tx`/`RAW`/`RX`/`MULTI`，
   `class` 来自信号的 `rate`
3. `gap_ms`、`requires`、`monitor`、`can`（集合）、`drivetrain` 相等
4. 每个带 `origin.legacy_idx` 的目录 profile 都有 golden 文件

引擎测试（veh-runtime）对比实际发出的命令时建议：

- 发送头按懒切换：只在头变化时出现 `ATSH`；`raw` 请求包在 `ATCAF0` / `ATCAF1` 之间，`rx` 请求前后是
  `ATCRA <rx>` / `ATCRA`
- 全部请求都有正常响应时，每轮只发 `src: 0` 的请求；`fast` 类在每个其他槽之后各出现一次
- 按 `class` 比较 `(hdr, tx)` 集合即可；同一个 `(hdr, tx)` 被多个信号共用时每轮只发一次
- `ATSP0` 表示 profile 未指定协议：引擎用 `vp_engine_cfg_t.proto`（保存的或自动检测到的），与旧固件
  用 `nvs_user_cfg_t.protocol` 一致

## 已知有意差异（生成器写进 `notes`）

| 车型 | 差异 |
|------|------|
| `mini.r55` | 机油温度 `225822` 旧固件经功能头 `7DF` 发出（枚举路径没有 `ATSH7E0` 包裹）；v1 拒绝在功能头上发 22 服务，改走 `7E0` |
| `bus.proto` 为 0 的车型 | `init` 里是 `ATSP0`，实际协议由引擎配置给出（见上） |
| 带增压的车型 | 旧固件把 `(MAP-100)/10` 截断成 0.1 bar；v1 存 0.01 bar |
| `subaru.zc6.can` | CAN 超过 `stale_ms` 无帧时旧固件把水温写成 -40；v1 置为无效 |
| 读档位 DID 的车型 | 旧固件每个越界样本都回落到速比推算；v1 用 `fail_after: 1` + `fallback: "ratio"` |

不影响请求集合、未单独记录的差异：旧固件即使某槽被跳过（例如 CAN 已提供水温）也会在该槽后补发 `010C`；
旧固件按截断取整，v1 按四舍五入。
