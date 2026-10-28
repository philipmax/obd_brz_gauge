# obd_brz_gauge 全面重构计划

- 日期：2026-10-11
- 分支：`rebuild`（从 `origin/main` 4b056c0 切出）
- 状态：代码/架构层面已完成（见第 12 节），尚未上板验证
- 依据：对固件仓库和 `obd_theme_store`（只读）的多轮代码调研。所有结论都给了文件/行号依据；标「推断」或「需上板验证」的除外。

---

## 0. 范围与已拍板的约束

| # | 决定 | 对设计的影响 |
|---|------|-------------|
| 1 | **只重构本仓库（固件）**。编辑器/网页（`obd_theme_store`）、手机 App、backend 一律不改 | 固件去适配编辑器现有的输出格式和 App 现有的接口，不能要求客户端配合 |
| 2 | **硬件不能改**。只针对 V1 微雪 ESP32-S3-Touch-LCD-1.85，V2/V3 暂时不管 | SD 用 V1 自带的 TF 槽，只能走 SDMMC 1-bit |
| 3 | 2GB SD 卡存放**所有视频和图片资源** | 新增 SD 驱动、流式播放器和 SD 资源加载 |
| 4 | 视频要**同时支持开机动画和主题** | 需要两种渲染：全屏独占、主题合成 |
| 5 | 固件**只内置 OBD2 通用**车型，其余车型（包括 ZC6）一律导入 | 删除编译期车型表；导入通道只能由固件自己提供 |
| 6 | 这次发版要求**用网页全量刷新一次**，之后只做单固件 OTA | 新分区表只通过网页下发，并且要经得住网页刷机往固定地址的写入 |
| 7 | App 侧的跨布局保护**不做** | 不改 `bootmedia_format`，也不依赖 NAS 上的 `hardware.json` |
| 8 | 删除所有无用代码 | 见第 6 节，清单已经独立复核过 |
| 9 | **删除刹车温度功能**（RS485 红外刹车温度传感器，BKT） | 驱动、Kconfig、数据项、RaceChrono F109 一并移除；凡是持久化或上线传输过的槽位只做占位、永不复用（见 6.4） |
| 10 | **主题完整支持带透明通道的图片** | 统一转成 LVGL 原生 `RGB565A8`，页面构建时转换进 PSRAM（见 5.7） |

---

## 1. 关键结论

1. **app 槽只能保持 3MB。** 网页刷机写主题时固定从 `0x620000` 开始写，所以两个 app 槽必须都放在 `0x20000–0x61FFFF` 这 6MB 以内，每个最多 3MB。新固件估算约 2.55MB（占 81%），算上 25% 余量约 2.63MB（占 83.6%），放得下。
2. **媒体搬到 SD，换来的是容量，不是 app 空间。** 固件里实际链接进去的图片只有 35KB（`images/` 下那 10MB C 数组本来就没编进去）。flash 上的 `theme_0`（4MB）和 `bootmedia`（4.75MB）不能挪给 app，因为网页刷机会往这两段硬写。所以它们改当「导入收件箱 + 当前主题/兜底开机动画的缓存」；SD 存主题库、视频和大图。
3. **V1 上 SD 只能走 1-bit。** 读速估计 2–3.7MB/s，需上板验证。裸 RGB565 视频不可行，必须用 MJPEG。
4. **固件兼容编辑器，不反过来。** 编辑器导出的 `theme.bin` 不含规则，规则只存在于编辑器能下载的 `<id>.gaugeproj.json` 里。固件要能导入这个文件当规则旁路（sidecar），规则语义逐位照搬 `ruleEngine.ts`。
5. **先定 3 份「单一事实源」，再动业务代码：**
   - `partitions.csv` 生成布局 JSON；
   - 信号注册表 `signals.json`：同一批数据源现在有 5–6 套平行定义；
   - 编辑器兼容契约：`theme.bin` 格式加规则 JSON 格式。
6. **内部 RAM 是头号风险。** 规则引擎在 `931008d` 加进来、在 `aaf6e16` 被回滚，从 map 文件看是因为静态 BSS 多占了 16KB 内部 DRAM（推断，回滚原因没有记录）。新模块一律放 PSRAM 动态分配，CI 里给 `.dram0.bss` 的增量设门限。

---

## 2. 目标架构

```
                 ┌──────── 不改的客户端 ────────┐
                 │ App: /ota/firmware /ota/theme* /ota/bootmedia*        │
                 │ 网页刷机: 0x0/0x8000/0xF000/0x20000 + 0x620000 + 0xA20000 │
                 │ 编辑器: <id>.bin  +  <id>.gaugeproj.json (含规则)      │
                 └───────────────┬──────────────────────────────┘
                                 │
  ┌──────────────────────────── 固件 ───────────────────────────────────┐
  │ 导入通道: SD /OBD/IMPORT │ 设备自带网页 (SoftAP /api/v1) │ 设备设置页 │
  │                                                                       │
  │ Flash: nvs │ nvs_app │ ota_0/ota_1(3M) │ theme_0(当前主题缓存/收件箱)  │
  │        bootmedia(兜底开机动画/收件箱) │ coredump │ config(LittleFS)   │
  │ SD(FAT, 32KB 簇): /OBD/{IMPORT,THEMES,VIDEO,BOOT,CFG,CATALOG,EXPORT} │
  │                                                                       │
  │ ELM327 ─▶ 通用请求/解码引擎(profile + RPN) ─▶ 信号表(int32+valid+ts)  │
  │ ADS1115/(TPMS) ────────────────────────────▶        │                 │
  │                     ┌───────────────────────────────┼────────┐        │
  │                主题引擎(绑定 + 规则)            ESP-NOW v6   RaceChrono │
  │                     │                                                 │
  │                   LVGL ◀── 视频播放器 (SD→1s 环形缓冲→MJPEG→条带/合成) │
  └───────────────────────────────────────────────────────────────────────┘
```

---

## 3. 分区表 v2（最终方案）

### 3.1 CSV

已用 `gen_esp32part.py`（v5.5.3，`--flash-size 16MB`）校验通过。

```
# Name,     Type, SubType,  Offset,    Size,      Flags
nvs,        data, nvs,      0x9000,    0x6000,
otadata,    data, ota,      0xF000,    0x2000,
nvs_app,    data, nvs,      0x11000,   0xF000,
ota_0,      app,  ota_0,    0x20000,   0x300000,
ota_1,      app,  ota_1,    0x320000,  0x300000,
theme_0,    data, spiffs,   0x620000,  0x400000,
bootmedia,  data, 0x82,     0xA20000,  0x4C0000,
coredump,   data, coredump, 0xEE0000,  0x20000,
config,     data, littlefs, 0xF00000,  0x100000,
```

- **与现行表的差异：**
  - `nvs`、`otadata`、`ota_0`、`ota_1`、`theme_0` 这 5 条逐字节相同；
  - 删除没用的 `phy_init`（`CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` 本来就没开）；
  - `bootmedia` 从 5.875MB 缩到 4.75MB；
  - 新增 `nvs_app`、`coredump`、`config`。
- **分区类型：** `theme_0` 和 `bootmedia` 的 subtype 都是 0x82，代码一律按 label 查找。
- **layout_id：** 取全部 9 个条目的 CRC32，为 `0x94752aa3`（现行表是 `0xefaddf47`）。

### 3.2 与网页刷机写入地址的对照

| 区间 | 分区 | 网页 complete 模式 | 网页单独写入模式 |
|---|---|---|---|
| 0x0–0x5FFF | bootloader | 写 bootloader | – |
| 0x8000 | 分区表 | 写入 | – |
| 0x9000–0xEFFF | nvs | **不写**：设置和 BT 绑定保留 | – |
| 0xF000–0x10FFF | otadata | 写全 0xFF，复位后启动 ota_0 | – |
| 0x11000–0x1FFFF | nvs_app | 任何写入都碰不到 | 同左 |
| 0x20000–0x31FFFF | ota_0 | 写固件 | firmware 模式写这里 |
| 0x320000–0x61FFFF | ota_1 | 不写 | 不写（主题从 0x620000 起写，刚好在 ota_1 之后） |
| 0x620000–0xA1FFFF | theme_0 | 选了主题才写，≤4MB | theme 模式；清主题时只写 4KB 0xFF |
| 0xA20000–0xEDFFFF | bootmedia | 选了开机动画才写，≤0x5E0000 | bootanimation 模式 |
| 0xEE0000–0xEFFFFF | coredump | 只有总长 >4,980,736 B 的开机动画才会写到 | 同左 |
| 0xF00000–0xFFFFFF | config | 只有总长 >5,111,808 B 的开机动画才会写到 | 同左 |

**结论：** 不论哪种网页写入、设备当前跑在 ota_0 还是 ota_1，都不会破坏 app 分区。超大开机动画覆盖 config 时，一定先打到 LittleFS 超级块，挂载必然失败；固件随即格式化，并从 SD 镜像恢复。当前车型另有一份存在 `nvs_app` 里。

### 3.3 各分区的角色

