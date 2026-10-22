#!/usr/bin/env node
// Cross-checks contracts/ against the editor's own TypeScript sources.
//
//   node tools/fixtures_node/run_ts_crosscheck.mjs --editor <obd_theme_store checkout> [options]
//
//   --editor DIR              obd_theme_store checkout (read-only; required)
//   --repo DIR                firmware repo root (default: two levels above this script)
//   --emit-format             regenerate contracts/fixtures/format/label_format.json from the editor
//   --allow-commit-mismatch   only warn when the checkout is not the pinned commit
//
// Checks:
//   1. rules   contracts/fixtures/rules/vectors.json against ruleEngine.ts (verbatim editor code)
//   2. format  contracts/fixtures/format/label_format.json against themePacker.ts formatIntPrintf
//   3. signals contracts/signals.json "editor" blocks against gaugeModel.ts / themeContract.ts
//   4. limits  contracts/fixtures/editor-pin.json theme_limits against themeContract.ts THEME_LIMITS
//
// Needs a Node.js that runs .ts files natively (>= 23.6, or 22.18+) and module.registerHooks
// (>= 23.5 / 22.15). The editor sources import siblings without an extension ('./gaugeModel'),
// so a resolve hook appends '.ts' for extensionless relative specifiers.

import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import * as nodeModule from 'node:module';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

function parseArgs(argv) {
  const opts = { editor: null, repo: resolve(HERE, '..', '..'), emitFormat: false, allowCommitMismatch: false };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--editor') opts.editor = argv[++i];
    else if (a === '--repo') opts.repo = argv[++i];
    else if (a === '--emit-format') opts.emitFormat = true;
    else if (a === '--allow-commit-mismatch') opts.allowCommitMismatch = true;
    else if (a === '-h' || a === '--help') {
      console.log(readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n').slice(1, 17).join('\n'));
      process.exit(0);
    } else {
      console.error(`unknown argument: ${a}`);
      process.exit(2);
    }
  }
  if (!opts.editor) {
    console.error('usage: run_ts_crosscheck.mjs --editor <obd_theme_store checkout> [--emit-format] [--allow-commit-mismatch]');
    process.exit(2);
  }
  opts.editor = resolve(opts.editor);
  opts.repo = resolve(opts.repo);
  return opts;
}

const opts = parseArgs(process.argv.slice(2));

if (!process.features?.typescript) {
  console.error('error: this Node.js cannot run .ts files natively; use Node >= 23.6 (or 22.18+), or pass --experimental-strip-types');
  process.exit(2);
}
if (typeof nodeModule.registerHooks !== 'function') {
  console.error('error: module.registerHooks is missing; use Node >= 23.5 (or 22.15+)');
  process.exit(2);
}

const LIB = join(opts.editor, 'frontend', 'src', 'lib');
nodeModule.registerHooks({
  resolve(specifier, context, nextResolve) {
    const fromEditor = context.parentURL && context.parentURL.startsWith(pathToFileURL(LIB).href);
    if (fromEditor && /^\.{1,2}\//.test(specifier) && !/\.[cm]?[jt]sx?$/.test(specifier)) {
      return nextResolve(`${specifier}.ts`, context);
    }
    return nextResolve(specifier, context);
  },
});

const contracts = join(opts.repo, 'contracts');
const pin = JSON.parse(readFileSync(join(contracts, 'fixtures', 'editor-pin.json'), 'utf8'));
let failures = 0;
const fail = (msg) => { failures += 1; console.log(`FAIL ${msg}`); };
const warn = (msg) => console.log(`WARN ${msg}`);

