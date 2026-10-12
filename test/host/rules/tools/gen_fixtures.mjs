// Regenerates test/host/rules/fixtures/{extra_vectors,label_vectors}.json by
// running the editor's own code (obd_theme_store@5a7484e, read-only):
//   - frontend/src/lib/ruleEngine.ts   → expected results of the extra rule cases
//   - frontend/src/lib/themePacker.ts  → isIntOnlyFormat/formatIntPrintf for labels
// The golden vectors (contracts/fixtures/rules/vectors.json) are NOT produced here.
//
// Usage (Node >= 22.18 / 23.6, TypeScript type stripping):
//   node test/host/rules/tools/gen_fixtures.mjs [--editor <dir with ruleEngine.ts>] [--force]
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { stripTypeScriptTypes } from 'node:module';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '../../../..');
const argv = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : dflt;
};
const editorLib = resolve(opt('--editor', join(repo, '../obd_theme_store/frontend/src/lib')));
const force = argv.includes('--force');

const PINNED = 'd828878942274920978f19f6b4c531644f5e57bca6e6a32e8e15417303c5dcbc';
const engineSrc = readFileSync(join(editorLib, 'ruleEngine.ts'));
const sha = createHash('sha256').update(engineSrc).digest('hex');
if (sha !== PINNED && !force) {
  console.error(`ruleEngine.ts sha256 ${sha} != pinned ${PINNED} (5a7484e); use --force to accept`);
  process.exit(1);
}
const { evaluatePageRules, isRuleActive } = await import(pathToFileURL(join(editorLib, 'ruleEngine.ts')).href);

// themePacker.ts imports browser-only modules; lift the three formatter functions out verbatim.
const packer = readFileSync(join(editorLib, 'themePacker.ts'), 'utf8');
const start = packer.indexOf('export function isIntOnlyFormat');
const end = packer.indexOf('async function loadImageData');
if (start < 0 || end < 0) throw new Error('formatter functions not found in themePacker.ts');
const fmtJs = stripTypeScriptTypes(packer.slice(start, end)).replace(/^export /gm, '');
const { isIntOnlyFormat, formatIntPrintf } = new Function(`${fmtJs}\nreturn { isIntOnlyFormat, formatIntPrintf };`)();

// FabricCanvas.tsx createLabel() number path (no dataDisplay), as in the golden fmt.ts.
function labelText(format, divisor, raw) {
  const fmt = format && isIntOnlyFormat(format) ? format : '%d';
  if (divisor && divisor > 1) {
    const whole = Math.trunc(raw / divisor);
    const frac = Math.abs(raw % divisor);
    return formatIntPrintf(fmt, [whole, frac]);
  }
  return formatIntPrintf(fmt, [Math.trunc(raw)]);
}

// ---------------- extra rule cases ----------------
const ZERO = {
  'obd.rpm': 0, 'obd.speed': 0, 'obd.boost': 0, 'obd.coolant_temp': 0, 'obd.oil_pressure': 0,
  'obd.gear': 0, 'obd.battery_voltage': 0, 'obd.oil_temp': 0, 'obd.afr': 0, 'obd.throttle': 0, 'obd.intake_temp': 0,
};
const c = (source, op, value, extra = {}) => ({ source, op, value, ...extra });
const rule = (id, when, more = {}) => ({ id, when, apply: [], ...more });
const all = (...conditions) => ({ conditions });

const cases = [];
const ruleCase = (name, runtime, r, steps) => cases.push({ name, origin: 'extra-gen', api: 'isRuleActive', runtime, rule: r, steps });
const pageCase = (name, runtime, page, steps) => cases.push({ name, origin: 'extra-gen', api: 'evaluatePageRules', runtime, page: { id: 'p1', ...page }, steps });

ruleCase('X01_legacy_compare_source', 'none', rule('r1', { source: 'obd.rpm', op: '>', value: 0, compareSource: 'obd.speed' }),
  [{ values: { 'obd.rpm': 100, 'obd.speed': 50 } }, { values: { 'obd.rpm': 30, 'obd.speed': 50 } }, { values: { 'obd.rpm': 50, 'obd.speed': 50 } }]);
