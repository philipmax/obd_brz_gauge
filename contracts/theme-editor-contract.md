# 编辑器兼容契约（固件侧）

本文件规定固件如何解释**不改动的**编辑器（`obd_theme_store`）的输出：`theme.bin`、`<id>.gaugeproj.json`
和其中的规则。原则是「固件兼容编辑器，不反过来」（`REFACTOR_PLAN.md` 第 0 节决定 1、第 1 节结论 4）。

- 兼容目标锁定 `obd_theme_store@5a7484e`（分支 `codex/local-frontend`，规则系统只在这个分支上）。
  同时必须接受该仓库 main 分支的产出：缺 `rotation` 按 0，没有 `rules` 就是没有规则（计划 P4 最后一行）。
- 来源：调研 `delta_editor_compat.json` 的 design A–D 节和 facts（下文【证据】处的文件/行号都指 5a7484e 的编辑器源码或本仓库 4b056c0 的固件源码，路径省略 `obd_theme_store/frontend/src/` 前缀）。
- 信号的名字、编号和交付编码以 [`signals.json`](signals.json) 为准；规则和格式化的黄金向量在 [`fixtures/`](fixtures/README.md)。
- 取整函数全文统一记为 **`R(v) = floor(v + 0.5)`**（与 JS `Math.round` 一致，`R(-120) = -120`，`R(-0.5) = 0`），在 double 下计算。

---

## A. `theme.bin`

### A1 容器

```
[0, 16384)          manifest：UTF-8 JSON（JSON.stringify(…, null, 2)），长度 < 16384，其后为 0xFF 填充，没有 NUL
[16384, …)          images（project.assets 数组顺序，全部打包，不论是否被引用）
                    → fonts（≤ 16 个）→ 每页 layout（紧凑 JSON，每页 ≤ 65536 B，没有 NUL）
[usedBytes, EOF)    不属于主题包
```

- 只有 `[0, usedBytes)` 有效，且编辑器输出长度就是 `usedBytes`；主题在设备上存于 SD，不再受旧 4 MB Flash 分区限制。实际上传受 SD 可用空间与 32 位包偏移限制。固件**不得假设固定包长，也不得扫描找结尾**，一切以 manifest 里的偏移为准。
- manifest 必须按长度解析：`cJSON_ParseWithLength`，或读 16384+1 字节后补 NUL。现行 `theme_loader.c:661-668` 把未 NUL 结尾的缓冲交给 `cJSON_Parse`（内部 `strlen`）会越界读。【证据】`~/.espressif/v5.5.3/esp-idf/components/json/cJSON/cJSON.c:1136`
- **首字节为 0xFF** 表示空槽（网页「清除主题」只写 4 KB 0xFF 到 0x620000）：直接使用内建默认主题，不报错。【证据】`DeviceFlasherPage.tsx:667-677`
- 偏移都是相对 blob 起点的绝对偏移。【证据】`themePacker.ts:266-351`

### A2 manifest 字段

| 字段 | 编辑器输出 | 固件解释 |
|------|-----------|----------|
| `schema_version` | 常量 `"1.0"` | 精确匹配 `"1.0"`，否则用内建默认主题 |
| `theme.id` | `meta.id`，编辑器要求 `^[A-Za-z][A-Za-z0-9_-]{0,63}$`；商店路径不做诊断 | 动态分配或 ≥ 65 B（现为 `id[32]`，会截断）；同时是 sidecar 的配对键 |
| `theme.name` / `version` / `author` / `description` | 任意 UTF-8 | 只用于显示；截断时按 UTF-8 字符边界 |
| `colors.{bg,ring,arc_track,arc_indicator,text_primary,text_secondary,needle,panel}` | `"0xRRGGBB"`（大写） | 只作缺省值：页面背景缺省用 `bg`；元素颜色非法时分别用 `arc_track`/`arc_indicator`/`text_primary` |
| `assets.<name>`（图片） | `{offset,size,format:"rgb565"\|"rgba8888",width,height}` | 见 A3；名字用动态字符串（现 `name[32]` 截断后会查不到） |
| `assets.font_<fontName>_<size>` | `{offset,size,format:"lv_font_bin"}` | `lv_font_load`；名字没有长度上限、可能含 UTF-8，用动态字符串（现 `name[40]`） |
| `pages.theme_pages[]` | `{id,type:"custom_layout",layout_data_offset,layout_data_size}` | 数组顺序即翻页顺序；忽略 `type`；拒绝 id 为 `logo`/`intro`/`boot_video` 的页；没有 `system_pages` |