| 分区 | 用途 |
|---|---|
| `nvs`（24KB） | 只放 IDF 的 phy/bt/wifi 数据和 `cfg`。`nvs_user_cfg_t` 只在末尾追加字段 |
| `nvs_app`（60KB） | 唯一完全不会被网页写到的数据区。命名空间：`sys`（magic `'NAP2'`、layout_crc、mig_ver）、`imp`（收件箱 CRC 账本）、`veh`（当前车型 id 和 blob ≤8KB、上一个、pending 状态）、`stat`/`diag`（预留） |
| `theme_0`（4MB） | 网页和 App 主题的收件箱，同时是**当前主题的 mmap 零拷贝缓存**。现有 4MB 主题包、`/ota/theme/prepare` 返回的 `partitionSize` 都不用变 |
| `bootmedia`（4.75MB） | 旧格式 delta_varint 开机动画的收件箱，也是没插卡时的兜底动画（改成流式播放） |
| `coredump`（128KB） | ELF + CRC32 格式。开机检查，可通过 HTTP 拉取 |
| `config`（LittleFS，1MB） | 导入的车型 profile、规则 sidecar、索引。导入时占用上限 75%；每次修改同步镜像到 SD `/OBD/CFG` |

### 3.4 收件箱：每次开机执行

- **theme_0：**
  - 先解析 manifest（`schema_version=="1.0"`）；
  - CRC 和账本对不上，说明是网页或 App 新写进来的：复制到 SD `/OBD/THEMES/<id>/theme.bin`（先写 `.part` 再改名），并设为当前主题，**以最近一次写入为准**；
  - 第一扇区全 0xFF，说明用户清空了主题：改用内置默认主题；
  - 从 SD 主题库切换主题时，写回 theme_0。写入期间状态置 `writing`，断电后能从 SD 重做。
- **bootmedia：**
  - 校验 manifest（首字节 a–z，格式是 delta_varint v1/v2）；
  - 新的就复制到 SD，没插卡时直接从 flash 流式播放；
  - App 的 `/ota/bootmedia` 上传：不超过分区容量就照旧写分区；超过了但插着 SD 就流式写 SD；否则返回 413。

### 3.5 全量刷新后第一次启动的迁移

整个流程可重入，中途断电后重来是安全的。

1. 照常执行 `nvs_flash_init()`（0x9000 原样保留）。
2. `nvs_app` 初始化出错，或 magic 不对：擦除，再写 magic。
3. 如果 `mig_ver<1`：
   1. 挂载 SD（尽力而为，失败不阻塞）；
   2. **先导入旧 bootmedia。** 旧分区是 0x5E0000，旧码流可能延伸进新的 coredump/config 区，必须在格式化之前读出来；
   3. 导入旧 theme_0；
   4. 擦除 coredump；
   5. 格式化 config，再从 SD 镜像恢复；
   6. NVS cfg 从 v3 迁到 v4：**不扩大 `nvs_user_cfg_t`**。blob 一旦变大，旧固件读它时会因为长度不符（`ESP_ERR_NVS_INVALID_LENGTH`）整份读失败、所有设置回到默认值，OTA 回滚就不安全。所以新字段（`vehicle_profile_id`、TPMS 等）一律放进 `nvs_app` 的独立 key。`vehicle_profile_idx` 为 0 映射到 `obd2.generic`，1..16 映射到车型目录 id；idx 字节本身保留不动，并持续回写，作为降级镜像；
   7. 最后写入 `layout_crc` 和 `mig_ver=1`。

### 3.6 发布产物（不改客户端的前提下）

| 消费方 | 读取位置 | 文件 |
|---|---|---|
| 编辑器 backend 的 firmware-watcher（只看 `origin/main`） | `build/` | 必需：`obd_brz_gauge.bin`、`bootloader/bootloader.bin`、`partition_table/partition-table.bin`、`ota_data_initial.bin`；可选：`bootmedia.bin`。供网页 complete 模式使用 |
| NAS `obd_ota` publish-release.mjs | `firmware/release/` | 必需：`obd_brz_gauge.bin`、`bootmedia.bin`；可选：其余三件。供 App OTA 使用 |

- **`build/` 的跟踪方式：** 不能整个取消跟踪。`.gitignore` 忽略 `build/*`，再用白名单保留上面 5 个文件，其余约 2260 个产物用 `git rm --cached` 取消跟踪。
- **合并时机：** `rebuild` 合入 `main` 的那一刻，就等于向网页刷机发布了新固件。
- **`release.sh`：** 有「一版滞后」的 bug，新编译的产物要到下一次发版才被提交。还有 count 不一致：发版提交是 N+1，固件里编进去的是 N。这两处都必须在发版前修好。
- **`bootmedia.bin`：** 改为 raw 格式 `[manifest 用 0xFF 填到 4KB][bin]`，删除 CMake 的 SPIFFS 镜像规则。

---

## 4. 阶段计划

```
P0 基线与清理 ─▶ P1 契约与测试基建 ─▶ P2 存储底座 ─┬─▶ P3 车型数据化与导入
                                                    ├─▶ P4 事件系统（编辑器兼容）
                                                    └─▶ P5 媒体：视频与 SD 图片
                                       P3+P4+P5 ─▶ P6 发布：网页全量刷新与文档
```

规模标注：S（小）/ M（中）/ L（大）/ XL（很大），只表示相对工作量。

### P0 基线与清理

| 工作项 | 规模 |
|---|---|
| 取消跟踪 `build/`，只保留白名单里的 5 个文件；修 `.gitignore`（PID 挖掘产物的规则写错了）；`git gc`，273MB 预计降到约 160MB。是否用 filter-repo 改写历史另行决定 | S |
| 删除第 6.1 节里「已确认无用」的项目 | S |
| 删除已经并入 main 的分支（`theme-upgrade`、`feature/theme-partition-system` 等），清理过期 stash | S |
| 删除整个 BLE OTA 服务 0x1FFB（`ota_update_ble.c`，937 行）和 BLE 设备信息服务 0x1FFA。原因：没有客户端、写入不需要加密、开机动画路径会先擦后失败 | S |
| 重新生成最小化的 `sdkconfig.defaults`（现在是 IDF 5.3.2 时代的 2635 行全量导出），合并 `.esp32s3` 那份；关掉 LVGL examples/demos；删掉 `knob`/`button` 依赖 | M |
| 搭建 host 端单测工程（`test/host`，CMake + Unity），新增 CI；用 `tools/check_bss.py` 卡住 `.dram0.bss` 的增量 | M |
| **上板基线测量**：SD 1-bit 20/40MHz 的读写速度矩阵；`esp_new_jpeg` 解 360×360 一帧的耗时；BLE+ESP-NOW 同时运行时内部 RAM 的余量和最大连续块；WiFi 实际吞吐。这里有一处矛盾：memory 记录 32KB 分块才稳定，而 App 现在用 256KB 分块 | M |
| **内存预算总表**：把视频环形缓冲、帧缓冲、主题素材、规则 arena、BT/WiFi 统一放进一份 PSRAM 预算；SD 中转缓冲、JPEG 解码器、各任务栈统一放进一份内部 RAM 预算；加运行时埋点，打印 free/largest_free_block | S |
| **先保存 TPMS 代码**：`stash@{0}` 是 TPMS 的未提交代码。先用 `git stash branch` 把它存成分支，再做任何仓库清理（尤其是 filter-repo），否则可能丢失 | S |

**验收：** 构建通过；CI 跑通；拿到基线测量数据；`.git` 体积下降。**拿到上板实测数据之前，不要把 OBV1 的码率、环形缓冲大小和帧率写死进规范。** 基线要在删除刹车温度**之后**测：它把控制台用的 UART0 改成了 9600 波特，ESP_LOG 会忙等，之前的 WiFi 吞吐和帧时间数据可能都受了干扰。

### P1 契约与测试基建

| 工作项 | 规模 |
|---|---|
| `contracts/signals.json`：生成 C 头文件和枚举。编辑器的 11 个 `obd.*` key、编码和单位**原样保留**（油压按 `themeContract.ts` 是 PSI 整数）；新增 `obd.load`、`sys.*`（刹车温度已删除，不设 `obd.brake_temp`）；车型自定义信号放在 `veh.*`，在 0x0100 段动态分配，最多 32 个 | M |
| `contracts/theme-editor-contract.md`：把编辑器现有 `theme.bin` 和 `gaugeproj` 的格式、各元素的语义写成固件要遵守的契约（第 5.3 节） | S |
| `contracts/vehicle-profile.schema.json`（`skygauge.vehicle/1`）和 RPN 操作码规范 | M |
| 规则黄金向量：38 组、79 步（13 条来自原 `ruleEngine.test.ts`，另有 25 条边界用例），已用编辑器 `ruleEngine.ts` 原文验证通过；另加 24 条 label 格式化向量。全部入库 `contracts/fixtures/`，CI 里 C 端和 node 端（只读 checkout 编辑器的固定 commit）跑同一套 | M |
| `tools/gen_layout.py`：从 `partitions.csv` 生成 `partition_layout.json/.h`（含 `PARTITION_LAYOUT_CRC`）；`--check` 选项校验 FLASH.md 等文档 | M |