ruleCase('X02_legacy_value_null_relational', 'none', rule('r1', { source: 'obd.rpm', op: '>', value: null }),
  [{ values: { 'obd.rpm': 5 } }, { values: { 'obd.rpm': 0 } }, { values: { 'obd.rpm': -1 } }]);
ruleCase('X03a_legacy_value_null_eq', 'none', rule('r1', { source: 'obd.rpm', op: '==', value: null }), [{ values: { 'obd.rpm': 0 } }]);
ruleCase('X03b_legacy_value_null_ne', 'none', rule('r1', { source: 'obd.rpm', op: '!=', value: null }), [{ values: { 'obd.rpm': 0 } }]);
ruleCase('X04_legacy_missing_value', 'none', rule('r1', { source: 'obd.rpm', op: '>' }), [{ values: { 'obd.rpm': 5 } }]);
ruleCase('X05_legacy_empty_op', 'none', rule('r1', { source: 'obd.rpm', op: '', value: 1 }), [{ values: { 'obd.rpm': 5 } }]);
ruleCase('X06_unknown_op_with_band', 'shared', rule('r1', { conditions: [c('obd.rpm', '=>', 1)], hysteresis: 10 }),
  [{ values: { 'obd.rpm': 5 } }, { values: { 'obd.rpm': 0 } }, { values: { 'obd.rpm': 50 } }]);
ruleCase('X07a_unknown_source_missing_value_eq', 'none', rule('r1', all({ source: 'obd.fuel_level', op: '==' })), [{ values: {} }]);
ruleCase('X07b_unknown_source_missing_value_ne', 'none', rule('r1', all({ source: 'obd.fuel_level', op: '!=' })), [{ values: {} }]);
ruleCase('X07c_known_source_missing_value', 'none', rule('r1', { combine: 'any', conditions: [{ source: 'obd.rpm', op: '>=' }, { source: 'obd.rpm', op: '==' }] }),
  [{ values: { 'obd.rpm': 0 } }]);
ruleCase('X07d_known_source_missing_value_ne', 'none', rule('r1', all({ source: 'obd.rpm', op: '!=' })), [{ values: { 'obd.rpm': 0 } }]);
ruleCase('X08a_compare_source_unknown_rel', 'none', rule('r1', all(c('obd.rpm', '>', 0, { compareSource: 'obd.nope' }))), [{ values: { 'obd.rpm': 5 } }]);
ruleCase('X08b_compare_source_unknown_ne', 'none', rule('r1', all(c('obd.rpm', '!=', 5, { compareSource: 'obd.nope' }))), [{ values: { 'obd.rpm': 5 } }]);
ruleCase('X08c_unknown_vs_unknown_compare_eq', 'none', rule('r1', all(c('obd.nope', '==', 5, { compareSource: 'obd.nada' }))), [{ values: {} }]);
ruleCase('X09_compare_source_empty_string', 'none', rule('r1', all(c('obd.rpm', '>', 10, { compareSource: '' }))),
  [{ values: { 'obd.rpm': 20 } }, { values: { 'obd.rpm': 5 } }]);
