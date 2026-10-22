# 主题源文件

[English](README.en.md) | 简体中文

这里放**编译期主题**（TOML 清单 + 素材）的源文件。完整文档 ——
制作教程、颜色角色、素材规格、registry 规则，以及另一套运行时主题
（theme_0 分区）的说明 —— 全部在 **[docs/THEMES.md](../docs/THEMES.md)**。

```
themes/
├── registry.txt   # slot -> id 登记表，只能末尾追加（槽位 0 必须 default）
├── _TEMPLATE/     # 复制这个开始做新主题
├── builtin/       # 固件自带：default / amber / ocean
└── community/     # 社区主题放这里
```

运行时主题（theme_0 / SD 主题库）由编辑器导出 `theme.bin`，不放在这里。

最快上手：

```bash
cp -r themes/_TEMPLATE themes/community/sunset
$EDITOR themes/community/sunset/theme.toml
echo "3  sunset" >> themes/registry.txt
idf.py build flash
```

`tools/gen_themes.py` 在 CMake configure 阶段把这些清单生成
`main/export_path/ui_theme_generated.c`（不要手改生成文件）。
