# 主题系统

[English](THEMES.en.md) | 简体中文

本项目有**两套并存的主题系统**，服务不同场景：

| | 编译期主题（TOML）| 运行时主题（theme_0 分区）|
|---|---|---|
| 改什么 | 8 个装饰色 + 表框/指针/表盘素材（换皮，不换布局）| 颜色 + 素材 + **自定义表盘页面与布局**（layout.json）|
| 怎么装 | 主题文件夹编译进固件，重新刷固件 | 打包成 4MB `theme.bin`，USB 烧录或 WiFi OTA 推送 |
| 怎么切 | 设置页 THEME 滚轮（重启生效）| 设备加载 theme_0 分区，开机即生效 |
| 适合谁 | 想快速给内置页面换配色 / 换素材 | 想自己定义表盘页面组成与数据绑定 |
| 来源 | [themes/](../themes/) | `themes/example_boost_oil/` + [tools/theme_packer/](../tools/theme_packer/) |

两套系统的页面可以协同：运行时主题声明了页面时，开机跳过内置表盘页（省 40–60 KB RAM），
主题页面进入轮播环（GEAR 页下滑进入）；运行时主题损坏或未刷时自动回退内置页面。

---

## 一、编译期主题（TOML）

**不用写任何 C 代码** —— 一个文件夹、一份清单、一行登记。

```
themes/
├── registry.txt           # slot -> id，只能追加
├── _TEMPLATE/             # 复制这个开始
├── builtin/               # 固件自带（default / amber / ocean）
└── community/             # ← 你的主题放这里
```

### 能改什么

**只换皮，不换布局。** 8 个装饰色角色（必填）+ 3 类美术素材（可选）。
元素位置、字号、页面排布不在范围内（那是 20 多个 `ui_ScreenPageXxx.c` 的事）；
字体刻意排除 —— 换字体等于换字符尺寸，等于换布局。

| 角色 | 用在哪 |
|------|--------|
| `bg` | 屏幕/页面背景 |
| `ring` | 外圈表框 |
| `arc_track` | 仪表弧线底轨、滑块凹槽（"还没走到"的部分）|
| `arc_indicator` | 仪表弧线进度、滑块填充（"已走到"的部分）|
| `text_primary` | 数值、主文字 |
| `text_secondary` | 提示文字、单位、小标签 |
| `needle` | 指针页的指针 |
| `panel` | 面板/列表/滚轮背景 |

### 不能改什么

语义色全局固定（`ui_theme.h`），任何主题都不许动 —— 装在车上的仪表，
红色必须永远意味着"出事了"：

| 宏 | 值 | 含义 |
|----|----|------|
| `UI_SEM_ALERT` | `0xFF4D4D` | 报警/超阈值文字 |
| `UI_SEM_FLASH` | `0xFF0000` | 转速报警全屏闪烁背景 |
| `UI_SEM_ON` | `0x06D6A0` | 开关 ON / 正常状态 |
| `UI_SEM_WARN` | `0xFFD166` | 注意（机油压力等）|

### 制作四步

```bash
# 1. 复制模板
cp -r themes/_TEMPLATE themes/community/sunset

# 2. 填 themes/community/sunset/theme.toml
```

```toml
id   = "sunset"        # 必须和文件夹名一致
name = "SUNSET"        # 设置页显示名，1~10 个大写字符

author      = "your-github-handle"
description = "Warm orange face with a deep purple ring."

[colors]
bg             = 0x140800
ring           = 0x6A2C8A
arc_track      = 0x3A1810
arc_indicator  = 0xFF7A18
text_primary   = 0xFFE0C0
text_secondary = 0x9A6440
needle         = 0xFF3020
panel          = 0x2A1408

[bezel]
style_id = 0

# 可选素材（有什么写什么，没声明的继续用代码画 + 颜色角色）
[assets]
ring   = "assets/ring.png"
needle = "assets/needle.png"
needle_pivot_x = 24      # 指针旋转中心（根部轴心），从左上角算像素
needle_pivot_y = 12
dial   = "assets/dial.png"
```

```bash
# 3. 登记：registry.txt 末尾追加一行（只能追加！）
echo "3  sunset" >> themes/registry.txt

# 4. 编译
idf.py build flash monitor
```

开机进 **设置 → THEME** 滚轮选择，选中后设备**自动重启**生效 —— 页面只在开机创建一次，
重启是最可靠的换肤方式。想快速验证可以先单独跑生成器（错误会指到具体行号）：

```bash
python3 tools/gen_themes.py            # 重新生成
python3 tools/gen_themes.py --check    # 只校验不写文件（适合 CI）
```