**验收：** 黄金向量在 node 下全部通过；生成物纳入 CI。

### P2 存储底座

| 工作项 | 规模 |
|---|---|
| 换上分区表 v2；同步 `docs/FLASH.md`(+en)、`firmware/README.md`、README | S |
| `nvs_app` 初始化；首次启动迁移（3.5）；NVS cfg v4 迁移 | M |
| `config` 分区：LittleFS（`joltwallet/littlefs`），`format_if_mount_failed=false`，开机自检，SD 镜像，配额 | M |
| coredump 写 flash（ELF/CRC32/CHECK_BOOT），提供 `GET /ota/coredump` 和 `POST /ota/coredump/erase` | S |
| SD 驱动 `sd_card.c`：SDMMC slot1、1-bit（CLK14/CMD17/D0 16）；D3 在 `EXIO_Init` 之后本来就是高电平，加上 10K 上拉，不用额外处理；先试 40MHz，失败降 20MHz；`format_if_mount_failed=false`；没有卡检测脚，用轮询探测插入、CMD13 检测拔出；设置页提供弹出按钮；所有 SD I/O 经 16KB 内部 DMA 中转缓冲，按 512B 对齐的块读写，**禁止对 PSRAM 缓冲直接 f_read/fwrite**（S3 的 SDMMC 不能 DMA 到 PSRAM，会退化成一次一个扇区） | M |
| sdkconfig：`FATFS_LFN_HEAP`、`MAX_LFN=255`、`API_ENCODING_UTF_8`、`USE_FASTSEEK`、`USE_DYN_BUFFERS`；用户需要把卡格式化成 **32KB 簇** | S |
| 收件箱导入（3.4）；把两份重复的 flash 写入实现（`theme_mount.c`/`boot_media_mount.c`，各有一个 16KB 静态内部 RAM 缓冲）合并成一份，缓冲只在写入时分配 | L |
| 上传统一成 chunk sink，后端可插拔：固件仍是「先收进 PSRAM 再写 flash」；SD 用 `.part` 流式写、增量 SHA、每 1MB fsync、最后改名；LittleFS 写配置。旧的 `/ota/*` 协议原样保留 | M |
| httpd 改用通配匹配（`httpd_uri_match_wildcard`），6 个 OPTIONS 合并成一个 `/*`，新增 `/api/v1/*` 分发器；handler 数从 18/20 变为约 15/24；`max_req_hdr_len=1024` | M |
| （可选）旧布局最小守卫：按 label 查分区，缺分区时降级运行、不崩溃。按决定 7，不做 App 侧拦截，也不做专门的回滚页 | S |

**验收：** 网页 complete 刷写后设置和 BT 绑定都在；插拔卡和断电各 50 次后挂载成功率达标；收件箱导入可重入。

### P3 车型 profile 数据化与导入

| 工作项 | 规模 |
|---|---|
| 信号存储 `signal_store`：int32 值、有效位图、时间戳、seq；`obd_data_set_*` 改成包装函数；各处哨兵值（-1/-100/-32768/127/-40）一律映射成「无效」 | M |
| 安全的 RPN 执行器：固定操作码、≤32 条指令、栈深 ≤8、无跳转、导入时静态检查，可在 host 上编译 | S |
| 通用请求/解码引擎：替换 12 槽 switch 和两套并行的油温机制；应答按在途请求期望的前缀匹配，不再全局 `strstr`；头切换、ATCRA、CAF0 包装、ATMA 监听窗口都从 profile 生成；保持单线程状态机（AGENTS.md 红线） | L |
| 内置 `obd2.generic`：由 JSON 生成 C 常量 | S |
| 校验器：服务号白名单（只允许 01/09/21/22 和受控裸帧；拒绝 04、0x10/0x11/0x27/0x2E/0x31 等）；AT 白名单（SH/CRA/CEA/CAF/ST/SP 子集，禁止 ATPP/ATBRD/ATZ 插入）；数量和大小上限 | L |
| 生命周期：INSTALLED → PENDING → ACTIVE，失败为 FAILED。验车只在 ELM 已连接且总线醒着（ATRV ≥13.0V 或已有正响应）时计时，窗口 20 秒，K 个 PID（K=min(3,n)）有响应，有 rpm 时 rpm 必须在内。连续 3 次启动仍未通过就回滚到上一个，再不行回滚到 generic；崩溃计数进入安全模式；手动复位入口有开机长按、BOOT 键、SD 上的 `RESET.TXT`、设置页 | M |
| 转换器：`tools/vehicles/`（`dump_legacy.py` 从旧表导出中间 JSON，再由 `legacy_to_v1.py` 转换），把 17 个旧车型转成 17 个 JSON 放进 `vehicles/catalog/`；**不内嵌进固件**（决定 5） | S |
| golden 请求序列测试：17 个车型逐一对比新引擎和现行固件发出的 AT/请求序列；ZC6 的 RPM 刷新率做实车 A/B 对比 | M |
| SD 投放 `/OBD/IMPORT`：`*.vehicle.json`、`*.gaugeproj.json`、`*.bin`；处理结果改名为 `.ok`/`.err`（失败另写 `.err.txt`），追加 `IMPORT.LOG`（CRLF）；`ACTIVATE.TXT` 和 `RESET.TXT` 两个指令文件；投放的文件只安装、不自动激活；OTA 模式期间暂停扫描 | M |
| 设备自带网页（SoftAP）：单文件 vanilla JS，gzip 后 ≤48KB，超了构建失败；`GET /` 只在请求 `Accept: text/html` 时返回页面，否则仍返回 204；`/generate_204`、`/connectivity-check` 原样保留；`/api/v1/{info,vehicles,theme/sidecar,fs/put,fs/list,logs,sd,time}`，统一错误码；网页自己的 JS 用 32KB 分块上传。**设备网页只在 OTA 模式下可用**（进 OTA 会释放 BT，退出必须重启），所以导入完成后屏幕上提供「应用并重启」，验车计时从重启后 ELM 链路建立时开始 | L |
| OTA 页显示真实 SSID（现在写死成 `OBD-Gauge-OTA-xxxx`），不依赖任何决策 | S |
| LVGL 车型页和存储页：选择、回滚、恢复通用、显示错误；SD 状态、扫描、弹出、导出；设置页的车型行改成按钮加状态徽标 | M |
| ESP-NOW 升到 v6：改成 TLV，加入 gear、valid_mask、profile hash，从表不再需要本地 profile；RaceChrono 保留 F101–F108，F109（原刹车温度）退役、永不复用，新信号从 F10A 开始分配；v6 TLV 不定义刹车类型，v5 包里的刹车槽恒为 -1000，直到 v5 退役。**兼容过渡期**：从表同时接受 v5 和 v6；主表确认所有从表都升级之前继续发 v5，因为逐台升级期间版本严格相等会让从表静默断开。**包大小**：ESP-NOW 单包上限 250B，12 个标准信号加 32 个 `veh.*` 再加 TPMS 会超限，需要分包，或只发有效且被绑定的信号 | M |
| BLE 回调分发器 `ble_hub`：GAP/GATTC/GATTS 事件按 app_id 分发，统一仲裁扫描和连接。这是接入 TPMS（以及以后任何新 BLE 功能）的前提。stash 里的 TPMS 代码直接注册回调、不做转发，合进来会让 ELM327 断连 | M |
| 迁移后删除编译期车型表（第 6.2 节） | S |

**验收：** golden 请求序列全部一致；导入非法 profile 被拒绝并给出可读的错误；ZC6 实车刷新率不低于现状。

### P4 事件系统（以编辑器现有格式为准）