ruleCase('X10a_value_bool_relational', 'none', rule('r1', all(c('obd.rpm', '>=', true))), [{ values: { 'obd.rpm': 1 } }, { values: { 'obd.rpm': 0 } }]);
ruleCase('X10b_value_bool_eq', 'none', rule('r1', all(c('obd.rpm', '==', true))), [{ values: { 'obd.rpm': 1 } }]);
ruleCase('X11a_value_numeric_string', 'none', rule('r1', all(c('obd.rpm', '>', '4000'))), [{ values: { 'obd.rpm': 5000 } }, { values: { 'obd.rpm': 3000 } }]);
ruleCase('X11b_value_numeric_string_eq', 'none', rule('r1', all(c('obd.rpm', '==', '5000'))), [{ values: { 'obd.rpm': 5000 } }]);
ruleCase('X11c_value_blank_string', 'none', rule('r1', all(c('obd.rpm', '>', ' '))), [{ values: { 'obd.rpm': 1 } }, { values: { 'obd.rpm': 0 } }]);
ruleCase('X11d_value_hex_string', 'none', rule('r1', all(c('obd.rpm', '>=', '0x10'))), [{ values: { 'obd.rpm': 16 } }, { values: { 'obd.rpm': 15 } }]);
ruleCase('X11e_value_garbage_string', 'none', rule('r1', { combine: 'any', conditions: [c('obd.rpm', '>', '12abc'), c('obd.rpm', '<', '12abc')] }), [{ values: { 'obd.rpm': 5 } }]);
ruleCase('X11f_value_object', 'none', rule('r1', { combine: 'any', conditions: [c('obd.rpm', '>', { a: 1 }), c('obd.rpm', '!=', { a: 1 })] }), [{ values: { 'obd.rpm': 5 } }]);

const hit = (id, more = {}) => ({ id, when: all(c('obd.rpm', '>=', 1)), apply: [], ...more });
pageCase('X12_enabled_only_false_disables', 'none', {
  elements: [],
  rules: [hit('s', { enabled: 'false' }), hit('n', { enabled: null }), hit('z', { enabled: 0 }), hit('f', { enabled: false }), hit('t', { enabled: true })],
}, [{ values: { 'obd.rpm': 5 } }]);
pageCase('X13_trigger_type_variants', 'none', {
  elements: [],
  rules: [hit('cap', { trigger: { type: 'Timer', intervalMs: 1000 } }), hit('nul', { trigger: null }), hit('str', { trigger: 'timer' }),
    hit('cond', { trigger: { type: 'condition', source: 'obd.speed' } }), hit('arr', { trigger: ['timer'] })],
}, [{ elapsedMs: 1000, values: { 'obd.rpm': 5 } }, { elapsedMs: 1000, values: { 'obd.rpm': 0 } }]);
ruleCase('X14_change_trigger_source_empty_string', 'none',
  rule('r1', { source: 'obd.rpm', op: '>', value: 0 }, { trigger: { type: 'change', source: '' } }),
  [{ changed: ['obd.rpm'] }, { changed: [] }]);
ruleCase('X15_change_trigger_null_source_uses_when', 'none',
  rule('r1', { source: 'obd.speed', op: '>', value: 0 }, { trigger: { type: 'change', source: null } }),
  [{ changed: ['obd.speed'] }, { changed: ['obd.rpm'] }]);
ruleCase('X15b_change_trigger_when_source_null_defaults_rpm', 'none',
  rule('r1', { source: null }, { trigger: { type: 'change' } }), [{ changed: ['obd.rpm'] }, { changed: ['obd.speed'] }]);
ruleCase('X15c_change_trigger_numeric_source', 'none',
  rule('r1', {}, { trigger: { type: 'change', source: 5 } }), [{ changed: ['obd.rpm'] }]);
ruleCase('X15d_change_ignores_enabled_true_and_conditions', 'none',
  rule('r1', all(c('obd.rpm', '>', 100000)), { enabled: true, trigger: { type: 'change', source: 'obd.gear' } }),
  [{ changed: ['obd.gear', 'obd.rpm'], values: { 'obd.rpm': 5 } }]);

const tmr = (id, t) => ({ id, trigger: { type: 'timer', ...t }, when: {}, apply: [] });
pageCase('X16_timer_interval_variants', 'none', {
  elements: [],
  rules: [tmr('t_null', { intervalMs: null }), tmr('t_zero', { intervalMs: 0 }), tmr('t_250', { intervalMs: 250 }),
    tmr('t_neg', { intervalMs: -5 }), tmr('t_str', { intervalMs: '500' }), tmr('t_frac', { intervalMs: 333.3 })],
}, [50, 260, 370, 510, 1000.5, 1119.9, 1120, 666.6, 1e12 + 50, 123456789.5, -100, 0].map((e) => ({ elapsedMs: e })));
ruleCase('X17_combine_is_case_sensitive', 'none', rule('r1', { combine: 'ANY', conditions: [c('obd.rpm', '>', 1), c('obd.speed', '>', 1)] }),
  [{ values: { 'obd.rpm': 5, 'obd.speed': 0 } }, { values: { 'obd.rpm': 5, 'obd.speed': 5 } }]);