### 为什么 registry.txt 只能追加

NVS 存的是**槽位号**（`theme_cfg.theme`），不是主题名。一旦调换顺序，排在后面的主题槽位
集体位移 → **所有已升级的设备被静默换成另一个主题**，而且本地测试发现不了。所以：

- 只能在末尾追加，不能改号、调序、删行
- 槽位 0 必须是 `default`
- 废弃主题时行和文件夹都原样保留；只删文件夹会让构建失败

生成器在构建时强制检查以上规则，违反直接编译失败并提示改法。

### 美术素材规格

| 素材 | 尺寸 | 透明通道 | 编译后大小 | 替换掉什么 |
|------|------|----------|-----------|-----------|
| `ring` | **正好 360×360** | 需要（中间镂空）| 380 KB | 外圈表框 |
| `needle` | ≤ 360×360 | 需要 | 宽×高×3 B | 指针页的指针 |
| `dial` | **正好 360×360** | 忽略 | 253 KB | 页面背景 |

- **指针必须画成朝右（东）**：LVGL 角度 0 = 原样绘制，画朝上会整体偏 90°
- **空间是硬约束**：所有已注册主题的素材都会被链接进固件（`g_ui_themes[]` 全量引用，
  `--gc-sections` 丢不掉）。一整套约 645 KB，总预算 **1536 KB**（`gen_themes.py --asset-budget`），
  超限构建失败并列出每个文件占用。ota_0/ota_1 各 3 MB，固件本身约 2 MB
- 素材转换需要 Pillow（`pip install Pillow`），但**只在 PNG 变动时**才跑 —— 转换出的 C 文件
  入库，生成器靠源图 SHA-256 判断是否需要重转；生成的 `main/export_path/theme_assets/` 不要手改

### 配色建议

- `arc_indicator` 明显亮于 `arc_track`，否则看不出走了多少（同色会被生成器警告）
- `text_secondary` 比 `text_primary` 暗一点，但开车瞄一眼还得看得清
- `bg` 用很深的颜色 —— 亮背景在夜里晃眼

### 分享

主题文件夹放进 [`themes/community/`](../themes/community/)，可附一张同目录
`preview.png`（拍屏幕即可），PR 标题写 `theme: add SUNSET`。

### 框架内部（改框架才需要看）

- **生成流水线**：`registry.txt` + `theme.toml` → `tools/gen_themes.py`（CMake configure 阶段，
  在 `idf_component_register` 之前）→ `main/export_path/ui_theme_generated.c` +
  `theme_assets/theme_*.c`。`registry.txt` 和所有 `theme.toml` 都在 `CMAKE_CONFIGURE_DEPENDS`
  里，新增主题必触发重新生成；生成器只在内容变化时重写输出，避免无谓全量重编
- **主题状态**：`ui_theme.c` 管活动主题与 NVS 持久化；`ui_init()` 在建任何屏幕前调
  `ui_theme_init()`，所以每屏创建时即取到主题色。页面取色用
  `ui_theme_color_lv(role)` / `ui_theme_color(role)`，不要硬编码 `lv_color_hex()`
- **加颜色角色**：`ui_theme.h` 的 `ui_color_role_t` 末尾（`UI_COLOR__COUNT` 前）→
  `gen_themes.py` 的 `COLOR_ROLES`（同序）→ 所有 `theme.toml`（缺角色直接构建失败，不会静默变黑）
- **加素材类型**：`gen_themes.py` 的 `ASSET_KINDS` + `ui_theme_t` 末尾加字段 + 消费方留 NULL 回退
- **预留字段**：`ui_theme_t` 已有 `dial_face` / `font` / `bezel`（现为 NULL），新能力一律
  往结构体末尾追加，指定初始化器自动补零，旧主题不用改
- **格式细节**：`CONFIG_LV_COLOR_16_SWAP=y` 所以 RGB565 按大端 `(hi, lo)` 输出；
  指针不能用 `LV_IMG_CF_ALPHA_8BIT`（旋转绘制路径下颜色未定义），所有素材用
  `TRUE_COLOR_ALPHA` / `TRUE_COLOR`。`theme.toml` 由内置的受限 TOML 解析器读取
  （ESP-IDF 自带 Python 3.8 没有 `tomllib`），支持注释、`[section]`、`key = value`
  （字符串/十进制/0x 十六进制），超集直接报错并指行号

---

## 二、运行时主题（theme_0 分区）