| 工作项 | 规模 |
|---|---|
| 主题加载加固：manifest 用 `cJSON_ParseWithLength`（现在对不以 NUL 结尾的 16KB 缓冲调 `cJSON_Parse`，会越界读）；首字节为 0xFF 视为空槽；校验 offset/size/w·h·bpp；名字改用动态字符串；给商店里没经过诊断的主题加防御性限额 | M |
| 资源管线（透明图，见 5.7）：`rgb565` 大端零拷贝；`rgba8888`（4 字节/像素）在**页面构建时**转成 LVGL 原生 `RGB565A8`（平面格式，3 字节/像素）写进 PSRAM：可见区裁剪、圆屏四角当 don't-care、按 alpha 求包围盒裁边、裁边后全不透明的降成 `TRUE_COLOR`、alpha bleed 防旋转黑边；转换核心是纯 C，可在 host 上测；缓冲一律 `MALLOC_CAP_SPIRAM`（小于 16KB 的普通 malloc 会落进内部 RAM） | M |
| pointer 预缩放（预乘 alpha 的面积平均/双线性）；rotation≠0 的静态 image 预烘焙旋转；规则颜色（recolor）预烘焙，pointer 总是烘焙，其他图单个变体 ≤128KB 才烘焙 | M |
| 图像缓存 `theme_img_cache`：PSRAM LRU，键为 (账本 CRC, 资源, 变体)；上限无视频 2.5MB、有视频槽位 1.5MB；超预算时先保证 pointer 和规则目标，剩下的顶层装饰图跳过并在报告里列出；后台预热相邻 ±1 页；进入 OTA、切换主题、擦写 theme_0 之前全部释放（擦写前还要先删主题对象） | M |
| 元素构建与编辑器逐项对齐：取整统一用 `R(v)=floor(v+0.5)`；arc 半径取 w/2、range 归一到 0..10000；bar 用两个矩形实现（`lv_bar` 在 h>w 时会自动变竖向）；label 移植 `formatIntPrintf`，不能用 vsnprintf；image 只裁剪不缩放、pivot(0,0)；**新增 pointer**；颜色接受 `0x`、`#` 和裸 6 位十六进制；字号 >48 时回退 | L |
| 规则核心（纯 C、host 可编译）：直接解析编辑器的 camelCase `GaugeRule`，包括旧的单条件写法；逐位照搬 `ruleEngine.ts` 的语义（见 5.4）；规则表放 PSRAM，按页分配，**内部 BSS 增量为 0** | M |
| 运行时接入：在 `my_timerMain` 里先更新绑定，再用同一份快照求值；比较期望值和已应用值，只对变化的属性调用 LVGL setter；规则失效时回到 base；绑定值没变就不调 `lv_label_set_text_fmt`；系统报警（RPM 闪烁、阈值变红）优先于主题规则；修复主题页滑动时旧屏对象不释放的泄漏；修复 RPM 闪烁结束后主题页背景被改成 `UI_COLOR_BG` | M |
| 规则送进设备的方式（sidecar）：从 `gaugeproj` 抽出 `pages[].id`、`elements[]`（id 和 kind）、`rules`，按 `theme.id + 页 id + 元素 id` 和已安装的 `theme.bin` 配对，生成配对报告；存进 config `/themes/<id>/sidecar.json` 并镜像到 SD。主通道是设备网页：浏览器解析 gaugeproj，只上传几 KB；次通道是 SD 投放，设备端用流式过滤器跳过 assets/fonts | L |
| 数据修正：主题快照里油压换算多除了 10（5bar 显示成 7PSI，需实车确认）；温度改成有符号（负温度时 label 显示 `-5` 而不是 `0`，文档写明）；挡位未知时交付 NaN，不再交付 127，否则 `gear>=5` 这类规则会在未知时误触发；无效时显示编辑器约定的值 | S |
| 主题页入口：现在只有主题里有 id 为 `main_gauge` 的页才开放入口（`ui.c:934`、`ui.c:1320`）。用户在编辑器里改了页名，整套主题就进不去。改成只要 `theme_pages` 非空就进第一页，`main_gauge` 只作为优先匹配；加一个改名页的回归用例 | S |
| **overlay（必做）**：从 sidecar 补回 `theme.bin` 没导出的字段，包括 label/bar 旋转、bar 方向、`dataDisplay`。编辑器预览里 arc/bar/pointer/label 的数值都要经过 `transformDataValue`（scale、offset、输入到显示的映射，量程取 `displayMin/Max ?? rangeMin/Max`），之后再处理 valueMap、precision、unit。不实现的话，预览和设备会对不上。黄金向量里补上 `transformDataValue` 的用例。只装 bin 的设备（App、商店、网页刷机）拿不到 overlay，这是固有限制，要在配对报告和文档里写明 | M |
| 编辑器兼容目标锁定在 `obd_theme_store@5a7484e`（`codex/local-frontend`，规则系统只存在于这个还没合并的分支）；同时要能接受它 main 分支的产出（缺 rotation 按 0 处理，没有 rules 就是没有规则）。CI 交叉校验固定用这个 commit | S |

**验收：** 黄金向量在 C 端全部通过（逐字节对比）；用编辑器导出的样例主题和 gaugeproj 上板，效果与预览一致；`.dram0.bss` 增量不超过 256B。

### P5 媒体：视频（开机 + 主题）与 SD 图片

| 工作项 | 规模 |
|---|---|
| 接入 `esp_new_jpeg`（锁定精确版本），全局只有一个解码句柄（约 10KB 内部 RAM）；开机自检：解一帧内嵌的 360² 测试图，判断 block 模式输出是否正确 | S |
| OBV1 容器解析器（64B 头 + 帧索引 + baseline MJPEG 帧，带 CRC 和帧大小上限）和 host 单测 | M |
| 播放器核心：读取任务（core0，优先级 3，经 16KB 内部 DMA 中转缓冲）→ PSRAM 环形缓冲（按整帧存放，默认 1.25MB，开播水位 0.3–0.5s，目标是缓冲满 1 秒）→ 解码任务（core1，优先级 5）；状态机；自动降帧（L0 → 1/2 → 1/3 → 冻结） | L |
| 全屏独占模式（开机、展厅）：暂停 LVGL，借用 LVGL 现有的两块内部 DMA 缓冲，按块解码后按条带 ping-pong 推屏；**绝不从 PSRAM 缓冲调 `draw_bitmap`**（esp_lcd 每块会临时申请 28.8KB 内部 RAM，有 OOM 风险）；默认 24fps，实测单帧 ≤26ms 才开放 30fps | M |
| 主题合成模式：整帧解码到 PSRAM 双帧缓冲，**用自定义的 `video_view` 控件显示，不用 `lv_img`**。LVGL 8.4 的 `lv_img` 永远不报告「覆盖」（`lv_obj.c:511-513`、`lv_img.c:548`），视频矩形下面的所有图层每帧都会重画。`video_view` 是普通 `lv_obj` 挂用户 DRAW_MAIN 和 COVER_CHECK 回调，不改 LVGL。预期（推断）：360² 无叠加约 30fps（受解码限制），叠半透明大图时降到 10–27fps；240² 基本都能 30fps。页面构建时按叠加层的透明度直方图估算每帧成本，选初始帧率档位 | M |
| 主题视频约定（编辑器不改）：图片元素的 id 以 `vid_` 或 `video_` 开头，就在它的矩形里播放 SD 上 `/OBD/THEMES/<theme_key>/<key>.obv`，找不到再查 `/OBD/VIDEO/<key>.obv`；这张图片本身当海报和兜底；规则 `visible` 控制暂停/继续，`asset` 借助 gaugeproj 里的 `fileName` 切换视频；每页同时只解码 1 路；写 `THEME.TXT` 提示每个视频槽位的矩形和建议分辨率 | M |
| 开机来源：SD 优先，旧格式其次，内置 Logo 最后；推荐**以最近一次更新为准**；目标是从 `app_main` 到第一帧 ≤1s | M |
| 旧格式兼容：保留 delta_varint 播放器，但改成从 flash 按 16KB 分块流式读，不再整包 malloc 进 PSRAM；增加大小检查；`device_identity` 保持 `bootmedia_format=1` | S |
| SD 图片：主题资源以 `theme.bin` 为单位，进库到 SD，当前主题缓存在 theme_0 走 mmap；透明图在页面构建时从 theme_0 现转（从 SD `.cache` 读反而慢 2–4 倍，不做缓存）；SD 上主题外的图片用预转换的 `.lvi` 格式（`tools/make_lvi.py`，大端 RGB565 或 RGB565A8，带 CRC），不做运行时 PNG 解码；**系统页用的黑耳图（35KB）和 Typoder 字体继续编在固件里**，因为没插卡时系统页也必须能用；Montserrat 14–48（约 200KB）是 app 空间的后备，必要时挪到 SD | M |
| `tools/make_video.py`（替代 `make_boot_block.py`）：`boot`/`theme`/`inspect`/`verify` 四个子命令；`theme` 子命令按元素矩形自动选尺寸并输出海报 PNG；开机视频默认 4:2:2，避开 360/16 除不尽导致 4:2:0 最后半行 MCU 的问题；单帧上限 48KB | M |
| 进入 OTA 模式前释放视频和主题的转换资源，给整包上传腾出最大的连续 PSRAM | S |
| （可选）设备网页的 SD 上传页：在浏览器里转码打包 OBV1，分块上传 | L |

**验收：** 开机 24fps 全屏不撕裂、无明显卡顿；主题 240² 视频达到 30fps；播放期间 OBD 轮询周期劣化不超过 1.5 倍；拔卡后回到海报或 Logo，不死机。

### P6 发布与文档

| 工作项 | 规模 |
|---|---|
| 修 `release.sh`：一版滞后、count 不一致；用 `git add -f` 提交白名单里的 5 个 build 产物和 `firmware/release`；不在 main 上时发出警告 | S |
| `gen_release.py`：在 `latest.json` 里加 `layout` 和 `flash_files`；校验镜像 ≤0x300000、超过 90% 告警；校验 bootmedia 是 raw 格式且 ≤0x4C0000 | S |
| CMake：删除 `spiffs_create_partition_image` 和 `REQUIRES spiffs`；新增 `tools/make_bootmedia_raw.py`，注册成单独的 `bootmedia-flash` 目标 | S |
| 删除 `firmware/release` 下的孤儿文件：`firmware.bin`、根目录的 `bootloader.bin` 和 `partition-table.bin`、`flash_address_map.txt` | S |
| 文档（中文为事实源，同一提交里同步 `.en.md`）：FLASH（9 行分区表；本版本必须用网页 complete 刷写一次；禁止 erase_flash）、APP_PROTOCOL（`/ota/*` 的真实契约、`/api/v1`、sidecar、SD 目录）、VEHICLES（改写成 profile schema 和导入指南）、THEMES（编辑器契约、`vid_` 约定、OBV1）、USER_GUIDE（车型页、存储页、安全模式）、README，CHANGELOG 两份 | M |
| 发布顺序：在 `rebuild` 上完成实车验收 → 合入 `main`（watcher 随即发布网页刷机包）→ NAS 发布 → 用户用网页 complete 刷写一次 → 之后走单固件 OTA | S |