ruleCase('X18_hysteresis_null_is_raw', 'shared', rule('r1', { conditions: [c('obd.rpm', '>=', 4000)], hysteresis: null }),
  [{ values: { 'obd.rpm': 5000 } }, { values: { 'obd.rpm': 3999 } }]);
ruleCase('X18b_hysteresis_bool_true_is_one', 'shared', rule('r1', { conditions: [c('obd.rpm', '>=', 10)], hysteresis: true }),
  [11, 9.5, 8.9, 10.5, 11.5].map((v) => ({ values: { 'obd.rpm': v } })));
ruleCase('X18c_hysteresis_garbage_freezes_latch', 'shared', rule('r1', { conditions: [c('obd.rpm', '>=', 10)], hysteresis: 'abc' }),
  [20, 0, 20, 0].map((v) => ({ values: { 'obd.rpm': v } })));
pageCase('X19_debounce_negative_null_string', 'shared', {
  elements: [],
  rules: [hit('neg', { debounceMs: -100 }), hit('nul', { debounceMs: null }), hit('str', { debounceMs: '100' }), hit('bad', { debounceMs: 'x' })],
}, [0, 50, 100, 5000].map((e) => ({ elapsedMs: e, values: { 'obd.rpm': 5 } })));
pageCase('X20_debounce_elapsed_backwards', 'shared', { elements: [], rules: [hit('r1', { debounceMs: 100 })] },
  [500, 300, 599, 600].map((e) => ({ elapsedMs: e, values: { 'obd.rpm': 5 } })));

const prioPage = (prios) => ({
  elements: [{ id: 'lbl1', kind: 'label' }],
  rules: prios.map((p, i) => ({
    id: `r${i}`, ...(p === undefined ? {} : { priority: p }), when: all(c('obd.rpm', '>=', 1)),
    apply: [{ target: 'lbl1', set: { color: `#${String(i).padStart(2, '0')}0000` } }],
  })),
});
pageCase('X21a_priority_mixed', 'none', prioPage([3, -1, 2.5, null, 3, 0, -1, undefined]), [{ values: { 'obd.rpm': 5 } }]);
pageCase('X21b_priority_descending_run', 'none', prioPage([5, 4, 3, 2, 1]), [{ values: { 'obd.rpm': 5 } }]);
pageCase('X21c_priority_desc_then_equal', 'none', prioPage([5, 4, 4, 3, 9, 0]), [{ values: { 'obd.rpm': 5 } }]);
pageCase('X21d_priority_sixteen', 'none', prioPage([7, 3, 3, 9, -2, 0, 5, 5, 1, 8, -2, 4, 6, 0, 2, 3]), [{ values: { 'obd.rpm': 5 } }]);
pageCase('X21e_priority_strings_bools', 'none', prioPage(['5', 4, true, '-1', false]), [{ values: { 'obd.rpm': 5 } }]);
pageCase('X21f_priority_nan_v8_order', 'none', prioPage([2, 'abc', 1, 0, 'x', -1]), [{ values: { 'obd.rpm': 5 } }]);
pageCase('X21g_priority_nan_first', 'none', prioPage(['abc', 3, 2, 1]), [{ values: { 'obd.rpm': 5 } }]);
pageCase('X21h_priority_subset_active', 'none', {
  elements: [],
  rules: [hit('a', { priority: 2 }), { id: 'b', priority: 1, when: all(c('obd.speed', '>', 0)), apply: [] }, hit('c', { priority: 1 }), hit('d', { priority: 0 })],
}, [{ values: { 'obd.rpm': 5 } }, { values: { 'obd.rpm': 5, 'obd.speed': 1 } }]);