- 解析时忽略未知键。
- 校验：`offset + size <= blob 长度`；图片 `size == width * height * {2|4}`；`width, height <= 2047`（LVGL 8 图片头只有 11 位，推断编辑器没有校验）；`format` 未知的资产跳过并记诊断。
- 商店主题上传前不跑 `diagnoseTheme`，所以页数、资产数、绑定数、ID 唯一性都要防御：超限截断，重复 ID 取第一个。【证据】`CreateThemePage.tsx:61-79`
- 编辑器侧限额（`themeContract.ts:6-17`）：32 页、每页 32 个绑定、16 条规则、每条 4 个条件 8 个动作、16 个资产、16 个字体；element/page/meta id 为 `^[a-zA-Z][a-zA-Z0-9_-]{0,63}$`，element id 全工程唯一（`themeDiagnostics.ts:65-71, 151-192`）。

固件字符串缓冲必须不短于编辑器允许的长度（现行 `theme_interface.h:59`、`theme_loader.c:35-73` 的 `id[32]`、`name[32]`、`name[40]`、`data_source[24]`、`format[24]` 都偏短，计划第 7 节 bug 18）。

### A3 资产格式

- **`rgb565`**：大端 2 B/px，像素 `((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3)`，`setUint16(…, false)`。这恰好是 `LV_COLOR_16_SWAP=y` 下 `lv_color16_t` 的内存布局，flash 来源 mmap 零拷贝（`LV_IMG_CF_TRUE_COLOR`），SD 来源原样读进 PSRAM。【证据】`themePacker.ts:124-134`；`sdkconfig:2718-2722`
- **`rgba8888`**：只要有一个像素 alpha ≠ 255，整张图就输出 `getImageData` 原样的 RGBA，4 B/px。【证据】`themePacker.ts:116-122, 136-139, 272`
  LVGL 8 在 16 位色深下 `LV_IMG_CF_TRUE_COLOR_ALPHA` 是 **3 B/px**，现行固件直接把 4 B/px 数据标成它（`theme_loader.c:739-741, 822-825`，计划 bug 3）。必须在**加载时**转换，输出写进 PSRAM（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`）：

  ```c
  /* in: RGBA 4 B/px (editor order r,g,b,a); out: LV_IMG_CF_TRUE_COLOR_ALPHA, 3 B/px, LV_COLOR_16_SWAP */
  for (size_t i = 0; i < w * h; i++) {
      uint8_t r = in[4 * i], g = in[4 * i + 1], b = in[4 * i + 2], a = in[4 * i + 3];
      uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);   /* same truncation as themePacker.ts:129 */
      out[3 * i]     = c >> 8;
      out[3 * i + 1] = c & 0xFF;
      out[3 * i + 2] = a;
  }
  ```

  可以在安装时把转换结果缓存到 SD 的 `.cache`，但网页刷机器原样写进 theme_0 的 bin 必须仍能在加载时转换。
- **`lv_font_bin`**：`lv_font_conv --bpp 4 --format bin --range 0x20-0x7E --no-compress`，只含 ASCII；字号 ∈ `{12,14,16,26,32,48,64,80,96}`。【证据】`backend/src/routes/fonts.ts:64-74`；`gaugeModel.ts:66`

### A4 layout 通用规则

layout 是 `{"page_id","background_color","elements":[…]}`（`themePacker.ts:254-260`），**不含 rules**。

- 坐标系：360×360，原点在左上角，`x/y` 是编辑器坐标原样（`toFirmwareCoords` 未被调用）；角度单位为度，0° 指向 3 点钟，顺时针为正。
- 所有像素量用 `R(v)`。修掉现行 `theme_json_int` 的 `(int32_t)(d + 0.5)`（负数差 1，`-120 → -119`，`theme_loader.c:898-904`，计划 bug 5）。编辑器坐标和角度可以是小数和负数（`gaugeModel.ts:391-414`；`FabricCanvas.tsx:381-392`）。
- 颜色接受 `0x` 前缀、`#` 前缀和裸 6 位十六进制，不区分大小写；非法时用 A2 的缺省色。
- `elements[]` 的数组顺序就是 z 序（后面的在上）。未知 `type` 跳过，整页照常构建；未知字段忽略。
- 页面背景 = `layout.background_color`，缺省用 `colors.bg`。删掉 `theme_loader.c:1156` 的 `dial_background` 自动铺底（编辑器从不生成这个名字，是 `pack_theme.py` 的遗留）。