把 UI 表现层从固件里剥离出来：固件提供数据与系统页（设置/OTA/蓝牙），
`theme_0` 分区（4MB @ `0x620000`）提供表盘页面、布局、颜色与素材。

```
┌────────────────────────────────────┐      ┌─────────────────────────────────┐
│ 固件（ota_0/ota_1，各 3MB）          │      │ theme_0 分区（4MB）              │
│ OBD 采集 / BLE+WiFi / NVS / 设置页  │ ───→ │ theme_manifest.json（≤16KB 头） │
│ 主题引擎（加载 + 数据绑定）          │ 读取 │ assets/（dial.png、ring.png）   │
└────────────────────────────────────┘      │ layout.json（页面元素定义）      │
                                            └─────────────────────────────────┘
```

### 受保护的启动页

以下页面**永远不参与主题化**，始终用固件内置实现，主题清单里必须把它们列在
`system_pages`（列到 `theme_pages` 里会被忽略）：

| 页面 ID | 内容 |
|---------|------|
| `logo` | Sky Gauge Logo（品牌一致性）|
| `intro` | RACE AS ONE 开机动画（NVS `intro_enable`：0=OFF 1=RACE 2=VIDEO）|
| `boot_video` | 自定义开机视频（`bootmedia` 分区里的 `boot_block.bin`，由 `boot_block_player` 播放）|

保护原因：开机流程不能依赖可能损坏的主题数据；用户每次开机都应看到同样的启动体验。

### 制作与打包

主题目录结构（参考 [themes/example_boost_oil/](../themes/example_boost_oil/)）：

```
my_theme/
├── theme_manifest.json   # 必需
├── layout.json           # 可选，自定义页面元素与数据绑定
└── assets/
    ├── dial.png          # 可选，360×360 RGB
    └── ring.png          # 可选，360×360 RGBA
```

`theme_manifest.json`（schema 1.0）：

```json
{
  "schema_version": "1.0",
  "theme": {
    "id": "my_theme",
    "name": "MY THEME",
    "version": "1.0.0",
    "author": "your-name"
  },
  "colors": {
    "bg": "0x000000",
    "ring": "0xFF0000"
  },
  "pages": {
    "comment": "logo/intro/boot_video 必须列在 system_pages",
    "system_pages": ["logo", "intro", "boot_video", "settings", "ota", "bluetooth_pair"],
    "theme_pages": ["main_gauge"]
  }
}
```

打包成 4MB 分区镜像（固定 0xFF 填充，头部 16KB 预留清单）：

```bash
python3 tools/theme_packer/pack_theme.py themes/my_theme my_theme.bin
```

### 烧录 / 推送

**USB 直刷**（首次或调试）：

```bash
esptool.py --chip esp32s3 -p PORT write_flash 0x620000 my_theme.bin
```

**WiFi OTA 推送**（设备进 OTA 模式后，App 或 curl 调用，协议见
[APP_PROTOCOL.md](APP_PROTOCOL.md#wifi-ota-http-api)）：

- `POST /ota/theme/prepare` — 预检并返回挂载状态（不擦除）
- `POST /ota/theme` — 分块上传（`X-OTA-SHA256` / `X-OTA-Size` / `X-OTA-Offset` / `X-Last` 头）
- `POST /ota/theme/erase` — 立即擦除整个主题分区，下次开机回退内置默认主题

主题只能走 WiFi OTA 或 USB 烧录，没有 BLE 传输通道。

### 运行行为

- 开机时 `theme_engine_init()` 读取 theme_0：清单损坏 / 分区为空 / 解析失败 →
  自动回退内置默认主题，不会变砖
- 主题声明了页面时：内置表盘页（Gear/RPM/Speed/Temp/Chart 等）不再创建，主题页面进入
  轮播环；数据通过 `theme_update_data()` 周期推送（`obd_snapshot_t`，16 字节 ABI 稳定结构：
  rpm/speed/boost/水温/油压/挡位/电压/油温/AFR/节气门/进气温）
- 素材从 Flash 内存映射（mmap 零拷贝），不占堆
- 切换 / 更新主题需重启生效（与编译期主题一致）
- API 见 [main/theme_engine/theme_interface.h](../main/theme_engine/theme_interface.h)

### 分区表背景

`theme_0`（0x620000，4MB）与 `bootmedia`（0xA20000，5.875MB）见
[partitions.csv](../partitions.csv)。单一主题槽位即可 —— 坏主题自动回退默认，
不需要 A/B 双槽。老分区表（无 theme_0、bootmedia 在旧地址）的设备升级到当前布局
必须**一次性 USB 全量重刷**，OTA 无法改写分区表本身。