pageCase('X23_set_multiple_keys', 'none', {
  elements: [{ id: 'lbl1', kind: 'label' }, { id: 'img1', kind: 'image' }, { id: 'bar1', kind: 'bar' }],
  rules: [{
    id: 'r1', when: all(c('obd.rpm', '>=', 1)),
    apply: [
      { target: 'lbl1', set: { asset: 'a1', visible: false, text: 'HOT', color: '#ABCDEF' } },
      { target: 'img1', set: { visible: true, color: '0x00FF00', asset: 'warn' } },
      { target: 'bar1', set: { color: 'ff3158', text: 'ignored by bars' } },
    ],
  }],
}, [{ values: { 'obd.rpm': 5 } }, { values: { 'obd.rpm': 0 } }]);
pageCase('X24_empty_set_touches_target', 'none', {
  elements: [{ id: 'lbl1', kind: 'label' }],
  rules: [{ id: 'r1', when: all(c('obd.rpm', '>=', 1)), apply: [{ target: 'lbl1', set: {} }, { target: 'ghost', set: { foo: 1 } }] }],
}, [{ values: { 'obd.rpm': 5 } }]);
pageCase('X25_unknown_kind_and_duplicate_element_ids', 'none', {
  elements: [{ id: 'e1', kind: 'text' }, { id: 'e2', kind: 'arc' }, { id: 'e2', kind: 'image' }, { id: 'e3', kind: 'pointer' }],
  rules: [{ id: 'r1', when: all(c('obd.rpm', '>=', 1)), apply: ['e1', 'e2', 'e3'].map((t) => ({ target: t, set: { color: '#123456' } })) }],
}, [{ values: { 'obd.rpm': 5 } }]);
ruleCase('X26_ne_ignores_band', 'shared', rule('r1', { conditions: [c('obd.gear', '!=', 3)], hysteresis: 5 }),
  [3, 4, 3, 8, 3].map((v) => ({ values: { 'obd.gear': v } })));
ruleCase('X27_ge_band_edges_strict', 'shared', rule('r1', { conditions: [c('obd.rpm', '>=', 4000)], hysteresis: 100 }),
  [4100, 3900, 3899, 4100, 4101].map((v) => ({ values: { 'obd.rpm': v } })));
ruleCase('X28_le_band_edges_strict', 'shared', rule('r1', { conditions: [c('obd.rpm', '<=', 1000)], hysteresis: 100 }),
  [1100, 999, 899, 1100, 1101].map((v) => ({ values: { 'obd.rpm': v } })));
ruleCase('X29_gt_band_first_raw_false', 'shared', rule('r1', { conditions: [c('obd.rpm', '>', 4000)], hysteresis: 50 }),
  [4000, 4040, 4051, 3960, 3949].map((v) => ({ values: { 'obd.rpm': v } })));
ruleCase('X29b_fractional_band', 'shared', rule('r1', { conditions: [c('obd.boost', '>', 0.3)], hysteresis: 0.1 }),
  [0.5, 0.2, 0.19999999999999998, 0.4, 0.4000000000000001].map((v) => ({ values: { 'obd.boost': v } })));
pageCase('X30_timer_with_debounce', 'shared', {
  elements: [], rules: [{ id: 't', trigger: { type: 'timer', intervalMs: 1000 }, debounceMs: 50, when: {}, apply: [] }],
}, [1000, 1060, 1130, 2000, 2050, 2119].map((e) => ({ elapsedMs: e })));
pageCase('X31_change_with_debounce', 'shared', {
  elements: [], rules: [{ id: 'ch', trigger: { type: 'change', source: 'obd.gear' }, debounceMs: 100, when: {}, apply: [] }],
}, [[0, ['obd.gear']], [80, ['obd.gear']], [100, ['obd.gear']], [150, []], [200, ['obd.gear']], [300, ['obd.gear']]]
  .map(([e, ch]) => ({ elapsedMs: e, changed: ch })));