// ---- 0. editor checkout identity -------------------------------------------------------------
let head = null;
try {
  head = execFileSync('git', ['-C', opts.editor, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
} catch {
  warn('editor checkout is not a git work tree; commit not verified');
}
if (head !== null && head !== pin.commit) {
  const msg = `editor HEAD ${head} != pinned ${pin.commit}`;
  if (opts.allowCommitMismatch) warn(msg); else fail(msg);
}
for (const [rel, want] of Object.entries(pin.files)) {
  const path = join(opts.editor, rel);
  if (!existsSync(path)) { fail(`missing editor file ${rel}`); continue; }
  const got = createHash('sha256').update(readFileSync(path)).digest('hex');
  if (got !== want) warn(`${rel} sha256 ${got.slice(0, 12)}... differs from pin ${want.slice(0, 12)}... (local edits?)`);
}
console.log(`editor: ${opts.editor} @ ${head ?? 'unknown'} (pinned ${pin.commit.slice(0, 7)})`);

const ruleEngine = await import(pathToFileURL(join(LIB, 'ruleEngine.ts')).href);
const packer = await import(pathToFileURL(join(LIB, 'themePacker.ts')).href);
const contract = await import(pathToFileURL(join(LIB, 'themeContract.ts')).href);
const model = await import(pathToFileURL(join(LIB, 'gaugeModel.ts')).href);

function sortKeys(v) {
  if (Array.isArray(v)) return v.map(sortKeys);
  if (v && typeof v === 'object') return Object.fromEntries(Object.keys(v).sort().map((k) => [k, sortKeys(v[k])]));
  return v;
}
const sameJson = (a, b) => JSON.stringify(sortKeys(a)) === JSON.stringify(sortKeys(b));

// ---- 1. rule vectors -------------------------------------------------------------------------
// Semantics of a vector file entry: see contracts/fixtures/README.md. Every step starts from all
// 11 editor sources = 0, overlaid with step.values; runtime 'shared' keeps one Map across steps.
{
  const vectors = JSON.parse(readFileSync(join(contracts, 'fixtures', 'rules', 'vectors.json'), 'utf8'));
  const zero = Object.fromEntries(model.DATA_SOURCES.map((k) => [k, 0]));
  let pass = 0;
  let bad = 0;
  for (const vec of vectors) {
    const shared = new Map();
    vec.steps.forEach((step, i) => {
      const values = { ...zero, ...(step.values ?? {}) };
      const elapsed = step.elapsedMs ?? 0;
      const changed = new Set(step.changed ?? []);
      const rt = vec.runtime === 'shared' ? shared : undefined;
      let ok = true;
      let got;
      if (vec.api === 'isRuleActive') {
        got = ruleEngine.isRuleActive(vec.rule, values, elapsed, changed, rt);
        ok = got === step.expect.active;
      } else if (vec.api === 'evaluatePageRules') {
        const r = ruleEngine.evaluatePageRules(vec.page, values, elapsed, changed, rt);
        got = { activeRules: r.activeRules.map((x) => x.id), overrides: r.overrides };
        if ('activeRules' in step.expect) ok = ok && sameJson(got.activeRules, step.expect.activeRules);
        if ('overrides' in step.expect) ok = ok && sameJson(got.overrides, step.expect.overrides);
      } else {
        ok = false;
        got = `unknown api ${vec.api}`;
      }
      if (ok) pass += 1;
      else {
        bad += 1;
        fail(`rules ${vec.name} step ${i}: got ${JSON.stringify(got)} expected ${JSON.stringify(step.expect)}`);
      }
    });
  }
  console.log(`rules:   vectors=${vectors.length} steps pass=${pass} fail=${bad}`);
}

// ---- 2. label format vectors -----------------------------------------------------------------
// labelText() is the data-label branch of FabricCanvas.tsx createLabel (lines 1224-1233) without
// dataDisplay; the formatter itself is the editor's exported themePacker code.
function labelText(format, divisor, raw) {
  const fmt = format && packer.isIntOnlyFormat(format) ? format : '%d';
  if (divisor && divisor > 1) {
    return packer.formatIntPrintf(fmt, [Math.trunc(raw / divisor), Math.abs(raw % divisor)]);
  }
  return packer.formatIntPrintf(fmt, [Math.trunc(raw)]);
}

const FORMAT_CASES = [
  ['%d', null, 1234], ['%d.%02d', 100, 150], ['%d.%02d', 100, 105], ['%d.%d', 10, 136],
  ['%d.%02d', 100, -150], ['%d.%02d', 100, -50], ['%5d', null, 42], ['%-5d|', null, 42],
  ['%05d', null, -42], ['%+d', null, 5], ['%x', null, 255], ['%X', null, -1],
  ['%u', null, -5], ['% d', null, 5], ['%d %d', null, 7], ['%c', null, 65],
  ['%d%%', null, 65], ['%s', null, 3], ['%.2d', null, 5], ['%d km/h', null, 88],
  ['%o', null, 8], ['%d.%02d', 100, 0], [null, null, 3500], ['%+05d', null, 7],
];

const formatPath = join(contracts, 'fixtures', 'format', 'label_format.json');
if (opts.emitFormat) {
  const doc = {
    format: 'skygauge.fixtures.label-format/1',
    generator: 'tools/fixtures_node/run_ts_crosscheck.mjs --emit-format',
    editor: {
      repo: pin.repo,
      commit: pin.commit,
      code: [
        'frontend/src/lib/themePacker.ts:34-44 isIntOnlyFormat',
        'frontend/src/lib/themePacker.ts:46-101 formatOneIntConversion, formatIntPrintf',
        'frontend/src/components/canvas/FabricCanvas.tsx:1224-1233 createLabel data branch (no dataDisplay)',
      ],
    },
    semantics: 'text = formatIntPrintf(fmt, args) with fmt = (format && isIntOnlyFormat(format)) ? format : "%d"; args = divisor > 1 ? [trunc(raw/divisor), abs(raw % divisor)] : [trunc(raw)]. null = field absent in the layout JSON.',
    cases: FORMAT_CASES.map(([format, divisor, raw], i) => ({
      name: `F${String(i + 1).padStart(2, '0')}`,
      format,
      divisor,
      raw,
      text: labelText(format ?? undefined, divisor ?? undefined, raw),
    })),
  };
  writeFileSync(formatPath, `${JSON.stringify(doc, null, 2)}\n`);
  console.log(`format:  wrote ${doc.cases.length} cases to ${formatPath}`);
}
{
  const doc = JSON.parse(readFileSync(formatPath, 'utf8'));
  let pass = 0;
  let bad = 0;
  for (const c of doc.cases) {
    const got = labelText(c.format ?? undefined, c.divisor ?? undefined, c.raw);
    if (got === c.text) pass += 1;
    else {
      bad += 1;
      fail(`format ${c.name} ${JSON.stringify(c.format)}/${c.divisor} raw=${c.raw}: editor gives ${JSON.stringify(got)}, fixture says ${JSON.stringify(c.text)}`);
    }
  }
  console.log(`format:  cases=${doc.cases.length} pass=${pass} fail=${bad}`);
}

// ---- 3. signal registry vs editor data sources -----------------------------------------------
{
  const reg = JSON.parse(readFileSync(join(contracts, 'signals.json'), 'utf8'));
  const editorSignals = reg.signals.filter((s) => s.editor).sort((a, b) => a.editor.index - b.editor.index);
  let bad = 0;
  const check = (cond, msg) => { if (!cond) { bad += 1; fail(`signals ${msg}`); } };
  check(editorSignals.length === model.DATA_SOURCES.length,
    `registry has ${editorSignals.length} editor keys, editor DATA_SOURCES has ${model.DATA_SOURCES.length}`);
  model.DATA_SOURCES.forEach((key, i) => {
    const s = editorSignals[i];
    if (!s) return;
    check(s.key === key, `editor index ${i}: registry ${s.key} != editor ${key}`);
    const spec = contract.DATA_SOURCE_SPECS[key];
    for (const f of ['label', 'unit', 'rawScale', 'min', 'max', 'preview']) {
      check(spec && s.editor[f] === spec[f], `${key}.${f}: registry ${JSON.stringify(s.editor[f])} != editor ${JSON.stringify(spec?.[f])}`);
    }
    check(s.editor.pickerLabel === model.DATA_SOURCE_LABELS[key],
      `${key}.pickerLabel: registry ${JSON.stringify(s.editor.pickerLabel)} != editor ${JSON.stringify(model.DATA_SOURCE_LABELS[key])}`);
    check(s.editor.divisor === (model.DATA_SOURCE_DIVISORS[key] ?? null), `${key}.divisor mismatch`);
    check(s.editor.format === (model.DATA_SOURCE_FORMATS[key] ?? null), `${key}.format mismatch`);
  });
  console.log(`signals: editor keys=${model.DATA_SOURCES.length} mismatches=${bad}`);
}

// ---- 4. editor limits used by the contract ---------------------------------------------------
{
  let bad = 0;
  for (const [k, v] of Object.entries(pin.theme_limits)) {
    if (contract.THEME_LIMITS[k] !== v) {
      bad += 1;
      fail(`limits ${k}: pin ${v} != editor ${contract.THEME_LIMITS[k]}`);
    }
  }
  for (const k of Object.keys(contract.THEME_LIMITS)) {
    if (!(k in pin.theme_limits)) { bad += 1; fail(`limits ${k} exists in the editor but not in editor-pin.json`); }
  }
  console.log(`limits:  keys=${Object.keys(pin.theme_limits).length} mismatches=${bad}`);
}

console.log(failures === 0 ? 'RESULT: PASS' : `RESULT: FAIL (${failures})`);
process.exit(failures === 0 ? 0 : 1);