---

## 5. 子系统设计要点

### 5.1 SD 卡与存储

- **硬件（已核对原理图网表）：** `CLK=GPIO14`、`CMD=GPIO17`、`D0=GPIO16`、`D3=EXIO3`（TCA9554）。D1、D2 没接，CD 也没接 GPIO，4 根信号线都有 10K 上拉。GPIO16 本是 XTAL_32K_N，但 RTC 时钟用的是内部 RC，可以当普通 GPIO 用。
- **吞吐（推算，需上板验证）：**
  - 1-bit 40MHz 约 3.7MB/s，20MHz 约 2.1MB/s，SDSPI 20MHz 约 1.67MB/s；
  - 4KB 簇会让吞吐掉一半以上，所以要求 32KB 簇；
  - 2GB 卡按规范是 SDSC，能否跑 40MHz 要实测。
- **SD 上的目录：**
  ```
  /OBD/README.TXT
  /OBD/IMPORT/      用户投放（车型/gaugeproj/主题 bin）、IMPORT.LOG、ACTIVATE.TXT、RESET.TXT
  /OBD/THEMES/<theme_key>/theme.bin、<key>.obv、*.lvi、THEME.TXT
  /OBD/VIDEO/       全局视频库（.obv）
  /OBD/BOOT/        开机视频（boot.obv）和从收件箱导入的旧格式动画
  /OBD/CFG/         config 分区的镜像
  /OBD/CATALOG/     车型目录模板（用户从仓库 vehicles/catalog 拷入）
  /OBD/EXPORT/      设备导出的 profile 和规则
  ```
- **卡的规格与供给：** 卡统一格式化成 FAT16 或 FAT32、32KB 簇。exFAT 不支持（`FF_FS_EXFAT=0`），用户换 64GB 以上 SDXC 卡时要先在电脑上格式化成 FAT32。挂载失败只提示，绝不自动格式化。出厂预装内容（默认开机视频、README、空目录）由 `tools/` 下的脚本生成。
- **掉电安全：** 车辆熄火就断电。当前生效的 profile 和规则只存 flash（config/nvs_app），SD 只当可替换的库和投放区；写 SD 一律先写 `.part`，fsync 后再改名。

### 5.2 视频

| 模式 | 用途 | 渲染方式 | 默认参数 |
|---|---|---|---|
| 全屏独占 | 开机、展厅 | 按块解码直接写进 LVGL 的内部条带缓冲推屏，不用 PSRAM 帧缓冲 | 360²，4:2:2，Q 约 75，24fps，单帧 ≤48KB |
| 主题合成 | 主题里的 `vid_*` 元素 | PSRAM 双帧缓冲 + `video_view`（正确报告覆盖），只刷新视频矩形 | 240² 30fps；全屏约 30fps，叠半透明大图时降档 |

- **「读 1 秒、缓存 1 秒」的实现：** 读取任务把整帧读进 PSRAM 环形缓冲，开播水位约 0.3–0.5 秒，目标是始终缓冲满 1 秒。读速至少是码率的 3 倍（约 3.7 vs 1.2MB/s），所以没必要严格等满 1 秒才开播。
- **CPU：** Q75–85 时解码约 26–32ms/帧（推断）。24fps 占 core1 约 61–76%，30fps 风险较高。
- **PSRAM 预算：** 环形缓冲 0.9–2.4MB；合成模式帧缓冲 518KB；图像缓存无视频 2.5MB、有视频 1.5MB；要给 WiFi 和 BT 留出余量。视频页合计约 4MB，剩约 4MB。

### 5.3 编辑器兼容契约（摘要）

- **`theme.bin` 结构：** `[0,16KB)` 是 UTF-8 manifest，后接 0xFF、没有 NUL；之后依次是资源（`rgb565` 大端 2B/px，或 `rgba8888` 4B/px）、字体（`lv_font_bin`，只含 ASCII 0x20–0x7E）、每页 layout（≤64KB）。只有 `[0, usedBytes)` 有效。
- **要接受的元素：** arc、bar、label、image、pointer，字段集合见契约文档；未知字段忽略；`elements[]` 的顺序就是 z 序。
- **坐标约定：** 360×360，原点在左上角；0° 指向 3 点钟，顺时针为正。
- **gaugeproj 才有的字段：** `label.rotation`、`bar.rotation/direction`、`dataDisplay`、`rules`、`name`，只能通过 sidecar 补回。
- **字体：** 规则默认文本「警告」和数据预设里的中文在设备上会缺字。CJK 字体是否支持待定。

### 5.4 规则语义（逐位照搬 `ruleEngine.ts`）

- **字段与旧写法：** `enabled` 只有严格等于 `false` 才算禁用；`trigger.type` 只认 `timer` 和 `change`，其余按 condition 处理；`combine` 只有 `any` 是 OR。
- **滞后：** 每个条件各自锁存，按方向判断。首次求值等于原始比较；`==`/`!=` 不受带宽影响；H≤0 时不碰锁存。
- **组合：** **所有条件都要求值，不能短路**，否则锁存状态会分叉。
- **debounce：** 是导通延迟。规则不命中时只清 debounce 状态，不清锁存。
- **priority：** 按升序稳定排序，后写的覆盖先写的；每帧从 base 重新计算。
- **timer：** `elapsed>0 && elapsed % max(100,I) < 120`；elapsed 取本页构建后的单调时间，不做 10s 回绕。
- **change：** 每个源单独开 280ms 窗口；源取 `trigger.source ?? when.source ?? obd.rpm`。
- **颜色映射：** arc/bar 同时改 color 和 bgColor；image/pointer 用 recolor，`recolor_opa=191`（对应预览里 0.75 的 tint）；label 改 text_color。
- **text 动作：** 只对静态 label 生效。
- **数值与缺失源：** 全部用 double；缺失或未知的源按 NaN 处理（关系运算和 `==` 为假，`!=` 为真）。
- **限额：** 16 条规则、每条 4 个条件、8 个动作。超出部分截断并报告。

### 5.5 车型 profile 示例（BRZ ZC6，压缩后约 1.8KB）

```json
{"schema":"skygauge.vehicle/1","id":"subaru.zc6.can","rev":1,"name":"ZN/C6 CAN",
 "requires":{"elm":["ATMA"]},"bus":{"proto":6,"hdr":"7E0","atst":10,"gap_ms":1},
 "drivetrain":{"fd":4.1,"tire_m":0.314,"gears":[3.626,2.188,1.541,1.213,1.0,0.767],"tol":0.15},
 "monitor":{"mode":"probe","every_ms":500,"window_ms":120,"stale_ms":15000},
 "signals":[
  {"id":"obd.rpm","rate":"every","src":[{"req":"010C","expr":"(A*256+B)/4"}]},
  {"id":"obd.speed","post":{"zero_le":2,"zero_if_rpm_lt":800},"src":[{"req":"010D","expr":"A"}]},
  {"id":"obd.coolant_temp","src":[{"can":"360","bit":24,"bits":8,"expr":"X-40"},{"req":"0105","expr":"A-40"}]},
  {"id":"obd.oil_temp","valid":[-20,150],"filter":"temp_hyst",
   "src":[{"can":"360","bit":16,"bits":8,"expr":"X-40"},
          {"req":"2101","multiframe":true,"byte_from_end":5,"hold":{"delta":8,"max":30,"gap_ms":3000},"expr":"X-40"}]},
  {"id":"obd.gear","derive":"ratio"}]}
```

`expr` 只用于展示，导入时由工具或设备网页编译成 RPN，设备只执行 RPN。BMW F/G 读挡位用的扩展寻址可以这样写：`{"hdr":"6F1","rx":"618","raw":"18 03 22 DA 2E","expect":"62DA2E"}`。

### 5.6 HTTP 兼容约束（App 不改）