pageCase('X32_latch_survives_debounce_reset', 'shared', {
  elements: [], rules: [{ id: 'r1', debounceMs: 100, when: { conditions: [c('obd.rpm', '>=', 4000), c('obd.speed', '>', 10)], hysteresis: 200 }, apply: [] }],
}, [[0, 5000, 20], [50, 3900, 20], [60, 3900, 0], [70, 3900, 20], [170, 3900, 20], [180, 3700, 20]]
  .map(([e, r, s]) => ({ elapsedMs: e, values: { 'obd.rpm': r, 'obd.speed': s } })));
pageCase('X33_duplicate_ids_share_debounce', 'shared', {
  elements: [], rules: [hit('x', { debounceMs: 100 }), { id: 'x', when: all(c('obd.speed', '>=', 1)), apply: [] }],
}, [[0, 0], [100, 0], [200, 5], [300, 5], [350, 0], [400, 5]].map(([e, s]) => ({ elapsedMs: e, values: { 'obd.rpm': 5, 'obd.speed': s } })));
pageCase('X34_duplicate_ids_share_latch', 'shared', {
  elements: [], rules: [
    { id: 'x', when: { conditions: [c('obd.rpm', '>=', 1000)], hysteresis: 100 }, apply: [] },
    { id: 'x', when: { conditions: [c('obd.rpm', '<=', 500)], hysteresis: 100 }, apply: [] },
  ],
}, [2000, 700, 300, 1200, 950, 350].map((v) => ({ values: { 'obd.rpm': v } })));
ruleCase('X36_float_compare_exact', 'none', rule('r1', { combine: 'any', conditions: [c('obd.boost', '==', 0.3)] }),
  [{ values: { 'obd.boost': 0.30000000000000004 } }, { values: { 'obd.boost': 0.3 } }]);
ruleCase('X36b_float_compare_eq_same', 'none', rule('r1', all(c('obd.boost', '==', 0.30000000000000004))),
  [{ values: { 'obd.boost': 0.30000000000000004 } }]);
pageCase('X37_override_merge_across_rules', 'none', {
  elements: [{ id: 'arc1', kind: 'arc' }, { id: 'ptr1', kind: 'pointer' }],
  rules: [
    { id: 'a', priority: 0, when: all(c('obd.rpm', '>=', 1)), apply: [{ target: 'arc1', set: { color: '#111111', visible: false } }, { target: 'ptr1', set: { asset: 'n1' } }] },
    { id: 'b', priority: 1, when: all(c('obd.rpm', '>=', 1)), apply: [{ target: 'arc1', set: { visible: true } }, { target: 'ghost', set: { text: 'T' } }] },
    { id: 'c', priority: 1, when: all(c('obd.rpm', '>=', 100)), apply: [{ target: 'ptr1', set: { color: '#222222', asset: 'n2' } }] },
  ],
}, [{ values: { 'obd.rpm': 5 } }, { values: { 'obd.rpm': 500 } }]);
ruleCase('X38a_invalid_relational_false', 'none', rule('r1', { combine: 'any', conditions: [c('obd.rpm', '>', -1), c('obd.rpm', '<=', 1e9), c('obd.rpm', '==', 0)] }),
  [{ values: {}, invalid: ['obd.rpm'] }]);
ruleCase('X38b_invalid_ne_true', 'none', rule('r1', all(c('obd.rpm', '!=', 0))), [{ values: {}, invalid: ['obd.rpm'] }]);
ruleCase('X38c_invalid_compare_source', 'none', rule('r1', { combine: 'any', conditions: [c('obd.rpm', '<', 0, { compareSource: 'obd.speed' }), c('obd.rpm', '>=', 0, { compareSource: 'obd.speed' })] }),
  [{ values: { 'obd.rpm': 5 }, invalid: ['obd.speed'] }]);
ruleCase('X38d_invalid_ne_compare_source', 'none', rule('r1', all(c('obd.rpm', '!=', 0, { compareSource: 'obd.speed' }))),
  [{ values: { 'obd.rpm': 5 }, invalid: ['obd.rpm', 'obd.speed'] }]);