### A5 元素

字段名是 `theme.bin` 里的 snake_case（`themePacker.ts:152-252`）。p 统一定义为 `p = clamp01((v - min) / (max - min))`，其中 `clamp01` 按 JS `Math.max(0, Math.min(1, x))` 的语义：NaN 原样传播（C 的 `fmin/fmax` 会吞掉 NaN，不能直接用）。`max == min` 时 `v > min → p = 1`，`v < min → p = 0`，`v == min → NaN`，按「不画前景」处理。

**arc** `{type,id,x,y,width,height,start_angle,end_angle,rotation:0,data_source,range_min,range_max,color,bg_color,line_width}`

- 编辑器：圆心 `(x + w/2, y + h/2)`，外半径 `w/2`，内半径 `w/2 - line_width`；`sweep = (end - start + 360) % 360`；前景扫过 `sweep * p`；方角端。打包时把 `rotation` 烘进 `start/end` 并输出 `rotation:0`。【证据】`FabricCanvas.tsx:934-1000`；`themePacker.ts:157-180`
- 固件：`lv_arc` 尺寸 `(W, W)`，`W = R(width)`，位置 `(R(x), R(y + (height - width) / 2))`；pad 0；`arc_rounded = false`；MAIN = `bg_color`，INDICATOR = `color`；角度先 `R` 再 `((a % 360) + 360) % 360`。内部 range 固定 `0..10000`，`value = R(10000 * p)`，以支持小数 range，也和 overlay 的 `dataDisplay` 走同一路径。LVGL 起止角相等时不绘制，与编辑器一致（`lv_draw_arc.c:49`）。

**bar** `{type,id,x,y,width,height,data_source,range_min,range_max,color,bg_color,segments?,radius?}`

- 编辑器：背景圆角矩形；前景**永远从左往右**（`direction` 不导出，缺省 horizontal），宽 `w * p`；`segments` 不参与渲染；`radius` 前景背景共用。【证据】`FabricCanvas.tsx:1152-1215`；`themePacker.ts:181-196`
- 固件：用两个 `lv_obj` 矩形（bg、fg，fg 宽 `R(w * p)`，同一 `radius`），**不用 `lv_bar`**（LVGL 8 在 h > w 时自动变竖向，`theme_loader.c:1002-1031`）；忽略 `segments`。

**label** `{type,id,x,y,align,color,font_size, data_source+format(+divisor) | text, font_asset?}`

- 有 `data_source` 时文本 = 格式化器输出（见 A7）；否则 `text`（编辑器 `text` 为空时预览显示 `LABEL`，bin 里写 `""`）。【证据】`FabricCanvas.tsx:1217-1251`；`themePacker.ts:197-220`
- 定位：`align` = `left` 时左边 = x，`center` 时水平中心 = x，`right` 时右边 = x；顶边 = y。文本每次变化（数据刷新、规则改 text）后重新对齐。
- 字体：`font_asset` 能加载就用；否则内建 Montserrat，`12/14/16/26/32/48` 精确匹配；`64/80/96` 回退到 48，再加 `transform_zoom = size * 256 / 48`（推断 LVGL 8.4 可用，需验证），或从 SD 读字体（可选）。现行内建字体只到 48（`theme_loader.c:1052-1057`）。

**image** `{type,id,x,y,width,height,rotation,asset,video_playback?,data_source?,range_min?,range_max?,video_step?}`

- 编辑器：`width/height` 是 Fabric 的**裁剪框**，按原始尺寸绘制，显示 `min(w, natW) × min(h, natH)`；`originX/Y = left/top`，即绕**左上角**旋转。【证据】`fabric 7.4.0 dist/index.mjs:19654-19655`；`FabricCanvas.tsx:1266-1285`
- 固件：不缩放；对象尺寸 `(min(R(w), natW), min(R(h), natH))`（也避免 LVGL 在对象大于图片时平铺，`lv_img.c:152-153, 657-670`）；`lv_img_set_pivot(0, 0)`；`angle = R(rotation * 10)`（0.1° 单位，double 算完再取整，不先取整到度，`theme_loader.c:1125-1130`）；位置 `(R(x), R(y))`。