- **端点保持原样：** `/ota/discover`（不鉴权，返回 ssid/password/token）、`/ota/info`（键名不变）、`/ota/status`、`/ota/firmware`、`/ota/theme/prepare`、`/ota/theme`、`/ota/theme/erase`、`/ota/bootmedia/prepare`、`/ota/bootmedia`。
- **请求头和回包原样：** `X-OTA-Token`、`X-OTA-SHA256`、`X-OTA-Size`、`X-Offset`、`X-Last`；回包 `nextOffset`；错误文本 `'ota not active'`。
- **分块：** App 用 256KB 分块，固件必须照常接收。
- **SoftAP：** SSID 前缀 `OBD-Gauge-OTA`，IP `192.168.4.1`。密码默认**保持** `88888888`，因为 App 和网页的说明文字里写的是它；改成每台设备随机密码列为待决问题。
- **新端点：** 一律放在 `/api/v1/*` 下，同源访问、要求带 token，不对外开放 CORS。
- **`/ota/info` 完全冻结：** App 的兼容性检查会对 `device` 里的 9 个字段做严格相等比较，所以这里一个字节都不改；layout、SD 状态等新信息只放在 `GET /api/v1/info`。`bootmedia_format` 永远保持 1。

---

### 5.7 带透明通道的图片

- **格式：** 所有透明图统一转成 LVGL 8.4 原生的 `LV_IMG_CF_RGB565A8`（平面格式）：先是大端 RGB565 色平面（即 `LV_COLOR_16_SWAP` 下的内存序），后接 A8 平面，共 3B/px。它是 8.4 软件渲染器里唯一有直通路径的带 alpha 真彩格式（`lv_draw_sw_img.c:85-95`），透明像素只读 alpha 平面；比交错的 `TRUE_COLOR_ALPHA` 快约 2.2 倍。LVGL 9 有同名、同布局的格式，方便以后迁移。
- **不用的格式：** `ALPHA_8BIT` 不能旋转；`CHROMA_KEYED` 抗锯齿时边缘会被腐蚀；LVGL 8 没有「真彩 + 1bit alpha」。
- **转换时机：** 页面构建时，从 theme_0 的 mmap 里读编辑器的 `rgba8888`，转进 PSRAM。360² 全屏图约 46ms。另外两种方案都否决：
  - SD `.cache`：读回来比现转还慢；
  - 转好写回 flash：破坏收件箱的 CRC 账本，有断电风险，还加倍磨损。
- **转换步骤：**
  1. 校验：size 等于 w·h·bpp，w、h 在 1..2047；
  2. 只转换可见部分（元素框 ∩ 图片）；
  3. 圆屏四角（r>180）当 don't-care。圆内全不透明的直接降成 `TRUE_COLOR`；
  4. 用编辑器同款截断公式量化，不做预乘；
  5. 按 alpha>0 求包围盒裁边，裁完再分类一次；
  6. alpha bleed：避免旋转后出现黑边。
- **性能（推断，需上板验证）：**
  - 换 label 文本：<0.5ms；
  - 指针扫动：6–17ms/帧；
  - 360² 全屏叠加层完整重绘：环形分布 6–9ms，全半透明 36–41ms；
  - 规则 recolor 每次完整重绘多约 29ms，所以要预烘焙。
- **可选优化：** `CONFIG_LV_COLOR_MIX_ROUND_OFS=0` 能让半透明像素快约 1.7 倍，代价是 alpha 精度降到 32 级，需要上板目视后再定。
- **规则交互：**
  - `set.asset`：在同一次锁内切换描述符和位置，重设 pivot；
  - `set.color`：切换到预烘焙变体，或运行时 recolor 并在报告里提示成本；
  - `set.visible`：只设 HIDDEN 标志，不释放缓存。

## 6. 无用代码清单（目标 3，已独立复核）

### 6.1 已确认无用，P0 直接删除

- **仓库层面：**
  - `firmware/obd_brz_gauge.bin`（和 `firmware/release` 里那份重复）；`firmware/release/{firmware.bin, bootloader.bin, partition-table.bin, flash_address_map.txt}`（孤儿文件）；
  - 根目录 `bootmedia.bin`（和 `slot_a` 里那份逐字节相同）；`bootmedia/slot_a/boot.pcm`（固件里没有 I2S 播放代码）；
  - `fix_nvs.py`（拼好了命令但从不执行）；`verify_bootmedia.py`（写死了路径和端口）；
  - `main/python_test.py`；`tools/png_to_lvgl.py`（输入输出文件都不存在）；`tools/__pycache__/`；
  - `theme_store/` 和 `tools/gen_theme_store.py`（没有任何消费方）。
- **构建配置：**
  - `main/export_path/CMakeLists.txt`、`filelist.txt`（SquareLine 导出的 stub，没被引用）；
  - LVGL 的 examples/demos 编译项；用不到的 LVGL 控件开关；
  - `idf_component.yml` 里的 `knob`/`button`。
- **SquareLine 残留：**
  - 早期列出的 `ui_comp_hook.c`、`ui_events.h`、`ScreenPageMain` 和 `ui____initial_actions0` 当前均不存在；
  - 当前 `ui.h` 的图片/字体声明都有使用点；`ScreenPageEasterEgg` 已改作设备信息页，仍参与导航和定时更新；
  - `ui_helpers.h` 里的未使用 `ui_anim_user_data_t` 已删除；当前 `_ui_screen_change` 和项目自定义 helper 均有调用方。
- **不可达的页面：** `ui_ScreenPageOilWarn` 当前源码中已不存在；其退役 NVS 槽位 `rsv_brake_temp_warn_c` 仍保留以兼容旧配置布局。
- **没有调用方的函数和变量：**
  - 早期列出的 OBD 单项 getter 和里程统计链路当前均已不存在；`obd_data_cache` 仅保留 RPM、motion 和完整快照读取；
  - 早期列出的油温诊断/校准、OTA resume、RaceChrono 辅助 API、`ui_disp_item_scale`、`generate_wifi_password`、`ESPNOW_FORCE_SLAVE` 及只写全局变量候选当前均未在源码中定义；
  - `device_identity_get` 被 API v1 handler 使用；TCA9554 的 `EXIO_Init`/`Set_EXIO` 被板级代码使用，均保留；
  - `app_event` 当前的 3 种 ESP-NOW 事件都有实际发送方和 UI 消费方，保留事件队列；此前“6 个没有发送方”的判断已过期。
- **主题引擎：** 旧清单中的 `theme_test.c`、`ui.c:1269` 调用、`THEME_PAGE_TYPE_SYSTEM` 和 `create_fn` 在当前源码中均不存在；全仓检索未发现对应实现或调用点，不再作为待删候选。
- **开机动画：**
  - 旧版 `boot_media_mount` 空壳 API、无调用方的原始分区读写封装和 `boot_block_player` 路径配置/getter 已删除；超大旧动画迁移仍通过 `storage_bootmedia_probe` 探测并使用 `boot_media_invalidate_manifest` 清除旧头部。
- **车型相关：**
  - 早期列出的油温公式、`CH_BOOST`/`CH_INTAKE`、`on_parsed_fuel_level`、`can_num`/`can_den`/`can_off` 和 0x140/0x360/0x0D1 硬编码路径均不在当前固件源码中；
  - `forced_protocol`/`has_boost` 仍由 `tools/vehicles/` 的旧车型导出、转换和 golden 序列工具使用，不能按无用字段删除。
- **ST77916 驱动：** 旧清单提到的 `esp_lcd_st77916.c` 注释初始化表在当前路径/源码中不存在；`ST77916.c` 中三个按硬件配置编译的初始化表仍分别服务 V1、V2_NEW、V3，不是死代码。

### 6.2 被重构取代，在对应阶段完成后删除

- **P3 完成后：**
  - `vehicle_profiles.c` 的 `s_profiles[]`、整个 `vehicle_custom_config.h`；
  - 油温枚举链；12 槽 switch；
  - 按车型名写死的分支（`strncmp("ZN/C6")`）。
  - 合计约 1300 行以上。
- **P4/P5 完成后（已完成，见下）：**
  - `tools/theme_packer/pack_theme.py`：和编辑器的打包器重复，而且字节序相反。已删除——运行时主题统一用编辑器导出的 theme.bin；
  - `tools/convert_rpm_flash.py` 和 `images/imgRpmFlash1-3.c`、`imgBootLogoCustom.c`、`gifSnake400.c`（约 10MB 源码），以及 `ui.h` 里的 `USE_CUSTOM_*`、`USE_GIF_*` 开关。已删除，RPM 自定义闪烁改成主题规则，同时改了 DEVELOPMENT.md；
  - `make_boot_block.py`，已删除，由 `make_video.py` 取代；
  - 主题引擎里只认 `dial`/`ring` 的 `theme_get_asset` 映射和 `dial_background` 自动铺底已从当前源码移除（全仓无对应符号/逻辑）；
  - 5–6 套平行的信号枚举（`CH_*`、`DISP_ITEM_*`、`obd.*` strcmp 链、`obd_snapshot_t`），统一到信号注册表。
- **P2 完成后：** CMake 的 SPIFFS bootmedia 规则；两份重复的 flash 写入实现；两份重复的 SHA 辅助函数。

### 6.3 保留，但要整理

