# Theme Sources

English | [简体中文](README.md)

Sources for the **compiled-in themes** (TOML manifests + artwork). The full
guide — authoring walkthrough, color roles, artwork specs, registry rules,
plus the separate runtime-theme (theme_0 partition) system — lives in
**[docs/THEMES.en.md](../docs/THEMES.en.md)**.

```
themes/
├── registry.txt        # slot -> id registry, append only (slot 0 must be default)
├── _TEMPLATE/          # copy this to start a new theme
├── builtin/            # shipped with the firmware: default / amber / ocean
└── community/          # community themes go here
```

Runtime themes (theme_0 / the SD theme library) are `theme.bin` files exported by the editor; they don't live here.

Fastest start:

```bash
cp -r themes/_TEMPLATE themes/community/sunset
$EDITOR themes/community/sunset/theme.toml
echo "3  sunset" >> themes/registry.txt
idf.py build flash
```

`tools/gen_themes.py` turns these manifests into
`main/export_path/ui_theme_generated.c` at CMake configure time (never edit
generated files).