- 视频图片（`vid_` / `video_` 前缀）可声明 `video_playback: "value"`，绑定 `data_source` 与递增的 `range_min/range_max`；`video_step` 为非负原始单位步长，缺省 `0`。先 clamp，正步长按起点对齐取最近步进值（终点不变），归一化为 `p`，选择 `floor(p * (frame_count - 1) + 0.5)` 帧。无效数据保留上一位置，首次为首帧；未声明模式则循环。编辑器和固件扫表均支持此绑定，详细说明见 `docs/THEMES.md`。

**pointer** `{type,id,x,y,width,height,asset,data_source,range_min,range_max,angle_min,angle_max,rotation,pivot_x,pivot_y}`（现行 rebuild 固件跳过，`theme_loader.c:1229-1239`，**新增**）

- 编辑器：图片非等比缩放到 `width × height`（`scaleX = w/natW`，`scaleY = h/natH`）；枢轴 `(x + w*pivot_x/100, y + h*pivot_y/100)`；`deg = angle_min + (clamp(v, min, max) - min) * (angle_max - angle_min) / (max - min)`（`max - min <= 0` 时取 `angle_min`），再加 `rotation`。【证据】`FabricCanvas.tsx:1287-1316`；`pointerGeometry.ts:24-55`
- 固件：建页时把位图重采样到 `R(w) × R(h)` 放 PSRAM（LVGL 8 的 zoom 只能等比）；`pivot = (R(w * pivot_x / 100), R(h * pivot_y / 100))`；位置 `(R(x), R(y))`；`lv_img_set_angle(R(deg * 10))`。

### A6 只在 gaugeproj 里有的字段

`label.rotation`、`bar.rotation`、`bar.direction`、`dataDisplay`（`scale/offset/inputMin/inputMax/displayMin/displayMax/valueMap/precision/unit`；选数据源时编辑器默认写入 `unit`，预览就会显示「45 bar」）、`rules`、`name`。这些只能通过 sidecar 的 overlay 补回（C 节、计划 P4「overlay（必做）」）。`arc.cornerRadius` 预览本身也不渲染，忽略。只装 bin 的设备（App、商店、网页刷机）拿不到 overlay，这是固有限制，要在配对报告和文档里写明。

overlay 语义（`FabricCanvas.tsx:917-933, 1217-1251`）：

```
transformDataValue(el, raw):
    v = raw * (dataDisplay.scale ?? 1) + (dataDisplay.offset ?? 0)
    if inputMin, inputMax, displayMin, displayMax 都有值 且 inputMax != inputMin:
        v = displayMin + (v - inputMin) / (inputMax - inputMin) * (displayMax - displayMin)
range(el) = [dataDisplay.displayMin ?? range_min, dataDisplay.displayMax ?? range_max]   // arc/bar/pointer

label 文本（有 data_source 时）:
    v = transformDataValue(el, raw)
    if valueMap[String(trunc(v))] 存在:  text = 该映射
    elif precision 有值:                  text = toFixed(v, precision)      // JS Number.prototype.toFixed
    else:                                 text = 格式化器(format, divisor, v)  // A7，v 可能是小数
    if unit 非空: text += " " + unit
```

`toFixed` 按 double 的**精确二进制值**取最近的 n，并列时取较大的 n（对负数先取绝对值，等效于远离零），与 C `printf("%.*f")` 的舍入不同（`0.125.toFixed(2) = "0.13"`，glibc 给 `"0.12"`），必须自行实现。overlay 的黄金向量在 P4 补充。

### A7 label 格式化器（逐位移植，不能用 `vsnprintf`）

现行固件在 format 有两个转换而 divisor ≤ 1 时调 `lv_label_set_text_fmt(fmt, value)`，`vsnprintf` 读一个没传进来的参数（未定义行为，`theme_loader.c:449-458`，计划 bug 16）。新实现移植 `themePacker.ts:34-101` 和 `FabricCanvas.tsx:1224-1233`：

```
fmt = (format 非空 且 isIntOnlyFormat(format)) ? format : "%d"
if divisor && divisor > 1:  args = [trunc(v / divisor), |v % divisor|]     // JS % 取被除数的符号
else:                       args = [trunc(v)]
text = formatIntPrintf(fmt, args)
```