- `main/python_quick_rs485_check.py` 和 `main/SAX0AC BC CC.pdf` 随刹车温度功能一起删除（见 6.4）。
- `model/`（28MB STL）是合法的产品资产，是否挪到 Release 附件或 LFS 由你决定。
- `android_app/app-debug.apk` 已过期，但文档仍把它当下载入口。改成发到 Release，并同步改文档。
- **不能删的：**
  - 黑耳图 `ui_img_pngblackear_png.c` 和 8 个 Typoder 字体，系统页在用，而且没插卡时也必须可用；
  - `ui_theme_generated.c`，是有意提交的生成物，`gen_themes.py --check` 会校验它；
  - `themes/builtin` 和 `gen_themes.py` 这条编译期主题链路，还在用；
  - `tools/fake_elm327.py`、`analyze_proble.py`（顺带把拼写改成 `analyze_probe`）、`one_shot.py`、`parse_carscanner_backup.py`，属于 PID 挖掘工具链，改成输出 profile 草稿。

---

### 6.4 刹车温度整体移除（决定 9）

- **整块删除：**
  - `rs485_brake_temp.c/.h`、Kconfig `OBD_RS485_*`、app_main 里只在 V1 走的启动分支；
  - OTA 路径上 12 处 pause/resume（不含 BLE OTA）；
  - `obd_data_cache` 的刹车字段、setter、getter、状态枚举；`ui_ext.c` 的演示数据；
  - `python_quick_rs485_check.py`、`python_test.py`、本地 PDF 及其 `.gitignore` 规则；
  - 各处中英文档。
- **只占位、永不复用：**

  | 槽位 | 处理 | 为什么不能删 |
  |---|---|---|
  | `DISP_ITEM` 9 号 | 改名 `DISP_ITEM_RESERVED_9`，BOOST/AFR 显式写死为 10/11 | 数值存在 NVS 里，删掉会让 BOOST/AFR 串位 |
  | chartalarm[9] | 保留，默认 OFF | blob 长度不能变 |
  | `nvs_user_cfg_t` 的 offset 60 | 改名 `rsv_brake_temp_warn_c` | 删掉会让 blob 变小，`nvs_get_blob` 返回 INVALID_LENGTH，全部设置丢失 |
  | ESP-NOW v5 offset 23 | 改名 `_legacy`，恒为 -1000，发包前先清零 | 包长 47B 严格相等，删掉会让新旧表互相丢包 |
  | RaceChrono 0xF109 | 留一个恒为无效的退役桩，`RC_CH_MAX` 保持 9 | 避免 index 8 串到 BAT_MV |

- **运行时纠正（和删除放在同一次改动里）：** 开机时把不可选的持久化索引回落到默认值；把存量里 <800 的 AFR 阈值修成 OFF；9 号占位的 div 必须 ≥1，否则 `ui.c` 指针页会除零 panic。
- **附带收益：**
  - 释放 UART0：原来被改成 9600 波特，和控制台冲突；
  - 释放 GPIO12/13；
  - 少一个 core1 上的轮询任务，约省 4KB 内部堆；
  - 标准信号从 13 个变成 12 个。
- **AGENTS.md：** 新增红线「退役槽位永不复用」。

## 7. 现存 bug（重构中顺带修）

| # | 问题 | 位置 |
|---|------|------|
| 1 | 主题快照把 0.1bar 换成 PSI 时多除了 10（5bar 显示成 7PSI），需实车确认 | `ui.c` `ui_build_theme_snapshot` |
| 2 | manifest 是不以 NUL 结尾的 16KB 缓冲，却交给 `cJSON_Parse`，会越界读 | `theme_loader.c:668` |
| 3 | 编辑器的 `rgba8888` 是 4B/px，固件当成 3B/px 的 `TRUE_COLOR_ALPHA` 用，透明图会花屏。改为转换成 `RGB565A8`（见 5.7） | `theme_loader.c:739-741` |
| 4 | （已失效）`pack_theme.py` 按小端写 RGB565，和 `LV_COLOR_16_SWAP` 要求的大端相反——该脚本已随 P4/P5 删除，运行时主题统一用编辑器导出的 theme.bin，字节序由编辑器负责 | 已删除：`pack_theme.py:61` |
| 5 | `theme_json_int` 对负数取整差 1（-120 变成 -119） | `theme_loader.c:898-904` |
| 6 | 主题页左右滑动时旧屏对象不释放（`auto_del=false`） | `ui.c:957,969` |
| 7 | CMake 生成的 bootmedia 是 SPIFFS 镜像，固件读的却是 raw 格式，所以按 release 刷完没有开机视频 | `main/CMakeLists.txt:104` |
| 8 | BLE 上传开机动画会先擦掉整个分区，再去打开一个没挂载的路径，必然失败，旧动画已经没了 | `ota_update_ble.c:550-563` |
| 9 | RPM 闪烁结束后，主题页背景被改成 `UI_COLOR_BG` | `ui_helpers.c:374-382` |
| 10 | `s_chart_alarm` 只初始化了 11 个，`CHART_ALARM_N=12`，AFR 阈值默认为 0；很多设备已经把 0 落盘，读取时要把 <800 的 AFR 阈值修成 OFF；source idx 截断范围不对 | `nvs_storage.c:43-47,150-152` |
| 19 | 新设备首次启动时 `load_blob` 把配置整块清零再保存，覆盖了静态默认值：联动角色变成 MASTER（注释声称默认 STANDALONE），温度/信息页映射全变成 CLT | `nvs_storage.c:306-307` |
| 11 | Mode 22 的解析要求油温回调非空才执行，油压和挡位解析也被它牵连 | `elm327_ble_client.c:2037-2039` |
| 12 | `release.sh` 一版滞后，count 也不一致；`firmware/release` 停在 count 104，已过期 | `tools/release.sh` |
| 13 | 文档和代码不一致：`APP_PROTOCOL.md`（密码、token 获取方式、请求头）、`VEHICLES.md:161`（挡位回退）、DEVELOPMENT.md 里的页面数量、`fix_nvs.py` 的说明 | docs |
| 14 | 主题安装不防掉电：先整片擦除再从 manifest 开始写，而且在写 flash 之前就回复了成功，掉电会留下半写的主题，App 却显示成功 | `ota_wifi_server.c:1158-1170` |
| 15 | 主题资源的 width/height 没有校验，改成加载进 PSRAM 后会越界读 | `theme_loader.c:762-825` |
| 16 | label 的 format 里有两个占位符、而 divisor ≤1 时，调用 `lv_label_set_text_fmt(fmt, value)`，`vsnprintf` 会读一个没传进来的参数（未定义行为） | `theme_loader.c:449-458` |
| 17 | 主题页入口写死了页 id `main_gauge`，编辑器里改页名后主题进不去 | `ui.c:934,1320` |
| 18 | 固件里多处字符串缓冲比编辑器允许的长度短：theme id[32]（编辑器允许 64）、资源名 name[32]、字体名 name[40]、data_source[24]、format[24]，超长会被截断，然后就查不到 | `theme_interface.h:59`、`theme_loader.c:35-73` |

另外，编辑器和固件对负小数都显示成 `0.50`（应为 `-0.50`）。为了让预览和设备保持一致，固件**照编辑器的行为来**，不单独修。

---

## 8. 明确不做的事

- 不改编辑器、网页刷机、App、backend、NAS 发布器。
- 不做 App 侧的跨布局拦截（决定 7）：不改 `bootmedia_format`，也不改 NAS 上的 `hardware.json`。
- 不考虑 V2/V3 的 SD 支持（决定 2）。驱动用 Kconfig `OBD_SD_SLOT` 留好接口，默认为 NONE。
- 不在固件里内嵌车型目录（决定 5）。车型 JSON 放在仓库 `vehicles/catalog/` 里。
- app 槽不扩到 4MB。网页单独刷主题时会写坏 ota_1，有变砖风险。
- 不实现新的 BLE 配置服务：现在没有客户端，实现了就是新的死代码。需要时再出协议规范。
- 不合并 hokori 的分支（落后 main 128 个提交）。只借鉴「按页面用到的信号生成轮询掩码」这个思路，放进新调度器。
- 规则 v1 不做切页、声音、级联。schema 里预留字段，放到 v2.1。
- 不升级 LVGL。这次重构锁定 8.4；格式设计时尽量选 LVGL 9 也有的形式，方便以后迁移。

---

