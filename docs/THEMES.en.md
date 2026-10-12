# Theme System

English | [简体中文](THEMES.md)

This project has **two coexisting theme systems** for different scenarios:

| | Compiled-in theme (TOML) | Runtime theme (theme_0 partition) |
|---|---|---|
| What changes | 8 decorative colors + bezel/needle/dial artwork (reskin, no layout) | Colors + artwork + **custom gauge pages and layout** (layout.json) |
| How it installs | The theme folder compiles into the firmware; reflash | Packed into a 4 MB `theme.bin`; USB flash or WiFi OTA push |
| How it activates | Settings → THEME roller (reboot to apply) | Device loads the theme_0 partition at boot |
| Who it's for | Quickly reskinning the built-in pages | Defining your own gauge page composition and data bindings |
| Sources | [themes/](../themes/) | `themes/example_boost_oil/` + [tools/theme_packer/](../tools/theme_packer/) |

The two cooperate: when a runtime theme declares pages, boot skips the
built-in gauge pages (saving 40–60 KB RAM) and the theme pages join the
carousel (entered by swiping down from the GEAR page); a missing or corrupt
runtime theme falls back to the built-in pages automatically.

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
| `UI_SEM_ALERT` | `0xFF4D4D` | Alarm / over-threshold text |
| `UI_SEM_FLASH` | `0xFF0000` | Full-screen RPM warning flash |
| `UI_SEM_ON` | `0x06D6A0` | Toggle ON / positive state |
| `UI_SEM_WARN` | `0xFFD166` | Caution (oil pressure etc.) |

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

After boot, **Settings → THEME** lists it; selecting **reboots** the device —
screens are created once at boot, so a restart is the most reliable reskin.
For quick validation run the generator standalone (errors point at exact
file and line):

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

## 2. Runtime themes (theme_0 partition)

Separates UI presentation from the firmware: the firmware provides data and
system pages (settings/OTA/Bluetooth), while the `theme_0` partition
(4 MB @ `0x620000`) provides gauge pages, layout, colors and artwork.

```
┌────────────────────────────────────┐      ┌─────────────────────────────────┐
│ Firmware (ota_0/ota_1, 3MB each)   │      │ theme_0 partition (4MB)         │
│ OBD / BLE+WiFi / NVS / settings    │ ───→ │ theme_manifest.json (≤16KB hdr) │
│ Theme engine (load + bind data)    │ read │ assets/ (dial.png, ring.png)    │
└────────────────────────────────────┘      │ layout.json (page elements)     │
                                            └─────────────────────────────────┘
```

### Protected boot pages

The following pages are **never themed** and always use the firmware
implementation; manifests must list them under `system_pages` (entries in
`theme_pages` are ignored):

| Page ID | Content |
|---------|---------|
| `logo` | Sky Gauge logo (brand consistency) |
| `intro` | RACE AS ONE boot animation (NVS `intro_enable`: 0=OFF 1=RACE 2=VIDEO) |
| `boot_video` | Custom boot video (`boot_block.bin` in the `bootmedia` partition, played by `boot_block_player`) |

Rationale: the boot flow must not depend on possibly-corrupt theme data, and
users should see the same startup experience every time.

### Building and packing

Theme directory layout (see [themes/example_boost_oil/](../themes/example_boost_oil/)):

```
my_theme/
├── theme_manifest.json   # required
├── layout.json           # optional, custom page elements and data bindings
└── assets/
    ├── dial.png          # optional, 360×360 RGB
    └── ring.png          # optional, 360×360 RGBA
```

`theme_manifest.json` (schema 1.0):

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
    "comment": "logo/intro/boot_video must be listed in system_pages",
    "system_pages": ["logo", "intro", "boot_video", "settings", "ota", "bluetooth_pair"],
    "theme_pages": ["main_gauge"]
  }
}
```

Pack into a 4 MB partition image (0xFF-padded, 16 KB manifest header reserve):

```bash
python3 tools/theme_packer/pack_theme.py themes/my_theme my_theme.bin
```

### Flash / push

**USB** (first time or debugging):

```bash
esptool.py --chip esp32s3 -p PORT write_flash 0x620000 my_theme.bin
```

**WiFi OTA push** (device in OTA mode; from the app or curl, protocol in
[APP_PROTOCOL.en.md](APP_PROTOCOL.en.md#wifi-ota-http-api)):

- `POST /ota/theme/prepare` — preflight, returns mount status (no erase)
- `POST /ota/theme` — chunked upload (`X-OTA-SHA256` / `X-OTA-Size` /
  `X-OTA-Offset` / `X-Last` headers)
- `POST /ota/theme/erase` — erase the whole theme partition; next boot falls
  back to the built-in default theme

Themes go over WiFi OTA or USB flashing only; there is no BLE transfer
path.

### Runtime behavior

- At boot `theme_engine_init()` reads theme_0: corrupt manifest / empty
  partition / parse failure → automatic fallback to the built-in default
  theme, never a brick
- When the theme declares pages, the built-in gauge pages (Gear/RPM/Speed/
  Temp/Chart …) are not created and the theme pages join the carousel; data
  is pushed periodically via `theme_update_data()` (`obd_snapshot_t`, a
  16-byte ABI-stable struct: rpm/speed/boost/coolant/oil pressure/gear/
  voltage/oil temp/AFR/throttle/intake temp)
- Artwork is memory-mapped straight from flash (zero-copy mmap), no heap cost
- Switching / updating a theme takes effect after a reboot (same as
  compiled-in themes)
- API reference: [main/theme_engine/theme_interface.h](../main/theme_engine/theme_interface.h)

### Partition background

`theme_0` (0x620000, 4 MB) and `bootmedia` (0xA20000, 5.875 MB) — see
[partitions.csv](../partitions.csv). A single theme slot is enough: a bad
theme falls back to the default, so no A/B slots. Devices on the old
partition table (no theme_0, bootmedia at the old address) must be upgraded
with a **one-time full USB reflash** — OTA cannot rewrite the partition
table itself.