- `isIntOnlyFormat`：每个 `%` 之后若是 `%` 则跳过；否则跳过 `[0-9.\-+ ]*`，到串尾返回 false，下一个字符不在 `diuxXoc` 中返回 false。
- `formatIntPrintf`：逐字符扫描。`%%` 输出 `%`；否则依次读 flags `[-+0 ]*`、宽度 `[0-9]*`、**下一个字符无条件作为转换符**（即使它是 `.` 或已到串尾），并消耗一个参数（缺参按 0）。
- 单个转换：`x`/`X`/`o` 对负数取 `value >>> 0`（32 位补码）后按 16/16 大写/8 进制输出；`c` 输出 `String.fromCharCode(value)`（取 `value mod 65536`）；其余一切（`d i u` 和任何未知字符）输出 `trunc(value)` 的十进制，`%u` 遇负数仍带负号。符号：本体以 `-` 开头则符号为 `-`；否则仅当 flags 含 `+` 且转换符是 `d`/`i` 时为 `+`。填充：`padLen = max(0, width - 符号长 - 数字长)`；flags 含 `-` → 左对齐补空格；否则含 `0` → 符号后补 0；否则左侧补空格。flags 里的空格被忽略。
- 实测编辑器行为（已入黄金向量）：`'%u'(-5) → "-5"`、`'% d'(5) → "5"`、`'%d %d'(7) → "7 0"`、`'%s'(3) → "3"`（被替换成 `%d`）、`'%.2d'(5) → "52d"`、`'%d.%02d'/100 (-50) → "0.50"`（负小数丢符号，计划第 7 节末：固件照编辑器的行为来，不单独修）。

向量：[`fixtures/format/label_format.json`](fixtures/format/label_format.json)（24 条）。

---

## B. gaugeproj 与 sidecar（规则怎样进设备）

### B1 载体

`theme.bin` 不含规则；商店主题永远不带规则；localStorage 草稿（键 `sky-gauge:theme-workbench:draft:v1`）在编辑器源站，设备页跨源读不到。**唯一载体**是「保存工程」下载的 `${meta.id}.gaugeproj.json`，它和 `${meta.id}.bin` 天然同名成对。保存工程不跑诊断。【证据】`GaugeEditorFabric.tsx:389-404, 406-428, 468, 621`；`lib/themeDraft.ts:3, 27-29`

gaugeproj = `JSON.stringify(GaugeProject, null, 2)`（`gaugeModel.ts:69-158, 249-285`），字段全部 camelCase：

```
GaugeProject {
  schemaVersion: "1.0",
  meta:   {id, name, version, author, description?},
  colors: {bg, ring, arc_track, arc_indicator, text_primary, text_secondary, needle, panel},   // "#RRGGBB"
  pages:  [{id, backgroundColor, elements: GaugeElement[], rules?: GaugeRule[]}],
  assets: [{name, fileName, width, height, format: "rgb565"|"rgba8888", dataUrl}],            // base64 PNG/JPEG，默认指针是 URL 编码的 SVG
  fonts:  [{name, fileName, dataUrl, compiled: {<size>: base64 lv_font_bin}}]
}
GaugeElement 公共字段 {id, name?, x, y, locked?, dataDisplay?, kind: "arc"|"bar"|"label"|"image"|"pointer"}
  arc     {width, height, startAngle, endAngle, rotation, dataSource, rangeMin, rangeMax, color, bgColor, lineWidth, cornerRadius?}
  bar     {width, height, rotation, direction?, dataSource, rangeMin, rangeMax, color, bgColor, segments?, cornerRadius?}
  label   {align, color, fontSize, fontName?, dataSource?, format?, divisor?, text?, rotation}
  image   {width, height, rotation, assetName, videoBinding?: {dataSource, rangeMin, rangeMax, step?}}
  pointer {width, height, rotation, assetName, dataSource, rangeMin, rangeMax, angleMin, angleMax, pivotX, pivotY}
```

否决「设备直接把 gaugeproj 当完整源」：需 base64 解码数 MB、PNG/JPEG 解码、SVG 光栅化，像素与预览不一致（工作量 XL）。资产一律用编辑器自己栅格化的 bin（像素同源）。

### B2 sidecar 格式 `gauge-sidecar/1`

从 gaugeproj 只抽 `pages[].id`、`pages[].elements[]`（id、kind、overlay 字段）、`pages[].rules`：