## 9. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 内部 RAM 不够（WiFi 曾经 OOM，规则引擎曾经回滚） | 新模块全部放 PSRAM 动态分配；CI 卡 BSS 增量；SD 中转缓冲共用 16KB；上板测最大连续块 |
| 全量刷写后第一次启动没有 OTA 回滚保护 | 首次迁移流程可重入、各步骤幂等；发版前做实车验收；出问题时网页可以重刷 |
| 用户原来的车型（比如 ZC6）迁移后变成「待导入」，暂时按 generic 运行 | 发版说明里提供 `vehicles/catalog`；SD 上放了对应 JSON 的话，开机自动安装并激活映射过去的 id |
| PSRAM 碎片化：整包上传需要连续 4–6MB，和视频缓冲冲突 | 进入 OTA 模式前释放视频；新上传通道改成流式写 SD |
| esp_new_jpeg 在 360 高度、4:2:0 下的 block 输出没验证过 | 开机视频默认 4:2:2；OBV 头里带 `BLOCK_SAFE` 标志；开机自检；整帧解码回退路径 |
| 新的调度器让 ZC6 的 RPM 刷新率变低 | golden 请求序列测试；实车 A/B 对比 |
| 导入的 profile 向 ECU 发出危险命令 | 服务号和 AT 白名单，设备端永远重新校验 |
| SD 卡在车上震动、高温下接触不良，又没有卡检测脚 | 播放全程只读；连续 2 次 CMD13 失败才判定拔出；激活中的配置只存 flash |
| 网页刷入超大开机动画，覆盖 config | 超级块检测加自动格式化，从 SD 镜像恢复；当前车型另存一份在 nvs_app |
| sidecar 和 `theme.bin` 不同步 | 配对报告；元素 id 指纹比对 |
| app 3MB 槽的余量（估算剩约 518KB） | 每次构建检查镜像大小，超过 90% 告警；Montserrat 字体作为后备，必要时挪到 SD |
| 三块表逐台升级期间，主从 ESP-NOW 版本不一致导致从表断开 | v5/v6 兼容过渡期；主表确认所有从表升级后再切到 v6 |
| TPMS 代码合入后覆盖 BLE 回调，ELM327 断连 | 先做 `ble_hub` 分发器，再接入 TPMS；TPMS 的 NVS 字段并进 v4 迁移，一次规划好 |
| OTA 回滚到旧固件后读不到新配置 | 系统 nvs 的 cfg blob **大小和布局都不变**（往末尾追加也不行，blob 变大旧固件会整份读失败）；新字段放进 `nvs_app` 的独立 key；`vehicle_profile_idx` 持续回写作为降级镜像 |
| 透明图性能：大面积半透明叠加层在视频上方时帧率掉到 10–12fps | `video_view` 修复覆盖判断；按透明度直方图自动降档；导入报告建议缩小叠加层或改用 240² 视频；可选把静态叠加层预合成进视频 |
| 刹车温度删除后，持久化的索引 9 和存量阈值造成错位 | 槽位保留不复用；开机时运行时纠正不可选的索引；和删除放在同一次改动里 |

---

## 10. 上板验证清单

1. 2GB 卡的类型（SDSC/SDHC）、是否支持 40MHz；FATFS 顺序读写的速度矩阵（频率 × 簇大小 × 块大小 × 目标缓冲放内部 RAM 还是 PSRAM）。
2. `esp_new_jpeg` 360² 单帧耗时（Q60/75/85 × 4:2:0/4:2:2 × 整帧/block）；4:2:0 最后半行 MCU 输出是否正确。
3. 用真实素材编码后统计平均和峰值帧大小，确定码率上限和环形缓冲容量。
4. BLE OBD、ESP-NOW、仪表页同时运行时，内部 RAM 的余量和最大连续块；加上 SD 和解码之后再测一次。
5. 全屏 24/30fps 的实际帧率和丢帧率；主题合成模式在 360²/240² 下的帧率；播放期间 OBD 轮询周期的变化。
6. 撕裂情况：开和不开 TE（GPIO18）同步对比。
7. 播放中拔卡、断电各 50 次：重新挂载成功率和文件完整性。
8. 网页 complete 刷写（不擦除）后的首次迁移，用旧布局设备实测，包括旧开机动画超过 4.75MB 的情况。
9. 油压显示的 /10 问题；新旧引擎的 ZC6 RPM 刷新率对比。
10. 开机到第一帧的延迟（目标 ≤1s，从 `app_main` 起算）。
11. 3V3 电源：SD 读、WiFi/ESP-NOW 发射、背光全亮同时发生时，会不会触发 brownout。

---

12. 透明图：PSRAM/flash 读带宽；360² 叠加层在四种透明度分布下的重绘耗时（RGB565A8 / TRUE_COLOR_ALPHA / recolor，ROUND_OFS 取 128 和 0）；`video_view` 修复覆盖判断前后的视频帧率对比。
13. 刹车温度删除后重测一次基线（UART0 不再被改成 9600，ESP_LOG 不再忙等）。

## 11. 待决问题

1. **开机来源策略：** 以最近一次更新为准（推荐），还是严格 SD 优先？
2. 能否取消开机时 1 秒的 Logo 停留，直接显示视频第一帧？要做到首帧 ≤1s 必须这样改。
3. **SoftAP 密码：** 保持 `88888888`（默认），还是改成每台设备随机、在屏幕上显示二维码？后者 App 照样能用，但用户第一次需要在手机上「忘记」之前保存的网络。
4. **config 分区大小：** 1MB（开机动画超过 4.875MB 才会覆盖它），还是 512KB（阈值提高到 5.375MB，代价是 config 以后不能扩容）？
5. **验车参数：** 20 秒窗口、K=3、ATRV ≥13.0V 需要实车标定；是否允许用户关掉自动回滚，只用手动确认？
6. **规则和 label 的中文：** 是否支持 CJK？支持的话，要在 SD 上放字体子集。
7. **数据无效时的显示：** `--` 还是 `0`？
8. **合并时机：** `rebuild` 合入 `main` 就等于发布到网页刷机，什么时候合？
9. 全量刷写后第一次正常启动时，要不要擦掉 ota_1 里残留的旧固件？
10. 要不要做设备网页的 SD 视频上传页（在浏览器里转码）？不做的话，视频只能用读卡器加 `make_video.py` 放进 SD。
11. 编辑器实际部署的是哪个分支？规则系统只在还没合并的 `codex/local-frontend`（5a7484e）上，兼容目标暂时锁定在这个 commit。
12. stash@{0} 里的 TPMS BLE 代码：建议在 P3 完成、`ble_hub` 分发器就绪之后，作为 `tpms.*` 信号源接入，需要你确认。持续扫描会和 ELM327 连接、WiFi 抢射频，要先定扫描的占空比。

---

13. 是否接受 `CONFIG_LV_COLOR_MIX_ROUND_OFS=0`？半透明像素快约 1.7 倍，alpha 精度降到 32 级，需要上板目视。
14. 透明图超出 PSRAM 预算时：跳过顶层装饰图并报告（推荐），还是另做「从 flash 逐行现转」的兜底解码器？
15. 360² 视频上压大面积半透明叠加层时，接受自动降到 10–12fps，还是在导入报告里直接建议改用 240² 视频？

## 12. 实施进度

> 状态说明：✅ 已完成（未提交）　🔄 进行中　⏳ 未开始。按用户约定不跑 `idf.py build`；固件改动用单文件 `-fsyntax-only` 检查加独立复核把关，纯 C 模块用 host 单元测试。

| 阶段 | 工作项 | 状态 |
|---|---|---|
| P0 | TPMS stash 存成分支 `wip/tpms-stash` | ✅ |
| P0 | `build/` 改为白名单跟踪（保留 watcher 需要的 4 个 bin，其余 2261 个产物取消跟踪）；修正 `.gitignore` | ✅ |
| P0 | 删除仓库层面的无用文件（`fix_nvs.py`、`verify_bootmedia.py`、`theme_store/`、`gen_theme_store.py`、`png_to_lvgl.py`、`firmware/release` 孤儿文件、重复的 `bootmedia.bin`、`boot.pcm`、`python_test.py`），同步中英文档 | ✅ |
| P0 | 删除 BLE OTA 0x1FFB / 0x1FFA | ✅ |
| P0 | 删除刹车温度（6.4） | ✅ |
| P0 | 固件死代码清理（6.1，app/bsp/theme 和 UI 两批） | ✅ |
| P0 | 局部 bug 修复（第 7 节 #1、#2、#5、#10、#11、#16、#17、#19） | ✅ |
| P1 | 契约文件、host 测试基建、`gen_layout.py`、`check_bss.py`、CI | ✅ |
| P3/P4 | 纯 C 核心：规则引擎（对 38 组黄金向量）、车型 RPN 执行器与校验器、车型转换器（17 个 JSON） | ✅ |
| P2–P6 | 分区 v2、SD 存储（导入/导出/主题库/开机视频）、ESP-NOW v6、设备网页、OTA、发布 | ✅ |
| 全部 | 7 套 host 单测共 22 项全部通过（rules 5/5、vehicle 8/8、engine 1/1、espnow 1/1、media 1/1、theme 5/5、signal_store 1/1） | ✅ |
| P0 | 上板基线测量、实车验证 | ⏳（需要硬件，由你执行） |

代码/架构层面的重构已经完成：未内置的车型按需从 SD / App / 车型页安装；
主题运行时统一吃编辑器 theme.bin；开机动画走新的 SD/bootmedia 视频格式；
固件尚未在 `idf.py build` 下编译过，也没有上过真实硬件——下一步是上板测试。

## 附录 A：调研中产生的辅助材料

这些材料目前放在本次会话的临时目录里，会话结束后可能丢失。需要的话可以移进仓库，建议位置 `tools/` 和 `contracts/fixtures/`：

- 已校验的分区表 v2 CSV/bin。
- 规则黄金向量：38 组、79 步（`vectors.json`），以及 node 端运行脚本。
- 车型转换器原型（`dump_legacy.c`、`legacy_to_v1.py`）和 17 个车型转换出来的 JSON。
- V1 板原理图（网表已核对 TF 卡接线）。
