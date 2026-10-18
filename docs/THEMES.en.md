# Theme System

English | [简体中文](THEMES.md)

This project has **two coexisting theme systems** for different scenarios:

| | Compiled-in theme (TOML) | Runtime theme (SD card) |
|---|---|---|
| What changes | 8 decorative colors + bezel/needle/dial artwork (reskin, no layout) | Colors + artwork + **custom gauge pages and layout** (layout.json) |
| How it installs | The theme folder compiles into the firmware; reflash | Export a theme ZIP and install to SD over web USB; App WiFi OTA remains compatible |
| How it switches | Fixed by the firmware build; not selectable on the gauge | Install and select through the web UI or App; read from SD after reboot |
| Audience | Reskin built-in pages | Custom page layout and data bindings |
| Source | [themes/](../themes/) | Theme editor (`obd_theme_store`), `<id>.zip` (containing `theme.bin`, rules, and video resources); [contract](../contracts/theme-editor-contract.md) |
| Install paths | Developer rebuild | One-click web USB install, or App OTA (firmware manages the SD library) |
| Takes effect | Reboot | USB install and activation, APPLY, and OTA install automatically reboot |

The systems coexist: when the runtime theme declares pages, boot skips built-in gauge pages
(saving 40–60 KB RAM), and theme pages enter the navigation ring (swipe down from GEAR;
see [USER_GUIDE.en.md](USER_GUIDE.en.md#page-navigation)). A theme without pages only supplies
color defaults for its own elements; built-in pages keep their compiled palette.
No selection, missing card/file, corrupt manifest or parse failure falls back to the built-in
default theme while preserving the saved selection.

Web theme installation and “Delete current theme” both use ESP32-S3 native USB, requiring no device address, token or WiFi OTA. Deletion releases theme pages and resources, removes the current `/OBD/THEMES/<key>/` directory (including `theme.bin`, rules, images, fonts and video) and saved selection, then the web client separately requests reboot to restore the built-in theme. Other themes and the shared boot animation remain. Update to USB-management firmware first; missing SD, busy storage and deletion failures report errors.

---

## 1. Compiled-in themes (TOML)

**No C code required** — one folder, one manifest, one registry line.

```
themes/
├── registry.txt           # slot -> id, append only
├── _TEMPLATE/             # copy this to start
├── builtin/               # shipped with the firmware (default / amber / ocean)
└── community/             # ← your theme goes here
```

### What a theme controls

**Skin only, not layout.** Eight decorative color roles (required) plus three
kinds of artwork (optional). Element positions, font sizes and page
composition are out of scope (they belong to the ~20 `ui_ScreenPageXxx.c`
files); fonts are deliberately excluded — swapping a font changes glyph
metrics, which changes layout.

| Role | Used for |
|------|----------|
| `bg` | Screen / page background |
| `ring` | Outer bezel ring |
| `arc_track` | Gauge arc track, slider groove (the "not reached" part) |
| `arc_indicator` | Gauge arc progress, slider fill (the "reached" part) |
| `text_primary` | Values, main text |
| `text_secondary` | Hints, units, small labels |
| `needle` | Needle-page needle |
| `panel` | Panel / list / roller background |

### What a theme must NOT control

Semantic colors are global and off-limits (`ui_theme.h`) — this is a gauge
bolted into a car, and red must always mean "something is wrong":

| Macro | Value | Meaning |
|-------|-------|---------|
| `UI_SEM_FLASH` | `0xFF0000` | Full-screen RPM warning flash |

### Build one in four steps

```bash
# 1. copy the template
cp -r themes/_TEMPLATE themes/community/sunset

# 2. fill in themes/community/sunset/theme.toml
```

```toml
id   = "sunset"        # must match the folder name
name = "SUNSET"        # Settings roller label, 1-10 uppercase chars

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

# Optional artwork (declare what you have; anything omitted stays drawn + colored)
[assets]
ring   = "assets/ring.png"
needle = "assets/needle.png"
needle_pivot_x = 24      # rotation center (the needle hub), pixels from top-left
needle_pivot_y = 12
dial   = "assets/dial.png"
```

```bash
# 3. register: append one line to registry.txt (append only!)
echo "3  sunset" >> themes/registry.txt

# 4. build
idf.py build flash monitor
```

Compile-time themes ship with the firmware; users cannot switch them on the gauge.
Developers can validate a theme by running the generator standalone (errors point
at the exact file and line):

```bash
python3 tools/gen_themes.py            # regenerate
python3 tools/gen_themes.py --check    # validate only, writes nothing (CI)
```

### Why registry.txt is append-only

NVS stores a **slot number** (`theme_cfg.theme`), not a theme name. Reorder
the file and every later theme shifts a slot → **every upgraded device
silently switches to a different theme**, and local testing never catches it. So:

- Append only — never renumber, reorder or delete a line
- Slot 0 must stay `default`
- To retire a theme, keep both its line and folder; deleting just the folder
  fails the build

The generator enforces all of this at build time and tells you how to fix it.

### Artwork specs

| Asset | Size | Alpha | Compiled size | Replaces |
|-------|------|-------|---------------|----------|
| `ring` | **exactly 360×360** | required (transparent center) | 380 KB | Outer bezel ring |
| `needle` | ≤ 360×360 | required | w×h×3 B | Needle-page needle |
| `dial` | **exactly 360×360** | ignored | 253 KB | Page background |

- **Needle art must point RIGHT (east)**: LVGL angle 0 draws unrotated, so
  art pointing up ends up 90° off
- **Space is a hard constraint**: every registered theme's artwork is linked
  in (`g_ui_themes[]` references them all, so `--gc-sections` cannot drop
  them). A full set is ~645 KB against a total budget of **1536 KB**
  (`gen_themes.py --asset-budget`); exceeding it fails the build with a
  per-file breakdown. ota_0/ota_1 are 3 MB each and the firmware is ~2 MB
- Converting artwork needs Pillow (`pip install Pillow`), but **only when a
  PNG changes** — the converted C files are checked in and the generator
  decides via the source SHA-256; never hand-edit
  `main/export_path/theme_assets/`

### Palette guidance

- Keep `arc_indicator` clearly brighter than `arc_track`, or progress becomes
  unreadable (the generator warns on identical values)
- `text_secondary` dimmer than `text_primary` but still legible at a glance
  while driving
- Keep `bg` very dark — bright backgrounds are harsh at night

### Sharing

Drop the folder into [`themes/community/`](../themes/community/), optionally
with a `preview.png` beside the manifest (a photo of the screen is fine);
open a PR titled `theme: add SUNSET`.

### Framework internals (only for framework changes)

- **Codegen pipeline**: `registry.txt` + `theme.toml` → `tools/gen_themes.py`
  (at CMake configure time, before `idf_component_register`) →
  `main/export_path/ui_theme_generated.c` + `theme_assets/theme_*.c`.
  `registry.txt` and every `theme.toml` are listed in
  `CMAKE_CONFIGURE_DEPENDS`, so adding a theme always triggers regeneration;
  output is rewritten only when content changes
- **Theme state**: `ui_theme.c` owns the active theme and NVS persistence;
  `ui_init()` calls `ui_theme_init()` before any screen is built. Screens
  read colors via `ui_theme_color_lv(role)` / `ui_theme_color(role)` — never
  hard-code `lv_color_hex()`
- **Adding a color role**: append to `ui_color_role_t` in `ui_theme.h`
  (before `UI_COLOR__COUNT`) → `COLOR_ROLES` in `gen_themes.py` (same order)
  → every `theme.toml` (a manifest missing the role fails the build instead
  of silently defaulting to black)
- **Adding an artwork kind**: add an `ASSET_KINDS` entry in
  `gen_themes.py` + append the field to `ui_theme_t` + give the consumer a
  NULL fallback
- **Reserved fields**: `ui_theme_t` already carries `dial_face` / `font` /
  `bezel` (NULL today); new capabilities are always appended at the end of
  the struct — designated initializers zero-fill, so old themes keep
  compiling
- **Format details**: `CONFIG_LV_COLOR_16_SWAP=y` means RGB565 is emitted
  big-endian `(hi, lo)`; the needle can never use `LV_IMG_CF_ALPHA_8BIT`
  (undefined colors on the rotated draw path), so all artwork uses
  `TRUE_COLOR_ALPHA` / `TRUE_COLOR`. `theme.toml` is parsed by a built-in
  restricted-TOML parser (ESP-IDF ships Python 3.8 without `tomllib`);
  supported: comments, `[section]`, `key = value` (strings / decimal / 0x
  hex); anything else is an error with a line pointer

---

## 2. Runtime themes (SD card)

Firmware supplies data (`signal_store`), the theme engine and system pages (Settings/OTA/Bluetooth).
Custom images, fonts, layouts and theme videos are read from SD, with no Flash media cache. Boot
animation media is independent of the selected theme and is read only from the shared SD boot
directory. The legacy web uploader writes to that same directory.
Built-in default UI and compiled themes remain in firmware for fallback.
NVS stores the selected key; the SD sidecar is authoritative for rules and label overrides. A legacy config sidecar is migrated once when the SD copy is missing.

```text
/OBD/THEMES/<key>/
  theme.bin
  sidecar.json
  rpm.obv                  # optional ordinary theme video
/OBD/BOOT/boot.obv         # boot video (existing web upload workflow)
/OBD/BOOT/boot.jpg         # optional boot still
/OBD/BOOT/bootmedia.raw    # legacy boot animation
/OBD/VIDEO/rpm.obv        # optional shared fallback
```

`theme.bin` contains the 16 KB manifest, images/fonts and page layouts. Images/fonts must still
use the binary package format; arbitrary PNG/TTF files are not loaded directly. Theme videos
belong in their theme directory first; `/OBD/VIDEO` is an optional shared fallback directory.
VIDEO mode reads `/OBD/BOOT/boot.obv`, then `/OBD/BOOT/boot.jpg`, then legacy
`/OBD/BOOT/bootmedia.raw`. The existing web upload workflow continues to write to `/OBD/BOOT/`,
removing other old-format boot animations after the new file commits. Transfer interruption or validation failure preserves existing animations; old-file cleanup failure is reported as an error.
A still is displayed for 2.5 seconds.
Legacy raw animation duration is computed from `frame_count/fps` rather than manifest `duration_ms`, avoiding an extra last-frame hold when metadata disagrees.
JPEGs must be baseline and no larger than 360×360.
Packages no longer have the old 4 MB partition limit; SD space, format count
limits and runtime RAM budgets still apply.

### Protected boot pages

The following pages are **never themed** and always use the firmware
implementation; listing them under `theme_pages` is skipped and logged:

| Page ID | Content |
|---------|---------|
| `logo` | Sky Gauge logo (brand consistency) |
| `intro` | RACE AS ONE boot animation (NVS `intro_enable`: 0=OFF 1=RACE 2=VIDEO) |
| `boot_video` | Protected boot-media controller; reads shared SD media independently of the theme; player: `media/boot_video` |

The boot controller remains protected firmware code and is independent of theme selection. OBV1
files are played only after full validation. Missing or corrupt shared SD media falls back to the
built-in startup screen. Applying a theme does not change boot media.

### theme.bin format

`theme.bin` is exported by the theme editor (`obd_theme_store`), not a
hand-written directory; the firmware only knows the file's own binary layout
(code: `main/theme_engine/theme_loader.c`, `theme_internal.h`):

```
[0, 16 KB)   manifest JSON (parsed with cJSON_ParseWithLength, 0xFF-padded tail)
[16 KB, …)   images + fonts + per-page layout (offset/size given by the manifest)
```

Key fields and limits:

- First byte `0xFF` or `0x00` → the theme is treated as unavailable, built-in
  default theme is used
- `schema_version` must equal `"1.0"` exactly, otherwise
  `ESP_ERR_NOT_SUPPORTED` and the default theme is used
- `theme.id` is required, ≤64 chars; `name` / `version` / `author` are
  truncated on UTF-8 character boundaries when too long
- `assets.*`: `format` is `rgb565` / `rgba8888` / `lv_font_bin`, unknown
  formats are skipped entirely; width/height ≤2047 px; `size` must equal
  `width*height*bytes-per-pixel`; offset ≥16 KB; duplicate names keep the first
- `pages.theme_pages[]`: `{id, layout_data_offset, layout_data_size}`, one
  page's layout ≤64 KB; duplicate ids keep the first; ids `logo` / `intro` /
  `boot_video` are skipped per the previous section
- Colors fall back to the built-in default palette when missing
- Capacity: ≤32 pages, ≤16 images, ≤16 fonts; ≤128 elements and ≤32 data
  bindings per page

Fonts are `lv_font_bin`. The 'F' filesystem driver reads the font segment from SD into temporary
PSRAM during `lv_font_load`, then frees the segment on close; LVGL font objects remain until
unload. Failure logs "font '%s': lv_font_load failed, built-in font used" and uses a built-in font.
`theme_load()` refuses reload while theme page objects exist or a background boot-page
preparation is active. Theme switching/deletion waits for image reads to finish so it
cannot race the SD files used by the worker.

### Theme mode: with pages vs. without (`main/export_path/ui.c`)

The built-in gauge pages (Gear/Rpm/Speed/Temp/OilPressure/Needle) are skipped
only when the theme's **id is not `default` and it declares at least one
`theme_pages` entry** (saving 40–60 KB RAM); boot goes straight to the
theme's default page (the page with id `main_gauge`, else the first page),
falling back to the built-in default page if building it fails. Inside the
theme page carousel, swiping left/right moves between theme pages, and past
either end lands on the Info (EasterEgg) page; swiping down from the Gear
page also opens the theme's first page (it only requires a non-empty
`theme_pages`). The default page's images are read and converted by a background
task during the Logo / boot animation, leaving the boot screen visible. The
boot video replaces the Logo only after its first frame finishes transferring to the LCD.
If the video ends before theme preparation, playback releases its buffers and the gauge
returns to the Logo until the page is ready. Fullscreen playback guards the
LVGL refresh callback so page or layout invalidations cannot resume rendering, clear the
panel or overwrite borrowed DMA buffers. Normal refresh resumes after the decoder releases
the panel and buffers. LVGL creates the theme
widgets after video playback and preparation finish, then switches pages, so
neither transparent-image conversion nor widget creation interrupts video. If preparation finishes before playback starts, widgets can be created during the Logo. Theme-package streaming uses file-descriptor `read` calls and an internal DMA buffer for bulk transfers, avoiding byte-by-byte refills from `_IONBF` + `fread` on this target. Slow-read logs separate buffer waits, actual reads and callbacks. A
boot-video decode failure or interruption restores the Logo; failed theme
preparation falls back to the built-in page. Images, videos and other theme child widgets
bubble gestures to the page root, so swiping anywhere in the theme content
continues the carousel. Full navigation: [USER_GUIDE.en.md](USER_GUIDE.en.md#page-navigation).

LCD output also includes the upstream ESP-IDF 5.5.3 SPI bus-lock wakeup fix, preventing a cross-core request from waiting indefinitely at DMA interrupt exit. The low-speed LCD probe handle is released before the normal handle is registered. Only a build-local SDK source copy is replaced; the installed SDK is unchanged. On the tested device, preparation of the same 360×360 RGBA image takes about 238–240 ms; 20 consecutive USB resets each completed the 100-frame OBV2 video and entered the theme page. These measurements apply to that device and those resources, not every SD card, image or video.

**A theme with no pages**: in the current code, `colors.*` only supplies
default colors for the theme's own page elements — moot when it has no
pages. It does **not** change the built-in pages' colors; those always use
the compiled-in theme palette.

### Sidecar (rules + overlay)

Editor rules (alarms / recoloring / animation) and label overlays (rotation,
direction, numeric display format) do not go into `theme.bin`; they are kept
as a separate slim JSON:

- Location: SD `/OBD/THEMES/<key>/sidecar.json` (`<key>` =
  `storage_theme_key(theme.id)`), the only authoritative copy. Install/delete requires an SD card.
  A legacy config partition `/themes/<key>/sidecar.json` is migrated once when the SD file is
  missing, then the old copy is removed
- Input is either the reduced format itself (`"format":"gauge-sidecar/1"`) or
  a full `<id>.gaugeproj.json` (streamed and filtered, dropping large fields
  like images/fonts as it reads)
- Limits (`theme_sidecar_core.h`): reduced form ≤64 KB; ≤32 pages; ≤128
  element ids per page; one element id ≤128 chars; one element/rule/overlay
  entry ≤16 KB; overlong strings (>4096) are dropped while streaming
- Optional `asset_files`: asset name → gaugeproj file name, used by `vid_` /
  `video_` elements to switch videos with an `asset` rule
- Install failures: `ESP_ERR_INVALID_ARG` (bad JSON / format / theme id),
  `ESP_ERR_INVALID_SIZE` ("sidecar too big: reduced form exceeds 64 KB"),
  `ESP_ERR_NO_MEM`
- Install returns a pairing report: page/rule counts, truncation counts,
  `not in theme.bin: ...`, per-page matched/missing/kind-mismatch, unknown
  assets/sources, elements expected to be skipped by the PSRAM budget (up to
  8 names); the last line always reads "applies on the next page build;
  bin-only installs (app/store/web flash) have no overlay"; an overlong
  report ends in "..."
- API: `POST /api/v1/theme/sidecar` (chunked; ≤64 KB kept in PSRAM, larger
  streams to an SD temp file; an SD card is required for every save, otherwise
  409 `sd_unavailable`; 32 MB overall cap, 413 if exceeded; 422 `invalid_sidecar`
  on bad input; success `{"ok":true,"size":N,"report":"..."}`),
  `DELETE /api/v1/theme/sidecar[?theme=<id>]` (no parameter removes the
  current theme's). `/api/v1` requires `X-OTA-Token`. SD import of
  `*.gaugeproj.json` is installed the same way as a sidecar (`sd_import.c`)

### PSRAM budget and skip order

Building a page sizes its artwork budget by whether it has a video element
(`theme_img_cache.h`): **no video 2560 KB, with video 1536 KB**. Allocation
order when over budget:

1. Pointers and all their variants — required, kept
2. Rule-target images (images a rule can change the look of) — required,
   kept; their tint variants also count against the budget
3. Decorative images, bottom-to-top in stacking order, topmost dropped
   first when the budget runs out; decorative images must also leave 512 KB
   of PSRAM free (`TE_PSRAM_RESERVE`)

Dropped images log "'%s' skipped: over the page budget / PSRAM reserve". All images, including
untinted `rgb565`, own RAM pixels read from SD and count against the cache budget; Flash mmap
zero-copy is retired. Cache misses also need temporary source data and possibly resize buffers,
so package size differs from page RAM use. Tint variants over 128 KB (`TE_TINT_BAKE_MAX`) use
runtime recoloring instead of pre-baking.

### SD theme library

`/OBD/THEMES/<key>/theme.bin` is the firmware-managed runtime source. Users install and select
themes through the web UI or App and do not need direct SD-card access.
`storage_theme_key(theme.id)` generates
`<key>` (unsafe characters, leading/trailing dots and DOS reserved names handled; ≤64 chars).
Copy directly into that directory or import `*.bin` through `/OBD/IMPORT/`, leaving a `.ok` note.
WiFi OTA also installs directly to SD. Old Flash themes are migration sources only; existing
SD packages and selections are preserved.

The theme editor's `*-sd.zip` export is **not directly readable by firmware**. On a computer,
import the archive through the web UI or App; users do not need to organize SD files manually.
The archive places `theme.bin` in
`/OBD/THEMES/<key>/`, the rules sidecar in `/OBD/IMPORT/`, and theme videos beside the package;
firmware imports the sidecar and reads the package and videos directly from the SD theme folder.
Do not put the ZIP itself in `/OBD/IMPORT/`. Theme videos are exported by asset key and keep the
names referenced by the manifest. Theme images, fonts and videos are stored on SD; images and fonts
are inside `theme.bin`, while ordinary theme videos sit beside it. The editor's Media panel can use
an image or video for preview, but startup media is not part of the theme bundle. The existing web
uploader remains compatible and writes the independent boot media to `/OBD/BOOT/`.

Themes are selected through the web UI or App; the gauge has no theme picker or SD management page.
The following describes the firmware's internal theme-application behavior:

1. Deletes theme pages and calls `theme_unload()`.
2. Shows "Loading theme..." / "Loading theme NN%"; a worker validates the SD package and commits the NVS selection, polled every 500 ms.
3. Shows "Theme selected, restarting" and reboots after about 1.5 s to read SD directly.
4. On failure shows "Theme selection failed: <reason>" and reloads the old selection or built-in default.
5. Busy shows "Busy, try again"; an unusable package shows "Not a usable theme.bin".

Switching does not erase/write Flash media or copy the entire package into PSRAM. OTA, no card
or another storage job prevents switching. Interrupted old Flash writes become SD selections;
without a card the marker waits for retry. Ordinary SD import cannot replace the selected package:
switch themes first. OTA can replace it after theme pages are released. Boot without a card uses
the built-in UI; apply again after insertion. Removing a card during use may leave cached images
visible, but later page/asset reads fail; immediate whole-UI fallback is not guaranteed.

### Theme video

A theme image element whose id starts with `vid_` or `video_` is treated as a
video element: the image itself is the poster, and the key is the id with
that prefix stripped. Lookup order is
`/OBD/THEMES/<theme_key>/<key>.obv`, then `/OBD/VIDEO/<key>.obv` (for sharing
across themes); if neither exists, the poster stays. A `visible` rule can pause the video, and an
`asset` rule switches to another file named by the sidecar's `asset_files`
(no folder or extension). Theme videos loop, and only one video decodes at a
time.
Theme directory keys and video keys support up to 64 characters; the player path buffer accommodates
the complete SD path for a theme video.

Looping theme videos use the custom **OBV1** container
(`main/media/obv1_format.h`, produced by `tools/make_video.py`):
`[64 B header][8 B × frame_count frame index][JPEG frame data]`, width/height
8..360 and multiples of 8, fps 1..60, up to 36000 frames, each frame ≤48 KB,
checked by CRC-32. Web boot-animation uploads use OBV2 below. Commands:

```bash
python3 tools/make_video.py boot INPUT -o boot.obv            # boot video, defaults 360px/4:2:2/Q75/24fps
python3 tools/make_video.py theme INPUT -o rpm.obv --size 240x240 [--poster rpm.png]
python3 tools/make_video.py inspect boot.obv                  # print the header
python3 tools/make_video.py verify boot.obv                   # check CRCs and every frame
python3 tools/make_video.py legacy INPUT -o bootmedia/slot_a [--raw bootmedia.raw]  # old delta_varint boot animation
```

Needs Pillow (`pip install Pillow`); video inputs also need ffmpeg.

Local web boot animation uses **OBV2**; looping theme videos continue to use OBV1. OBV2 keeps the `OBV1` magic and 64 B header,
with `version=2`, `fps=0`, and `timescale=1000000`; each 16 B index entry is `offset,size,timestamp_us,duration_us`.
Timestamps start at 0 and are contiguous, frame intervals are ≥16666 µs, and `duration_ms` rounds the total duration up.
Limits are 36000 frames, ≤48 KiB per frame, ≤256 MiB per file, and ≤1.25 MiB for a sliding one-second compressed window plus wrap headroom.
Header, index, and frame data each have CRC-32. The browser converts H.264 MP4 to aspect-preserving, uncropped 360×360 Q90 baseline JPEG 4:2:0,
retaining the source timeline and reporting budget overflow. Firmware prebuffers about one second of compressed frames and reads SD sequentially.
360×360/4:2:0 uses whole-frame decoding and two DMA strip buffers for display output; MCU-aligned assets with a passing decoder self-test can use block decoding.
OBV2 supports only non-looping fullscreen boot playback; three consecutive frames missing their individual display deadlines log an error and stop playback,
without silently dropping frames or stretching the timeline. These are format limits; actual frame rate still requires device testing.
USB `BEGIN_OBV=7` stages, validates, and installs `/OBD/BOOT/boot.obv`. The tool's `boot/theme` commands still produce OBV1;
`inspect/verify` read both versions, and legacy BIN/raw animations remain compatible.

### Install / push

The theme editor merges the former BIN and SD-bundle exports into **Export theme ZIP**, downloading one `<id>.zip`.
The ZIP contains `/OBD/THEMES/<key>/theme.bin`, the theme's `*.obv` videos, and
`/OBD/IMPORT/<key>.gaugeproj.json`; images, fonts, and layouts are in `theme.bin`, while rules come from gaugeproj.
Export is blocked when referenced video files need relinking. Boot animation is independent and stays under shared `/OBD/BOOT/`.

Recommended workflow: insert an SD card, connect the ESP32-S3 native USB port to the web flasher, and choose **From community** or
**Upload local files** in the **Theme** section. The page checks paths, CRCs, theme IDs, data ranges, and video dependencies, unpacks it, and uploads files in order.
Firmware verifies SHA-256, stages files, and generates `sidecar.json`; after all steps succeed it saves the theme selection and restarts.
No manual extraction or separate activation is needed. First install firmware with USB theme-bundle support; older firmware prompts an upgrade.

New complete ZIPs, old `*-sd.zip` files, and STORE / DEFLATE compression are supported. Legacy BIN, companion gaugeproj / sidecar JSON,
and `.obv` files can also be selected together. A BIN alone retains its images, fonts, and layouts but cannot restore missing rules or videos;
upload is rejected if a declared video is missing. New community publications store complete ZIPs; historical BIN files remain installable,
with format detected from content. After a complete firmware flash, reconnect before installing themes or animations.

Updates to an existing theme ID are supported: before commit, release active theme resources, then replace the whole directory using staging,
a backup, and a recovery journal. Failed installation rolls back; if power fails during commit, the next boot with SD mounted attempts to restore
old files and selection. Before upload, at least `2 × upload stream size + 1 MiB` of free space is required. Both the web ZIP and upload stream
are limited to 256 MiB, with at most 128 installed files. Host tests cover directory replacement, validation failure, selection rollback, and
interruption recovery; they do not establish actual SD-card power-loss reliability.

Alternatively extract the ZIP at the SD root, preserving existing files. Rules are imported automatically at boot, but copying does not activate the theme.
Web theme installation uses ESP32-S3 native USB; the App WiFi OTA path remains compatible.

**WiFi OTA push** (enter OTA mode; protocol:
[APP_PROTOCOL.en.md](APP_PROTOCOL.en.md#wifi-ota-http-api)):

- `POST /ota/theme/prepare` — check SD mount and space; returns `storage:"sd"`. PSRAM fields are diagnostic.
- `POST /ota/theme` — stream to an SD temporary file, verify SHA and manifest, import, save selection and automatically reboot.
- `POST /ota/theme/erase` — delete the selected theme's entire `/OBD/THEMES/<key>/` directory (including `theme.bin`, rules, images, fonts, and dedicated videos) and clear selection; the built-in theme loads on next boot. Requires a mounted SD card; other themes and the shared `/OBD/BOOT/` boot animation are untouched. Returns `{"ok":true,"deleted":true|false}`.

While theme selection runs these endpoints and the first `/ota/bootmedia` chunk return
"theme library busy". Themes transfer via USB, SD, or WiFi; no BLE theme upload. Without a card,
media upload returns "SD card required".

### Runtime behavior

- At boot `theme_engine_init()` reads the SD package selected in NVS: missing card/file,
  corrupt manifest or parse failure falls back to the built-in default.
- While a theme page is on screen, the LVGL task normally calls `theme_tick()`
  every 16 ms (animations/alarms use their respective refresh periods): it reads `signal_store` once (keys defined in
  `contracts/signals.json`), updates data bindings (only changed values touch
  LVGL objects), then evaluates sidecar rules on that same snapshot and
  applies the diff; it is a no-op with no theme page on screen
- While a theme page is on screen during the Bluetooth-connect gauge sweep, rule evaluation temporarily simulates all 12 standard
  `obd.*` signals as changing inputs, so threshold conditions and `change` rules for every signal can respond to the sweep.
  Rule input immediately returns to real `signal_store` data when the sweep ends; simulated values are never written to or used
  to overwrite real OBD data. Vehicle-specific `veh.*` signals have no shared range and continue using their live data during sweep
- Rule conditions/hysteresis/debounce/timing state still advance on each call. An unchanged
  active set reuses the sorted action coverage; changes rebuild it and remove overrides of
  rules that exit. Rebuilding a page or theme clears the cache.
  When both the set and system-alarm suppression stay unchanged, target look assembly/comparison
  is skipped as well. Bindings continue updating; rule exits, suppression changes, and the first
  update after reset still apply looks. Ending an alarm still restores the page background.
- For arcs and bars, rule-action `set.color` changes only the indicator/fill color; the track retains the element's `bgColor`. Immediate changes and color animations follow the same rule, so crossing a sweep threshold does not light the unfilled track and appear to jump to full scale. When the rule exits, the original indicator color returns. Existing theme packages only need a firmware update, with no re-export.
- Arc progress uses the full 0–10000 value range without the 8191 pixel-coordinate limit; both sweep and live readings can reach full scale.
- Rule-action `set.color` supports `animation` modes `fade`, `keyframes` and `strobe`, updating colors
  or image tint on each theme tick. `change` and `timer` rules remain active through the longest
  animation duration. Frames are not pre-generated; `text`, `visible` and `asset` still change
  immediately, and image-resource changes are not crossfaded.
- Label fonts: try the manifest's `font_asset` first, fall back to built-in
  Montserrat 12/14/16/26/32/48 on failure (exact match first, else the
  nearest smaller size, else 12 as a last resort); sizes above 48 use the 48
  font with `transform_zoom = size*256/48` (LVGL's zoom, unrelated to the
  compiled-in theme's artwork scaling)
- A missing value or NaN shows `--` on the label (no unit)
- System alarms take priority over theme rules: while the RPM warning flash
  or the triple-gauge linked red owns the background, theme rules are
  suppressed (data bindings keep refreshing); when it ends, built-in pages
  get their palette background + dial face back, and theme pages get their
  own layout's `bg_color` back
- Layouts and assets are read from SD in segments; cache hits do not read SD. Image conversion,
  cropping, tinting and pointer resizing use PSRAM. Identity-size images still own RAM pixels;
  referenced cache entries remain alive. Font/layout source segments are freed after parsing.
  SD lock waits are bounded to one second; failure ends the current load. Host tests cover
  pixel/geometry compatibility and read/allocation cleanup, not physical SD/LCD performance.
- API reference: [main/theme_engine/theme_interface.h](../main/theme_engine/theme_interface.h)

### Partition background

Physical layout v2 is preserved: `theme_0` (0x620000, 4 MB) and `bootmedia` (0xA20000, 4.75 MB)
are legacy migration sources / reserved space totaling 8.75 MiB. New themes and animations do
not use them; this change does not reassign the space to firmware or config.
See [partitions.csv](../partitions.csv). Devices already on layout v2 can update by OTA;
older partition tables need one full reflash because OTA cannot change the table.