```json
{"format":"gauge-sidecar/1","theme_id":"my_theme","project_version":"1.0.0",
 "source":{"kind":"gaugeproj","size":1834567,"sha256":"..."},
 "pages":[{"id":"main_gauge","element_ids":["arc_x_1","ptr_x_2"],
   "elements":{"arc_x_1":"arc","ptr_x_2":"pointer","label_x_3":"label"},
   "overlay":{"label_x_3":{"kind":"label","rotation":0,"dataDisplay":{"unit":"RPM"}}},
   "rules":[]}]}
```

- `rules` 是 `GaugeRule` **原样 camelCase**，不做任何改写；`elements` 给出每个元素的 kind（配对第 3 步和颜色映射要用；调研 B2 正文要求抽取 id 和 kind，B5 示例只列了 `element_ids`，这里补成显式字段）。
- `theme_id` = gaugeproj `meta.id`，`project_version` = `meta.version`。
- 存储：config(LittleFS) `/themes/<id>/sidecar.json` 并镜像到 SD（计划 P4）；单文件 ≤ 64 KB。
- 交付通道：主 = 设备网页（浏览器解析 gaugeproj，只上传几 KB）；次 = SD 投放 `<id>.bin` + `<id>.gaugeproj.json`（设备端流式过滤器跳过顶层 `assets`/`fonts` 的值和任何 > 4 KB 的字符串）。

### B3 配对算法

1. `theme_id` 相等（只是必要条件，编辑器默认 id 都是 `my_theme`）；
2. 页 id 必须存在于已安装的 bin，否则丢弃该页规则；
3. `action.target` 必须存在于该页 layout 且 kind 一致，否则丢弃该动作（与预览等价）；`asset` 动作要求资产存在；
4. 比对每页 `element_ids` 指纹，不一致时仍然应用，但在设备页和诊断里报告「工程与主题不同步」；
5. 安装新 bin 后重新配对；erase 时删除 sidecar。

---

## C. 规则

### C1 输入 schema（`gaugeModel.ts:200-247`）

```
GaugeRule {
  id: string, name?: string, enabled?: boolean, priority?: number = 0, debounceMs?: number = 0,
  trigger?: {type: "condition"|"change"|"timer", source?: DataSource, intervalMs?: number = 1000},
  when: {
    combine?: "all"|"any",
    conditions?: [{source, op: ">"|">="|"<"|"<="|"=="|"!=", value: number, compareSource?}],
    source?, op?, value?, compareSource?,          // 旧单条件写法
    hysteresis?: number = 0
  },
  apply: [{target: elementId, set: {color?: string, text?: string, visible?: boolean, asset?: string}}]
}
```

- `enabled` 只有**严格等于 `false`** 才算禁用。
- `trigger.type` 只认 `timer` 和 `change`，其他值（含缺省、`condition`、未知串）都按条件规则处理；`combine` 只有 `"any"` 是 OR，其他都是 AND。
- 条件列表：`conditions` 非空就用它；否则 `when.source`、`when.op` 都真值且 `when.value !== undefined` 时用旧字段组成一个条件；都不满足则规则永不命中。【证据】`ruleEngine.ts:81-87`
- 缺失或未知的 `source`/`compareSource` 取 NaN；未知 `op` 结果为 false。
- 一个 `set` 里有多个键时，按 `color → text → visible → asset` 的顺序全部应用。
- 固件不做 `EventRulesPanel` 那套「打开时迁移」（旧格式迁移、`hysteresis=100`），按原值求值（`EventRulesPanel.tsx:44-51, 77-95`）。编辑器颜色来自 `input[type=color]`，是小写 `#rrggbb`。
- 限额 16 条规则/页、每条 4 个条件、8 个动作：取前 N 项，多余丢弃并报告。
- 数值全部用 double（阈值可以是小数，向量 E20）。

### C2 求值语义（逐位照搬 `ruleEngine.ts`）

【证据】`ruleEngine.ts:16-79`（比较与滞后）、`89-111`（单条规则）、`113-162`（整页）