ruleCase('X39_invalid_holds_directional_latch', 'shared', rule('r1', { conditions: [c('obd.rpm', '>=', 4000)], hysteresis: 100 }),
  [{ values: { 'obd.rpm': 5000 } }, { values: {}, invalid: ['obd.rpm'] }, { values: { 'obd.rpm': 3000 } }, { values: {}, invalid: ['obd.rpm'] }, { values: { 'obd.rpm': 4050 } }]);
ruleCase('X40_invalid_releases_eq_latch', 'shared', rule('r1', { conditions: [c('obd.gear', '==', 3)], hysteresis: 1 }),
  [{ values: { 'obd.gear': 3 } }, { values: {}, invalid: ['obd.gear'] }, { values: { 'obd.gear': 3 } }]);
ruleCase('X41_invalid_first_eval_sets_latch_false', 'shared', rule('r1', { conditions: [c('obd.rpm', '<', 4000)], hysteresis: 100 }),
  [{ values: {}, invalid: ['obd.rpm'] }, { values: { 'obd.rpm': 3950 } }, { values: { 'obd.rpm': 3899 } }]);
ruleCase('X45a_conditions_object_uses_legacy', 'none', rule('r1', { conditions: {}, source: 'obd.rpm', op: '>', value: 1 }), [{ values: { 'obd.rpm': 5 } }]);
ruleCase('X45b_conditions_null_uses_legacy', 'none', rule('r1', { conditions: null, source: 'obd.rpm', op: '>', value: 1 }), [{ values: { 'obd.rpm': 5 } }]);
ruleCase('X46_condition_index_latches_independent', 'shared',
  rule('r1', { combine: 'any', conditions: [c('obd.rpm', '>=', 1000), c('obd.rpm', '<=', 500), c('obd.speed', '>', 50), c('obd.gear', '==', 2)], hysteresis: 100 }),
  [[2000, 0, 0], [950, 0, 0], [800, 0, 0], [450, 0, 0], [550, 60, 0], [800, 0, 2], [800, 0, 3]]
    .map(([r, s, g]) => ({ values: { 'obd.rpm': r, 'obd.speed': s, 'obd.gear': g } })));
pageCase('X47_disabled_rule_clears_debounce_keeps_latch', 'shared', {
  elements: [], rules: [{ id: 'r1', debounceMs: 50, when: { conditions: [c('obd.rpm', '>=', 4000)], hysteresis: 100 }, apply: [] }],
}, [[0, 5000], [60, 5000], [70, 3950], [200, 3950], [210, 3000], [220, 4050], [400, 4200]]
  .map(([e, r]) => ({ elapsedMs: e, values: { 'obd.rpm': r } })));
pageCase('X48_duplicate_target_ids_across_rules', 'shared', {
  elements: [{ id: 'lbl1', kind: 'label' }],
  rules: [
    { id: 'a', when: all(c('obd.rpm', '>=', 1)), apply: [{ target: 'lbl1', set: { text: 'A' } }, { target: 'lbl1', set: { color: '#010101' } }] },
    { id: 'b', debounceMs: 100, when: all(c('obd.rpm', '>=', 1)), apply: [{ target: 'lbl1', set: { text: 'B' } }] },
  ],
}, [0, 50, 100].map((e) => ({ elapsedMs: e, values: { 'obd.rpm': 5 } })));

for (const vec of cases) {
  const shared = new Map();
  for (const step of vec.steps) {
    const values = { ...ZERO, ...(step.values ?? {}) };
    for (const k of step.invalid ?? []) values[k] = NaN;
    const elapsed = step.elapsedMs ?? 0;
    const changed = new Set(step.changed ?? []);
    const rt = vec.runtime === 'shared' ? shared : undefined;
    if (vec.api === 'isRuleActive') {
      step.expect = { active: Boolean(isRuleActive(vec.rule, values, elapsed, changed, rt)) };
    } else {
      const r = evaluatePageRules(vec.page, values, elapsed, changed, rt);
      step.expect = { activeRules: r.activeRules.map((x) => x.id), overrides: r.overrides };
    }
  }
}
const nSteps = cases.reduce((n, v) => n + v.steps.length, 0);
writeFileSync(join(here, '../fixtures/extra_vectors.json'), `${JSON.stringify(cases, null, 1)}\n`);
console.log(`extra_vectors.json: ${cases.length} groups, ${nSteps} steps (ruleEngine.ts sha256 ${sha.slice(0, 12)})`);

