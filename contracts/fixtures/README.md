# 黄金向量（fixtures）

这里的向量是固件规则核心和 label 格式化器的验收标准：**C 端（`test/host`）和 node 端（直接跑编辑器源码）跑同一套文件**，两边都通过才算与编辑器预览一致。契约正文见 [`../theme-editor-contract.md`](../theme-editor-contract.md)。

## 编辑器锁定

| 项 | 值 |
|----|----|
| 仓库 | `obd_theme_store`（GitHub `steveEcode/obd-theme-store`，私有） |
| 分支 | `codex/local-frontend`（规则系统只在这个未合并分支上） |
| commit | `5a7484e`（`5a7484ef2b25165e0f722ab4da6d8a18ff32e631`） |
| 被执行的源码 | `frontend/src/lib/ruleEngine.ts`（sha256 `d8288789…5dcbc`）、`themePacker.ts`、`themeContract.ts`、`gaugeModel.ts` |

完整 commit、文件哈希和编辑器限额在 [`editor-pin.json`](editor-pin.json)。升级锁定版本时：改 `editor-pin.json`，重跑下面的交叉校验（必要时 `--emit-format` 重新生成格式向量），在同一提交里更新契约文档。

## 交叉校验（node 端）

```bash
node tools/fixtures_node/run_ts_crosscheck.mjs --editor /path/to/obd_theme_store
```

- 只读编辑器 checkout，不安装任何 npm 依赖；需要能直接运行 `.ts` 的 Node（≥ 23.6，或 22.18+；CI 用 Node 24）。
- 检查：① 规则向量对 `ruleEngine.ts`；② 格式向量对 `themePacker.ts` 的 `formatIntPrintf`；③ `signals.json` 的 `editor` 块对 `gaugeModel.ts`/`themeContract.ts`；④ `editor-pin.json` 的限额对 `THEME_LIMITS`。checkout 不是锁定 commit 时报错（`--allow-commit-mismatch` 降为警告）。
- `--emit-format` 用编辑器代码重新生成 `format/label_format.json`。

本地实测（2026-10-11，Node v24.11.1，editor @ 5a7484e）：

```
rules:   vectors=38 steps pass=79 fail=0
format:  cases=24 pass=24 fail=0
signals: editor keys=11 mismatches=0
limits:  keys=10 mismatches=0
RESULT: PASS
```

## `rules/vectors.json`

38 组、79 步。T01–T13 对应编辑器原 `ruleEngine.test.ts` 的 13 条，E01–E25 是补充的边界用例（`origin` 字段注明出处）。文件是一个数组，每项：

| 字段 | 含义 |
|------|------|
| `name` | 唯一名字 |
| `origin` | 出处（原测试行号或 `ruleEngine.ts` 行号） |
| `api` | `isRuleActive`（单条规则是否命中）或 `evaluatePageRules`（整页：active 列表 + 覆盖表） |
| `runtime` | `none`：每步都不带 runtime（`isRuleActive` 不做滞后，`evaluatePageRules` 每步新建 runtime）；`shared`：同一组的各步共用一份 runtime（锁存、debounce 状态跨步保留） |
| `rule` | `api = isRuleActive` 时的 `GaugeRule`（编辑器 camelCase 原样） |
| `page` | `api = evaluatePageRules` 时的页：`{id, elements:[{id, kind}], rules:[GaugeRule]}` |
| `steps[]` | 按顺序执行的步骤 |

每一步：

| 字段 | 含义 |
|------|------|
| `values` | 数据源取值。**基线是 11 个编辑器数据源全为 0**，再用 `values` 覆盖；不在这个集合里、也没出现在 `values` 里的源（例如 `obd.fuel_level`）视为缺失 → NaN |
| `elapsedMs` | 页面已运行时间，缺省 0 |
| `changed` | 本步处于 change 窗口内的源列表，缺省空 |
| `expect.active` | `isRuleActive` 的期望布尔值 |
| `expect.activeRules` | `evaluatePageRules` 期望的 active 规则 id 列表（**有序**，即 priority 稳定排序后的顺序） |
| `expect.overrides` | 期望的覆盖表：`{elementId: {color?, bgColor?, tintColor?, text?, visible?, assetName?}}`，只列出被设置的键；比较时不看键顺序 |

`expect` 里没出现的项不比较。C 端实现时：`api = isRuleActive` 且 `runtime = none` 等价于「不传 runtime」（滞后被跳过，`ruleEngine.ts:54`）。

## `format/label_format.json`

24 条 label 格式化向量，由 `run_ts_crosscheck.mjs --emit-format` 跑编辑器代码生成。

```
text = formatIntPrintf(fmt, args)
fmt  = (format 非空 且 isIntOnlyFormat(format)) ? format : "%d"
args = divisor > 1 ? [trunc(raw / divisor), abs(raw % divisor)] : [trunc(raw)]
```

| 字段 | 含义 |
|------|------|
| `name` | `F01`..`F24` |
| `format` | layout 里 label 的 `format`；`null` 表示字段缺失 |
| `divisor` | layout 里 label 的 `divisor`；`null` 表示字段缺失 |
| `raw` | 交付给 label 的数值（编码见 `signals.json` 的 `delivery`） |
| `text` | 编辑器预览显示的文本（固件必须逐字节一致） |

包含编辑器的怪癖，固件照搬：`%u` 对负数带负号、`%.2d` 输出 `52d`、负小数 `-50/100` 显示 `0.50`、`%s` 被替换成 `%d`。

## 计划中的补充（P4）

- overlay 的 `transformDataValue` / `valueMap` / `precision`（`toFixed`）/ `unit` 向量（`FabricCanvas.tsx:917-933, 1217-1251`）。这些函数在 React 组件文件里、不导出，需要先确定抽取方式。