```c
/* per-condition latch: -1 = never evaluated, 0/1 = latched state; keyed by (rule, condition index) */
static bool cond_state(const rcond_t *k, const double *vals, int8_t *latch, double H)
{
    double c = VAL(k->src);                                   /* missing/unknown source -> NaN */
    double t = k->has_cmp ? VAL(k->cmp) : k->value;
    bool raw = raw_cmp(k->op, c, t);                          /* IEEE NaN semantics == JS semantics */
    if (H <= 0) return raw;                                   /* latch untouched (also H NaN? see below) */
    if (*latch < 0) { *latch = raw; return raw; }             /* first evaluation = raw comparison */
    bool dir = IS_REL(k->op), up = (k->op == GT || k->op == GE);
    if (*latch == 1) *latch = !(dir ? (up ? c < t - H : c > t + H) : !raw);
    else             *latch =  (dir ? (up ? c > t + H : c < t - H) :  raw);
    return *latch == 1;
}

static bool rule_active(const rrule_t *r, ...)
{
    if (r->disabled) return false;                            /* enabled === false */
    if (r->trig == TRIG_TIMER)  return elapsed > 0 && fmod(elapsed, fmax(100, r->interval)) < 120;
    if (r->trig == TRIG_CHANGE) return changed_has(r->change_src);
    if (r->ncond == 0) return false;
    bool acc = !r->any;
    for (int k = 0; k < r->ncond; k++) {                      /* evaluate ALL conditions: no short-circuit */
        bool s = cond_state(&r->cond[k], vals, &rt->latch[k], r->hyst);
        acc = r->any ? (acc || s) : (acc && s);
    }
    return acc;
}
```

- **滞后**：每个条件各自锁存、按方向判断。首次求值等于原始比较；`==`/`!=` 不受带宽影响（锁存跟随原始结果）；`H <= 0` 时完全不碰锁存。`hysteresis` 取 `when.hysteresis ?? 0`；`H` 为 NaN 时 `H <= 0` 为假，会进入锁存分支，按 IEEE 比较照搬即可。
- **不短路**：必须求值全部条件，否则锁存状态分叉（向量 E07）。
- **debounce（导通延迟）**：规则不命中时**只清 debounce 状态**（`rt.delete(rule.id)`），锁存保留；首次命中记 `first = elapsed`；`elapsed - first >= max(0, debounceMs)` 才算 active。
- **priority**：active 规则按 `priority ?? 0` 升序**稳定**排序（并列按数组下标），然后依次合并，后者覆盖前者；每帧从 base 重新计算。
- **timer**：`interval = max(100, intervalMs ?? 1000)`，`elapsed > 0 && elapsed % interval < 120`（`interval <= 120` 时恒真，向量 E14）。timer/change 规则忽略 conditions，但仍受 `enabled` 约束。
- **change**：源 = `trigger.source ?? when.source ?? "obd.rpm"`。
- **合并**（`ruleEngine.ts:138-158`）：`color` 打在 image/pointer 上记为 `tintColor`；打在 arc/bar 上只设进度/填充 `color`，底轨保留元素的 `bgColor`（即时变色和颜色动画相同）；其他（含 label、未知 target）只设 `color`。`text`/`visible`/`asset` 直接覆盖。同一规则内后一个动作覆盖前一个（向量 E25）。

### C3 覆盖结果 → LVGL

每 tick 从 base（建页时从 layout 记录）重新计算期望状态，与已应用状态比对，只对变化的属性调用 setter；规则失效后回到 base。

| set | arc | bar | label | image / pointer |
|-----|-----|-----|-------|-----------------|
| color | 只将 INDICATOR 的 `arc_color` 设为 c，MAIN 保留 base | 只将 fg 矩形设为 c，bg 保留 base | `text_color` | `img_recolor = c`，`recolor_opa = 191`（预览 tint 为 `0.75*tint + 0.25*src`，alpha 不变，`fabric dist/index.mjs:20627-20688`、`FabricCanvas.tsx:1254-1264`） |
| text | 忽略 | 忽略 | 只对静态 label（无 `data_source`）生效，改完重新对齐；绑定数据的 label 忽略（预览优先显示数据） | 忽略 |
| visible | 最终合并值 `=== false` 时加 `LV_OBJ_FLAG_HIDDEN` | 同左 | 同左 | 同左 |
| asset | 忽略 | 忽略 | 忽略 | 换 src；pointer 的新图在建页时就预缩放好 |

系统报警（RPM 闪烁、阈值变红）优先于主题规则（计划 P4「运行时接入」）。

### C4 预览帧伪影在固件里的定义