// ---------------- label format cases ----------------
const golden = [
  ['%d', undefined, 1234], ['%d.%02d', 100, 150], ['%d.%02d', 100, 105], ['%d.%d', 10, 136],
  ['%d.%02d', 100, -150], ['%d.%02d', 100, -50], ['%5d', undefined, 42], ['%-5d|', undefined, 42],
  ['%05d', undefined, -42], ['%+d', undefined, 5], ['%x', undefined, 255], ['%X', undefined, -1],
  ['%u', undefined, -5], ['% d', undefined, 5], ['%d %d', undefined, 7], ['%c', undefined, 65],
  ['%d%%', undefined, 65], ['%s', undefined, 3], ['%.2d', undefined, 5], ['%d km/h', undefined, 88],
  ['%o', undefined, 8], ['%d.%02d', 100, 0], [undefined, undefined, 3500], ['%+05d', undefined, 7],
];
const extra = [
  ['%5.2d', undefined, 5], ['%c', undefined, 0x4e2d], ['%c', undefined, 233], ['%-+5d|', undefined, 7], ['% 5d', undefined, 42],
  ['abc%', undefined, 1], ['%x', undefined, -255], ['%o', undefined, -8], ['0x%04X', undefined, 4096], ['%X', undefined, 3000000000],
  ['%d', undefined, 1e15], ['%d', undefined, 2 ** 53 + 2], ['%d', undefined, 1e21], ['%d', undefined, 1.5e300], ['%d', undefined, 2 ** 60],
  ['%d', undefined, -1e22], ['%d', undefined, -0.5], ['%d.%02d', 10, 7], ['%d.%d', 2.5, 7], ['%d.%02d', 100, 150.7],
  ['%d.%x', 100, 150.5], ['%d.%o', 10, 13.25], ['%d %x', 10, 0.1], ['%d|%x', 7, 1.0000001], ['%d|%X', 3, 2.9999999],
  ['%d|%o', 1e9, 1e-7], ['%5c', undefined, 65], ['%-3c|', undefined, 0x4e2d], ['%05c', undefined, 45], ['%+i', undefined, 5],
  ['%+u', undefined, 5], ['%+x', undefined, 5], ['%%d', undefined, 5], ['%d%', undefined, 5], ['%3%', undefined, 1],
  ['%d°C', undefined, -5], ['', undefined, 12], ['%i', undefined, -7.9], ['%u', 100, -150], ['%d.%02d', 100, -5],
  ['%x', undefined, 4294967296], ['%X', undefined, -4294967297], ['%o', undefined, 1e20], ['%x', undefined, 1e20], ['%x', undefined, 2 ** 70],
  ['%30d|', undefined, 1], ['%-08d|', undefined, -3], ['%0-8d|', undefined, 3], ['%d %d %d', 10, 123], ['%#x', undefined, 255],
  ['%ld', undefined, 5], ['%c%c|', 100, 6566], ['%c', undefined, -1], ['%c', undefined, 65601], ['%d', 1, 7.9],
  ['%d', 0.5, 7.9], ['%d.%d', 1.5, -4], ['%X|%x', 1e3, 255999], ['%d', undefined, 9007199254740991], ['%d', undefined, -9007199254740993],
];
const labels = [...golden, ...extra].map(([format, divisor, raw]) => ({
  format: format ?? null, divisor: divisor ?? null, raw, text: labelText(format, divisor, raw),
}));
writeFileSync(join(here, '../fixtures/label_vectors.json'), `${JSON.stringify(labels, null, 1)}\n`);
console.log(`label_vectors.json: ${labels.length} cases (${golden.length} golden from fmt.ts + ${extra.length} extra)`);