- `elapsed`：本页构建完成后的单调时间（ms），**不做 10 s 回绕**（预览时间线 10 s 循环、100 ms tick，只在播放时前进，`GaugeEditorFabric.tsx:194-197, 207-226`）。
- timer：与预览相同的公式；求值周期 ≤ 50 ms，保证每个 120 ms 窗口都能采到。
- change：每个源单独开 **280 ms** 窗口（预览是「集合在最后一次变化后 280 ms 清空、被下一次更新整体替换」，属可接受差异）；建页时用当前值初始化，无效值不触发 change。
- debounce：真实时间（预览只在播放时计时，本身就与设备不同）。
- 建页或重载主题时清空全部 runtime（锁存、debounce、change 窗口）。
- 求值时机：先更新数据绑定，再在**同一个快照**上求值规则。
- 不可避免的差异：字体（Arial 对 Montserrat）、CJK 和 `°` 缺字（规则默认文本「警告」会缺字）、文本竖向偏移几个像素、arc 角度按 1° 量化、tint 191/255、image 裁剪叠加旋转的组合、设备特有的「数据无效」状态。

### C5 内存

规则表、runtime、覆盖表一律经可注入的分配器放 PSRAM（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`），按页分配、页面销毁时释放，**内部 BSS 增量为 0**（计划 P4；规则引擎曾因 +16 KB 静态 BSS 被回滚，CI 用 `tools/check_bss.py` 卡 256 B）。16 条规则估计 < 5 KB。

---

## D. 数据源

11 个 `obd.*` 键的名字、顺序和交付编码由编辑器冻结（`gaugeModel.ts:17-29`、`themeContract.ts:41-53`），固件按下表交付；完整定义（数字 id、内部定点、范围、旧映射）见 [`signals.json`](signals.json)。

| key | 交付编码 | 相对现行固件的修正 |
|-----|---------|-------------------|
| `obd.rpm` | rpm 整数 | — |
| `obd.speed` | km/h 整数 | — |
| `obd.boost` | 0.01 bar 表压，有符号 | 内部即 0.01 bar（= kPa），可用 (MAP − baro) kPa 得到真实精度（现行只有 0.1 bar） |
| `obd.coolant_temp` | °C，有符号 | 不再把负值截成 0 |
| `obd.oil_pressure` | **PSI 整数** = `R(bar × 14.5038)` | 修正 `ui_build_theme_snapshot` 多除的 10（5 bar 显示成 7 PSI，`ui.c:480-484`；需实车确认）。`gaugeModel` 标签写「油压 (Bar)」只是文本，`themeContract.ts:46` 的 unit 是 PSI |
| `obd.gear` | -1 = R，0 = N，1..8 | 未知时**无效**，不再交付 127（否则 `gear >= 5` 会在未知时误触发） |
| `obd.battery_voltage` | 0.1 V 整数（`floor(mV / 100)`） | — |
| `obd.oil_temp` | °C，有符号 | 不再截成 0 |
| `obd.afr` | AFR × 100 | — |
| `obd.throttle` | 0..100 % | — |
| `obd.intake_temp` | °C，有符号 | 不再截成 0 |

- 无效数据：每个键带 valid 位和时间戳。无效时规则按 NaN 处理（关系运算和 `==` 为假、`!=` 为真），change 不触发；arc/bar/pointer 停在 `range_min`；label 显示待定（计划第 11 节待决问题 7：`--` 还是 `0`）。
- 规则或绑定里出现的未知键（包括编辑器之外的 `obd.load`、`sys.*` 在当前固件不提供时）一律按 NaN；`veh.*` 由当前车型 profile 提供，没有就无效。
- 从表（ESP-NOW v5）收到的数据里 `obd.gear` 永远无效。

---

## E. 黄金向量

| 文件 | 内容 | 验证 |
|------|------|------|
| [`fixtures/rules/vectors.json`](fixtures/rules/vectors.json) | 38 组 / 79 步：T01–T13 对应原 `ruleEngine.test.ts`，E01–E25 为边界用例 | node 端：`tools/fixtures_node/run_ts_crosscheck.mjs` 直接跑编辑器 `ruleEngine.ts`；C 端：`test/host/rules` |
| [`fixtures/format/label_format.json`](fixtures/format/label_format.json) | 24 条 label 格式化 | 同上，跑编辑器 `themePacker.ts` 的 `formatIntPrintf` |

格式说明见 [`fixtures/README.md`](fixtures/README.md)。
