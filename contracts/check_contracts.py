#!/usr/bin/env python3
"""Consistency checks for contracts/ (signal registry, vehicle profile schema, fixtures).

Usage:
    python3 contracts/check_contracts.py                     # registry + schema sync + fixtures
    python3 contracts/check_contracts.py --profiles DIR|FILE  # also validate vehicle profiles

Profile validation needs the 'jsonschema' package (pip install jsonschema). The registry and
fixture checks use only the standard library. Exit status is non-zero on any error.
"""

import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Frozen by the editor: obd_theme_store gaugeModel.ts:17-29 (order matters).
EDITOR_KEYS = [
    'obd.rpm', 'obd.speed', 'obd.boost', 'obd.coolant_temp', 'obd.oil_pressure', 'obd.gear',
    'obd.battery_voltage', 'obd.oil_temp', 'obd.afr', 'obd.throttle', 'obd.intake_temp',
]
DELIVERY_OPS = {'identity', 'mul', 'div'}
ROUNDING = {'half_up', 'floor', 'exact'}
KEY_RE = re.compile(r'^[a-z][a-z0-9]*(\.[a-z0-9_]+)+$')
ID_RE = re.compile(r'^0x[0-9A-F]{4}$')


class Report:
    def __init__(self):
        self.errors = []

    def error(self, msg):
        self.errors.append(msg)
        print('ERROR ' + msg)


def load_json(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def check_conversion(rep, where, conv):
    if conv is None:
        return
    op = conv.get('op')
    if op not in DELIVERY_OPS:
        rep.error('%s: unknown op %r' % (where, op))
        return
    if op != 'identity':
        if not isinstance(conv.get('by'), (int, float)) or conv['by'] <= 0:
            rep.error('%s: "by" must be a positive number' % where)
        if conv.get('round') not in ROUNDING:
            rep.error('%s: round must be one of %s' % (where, sorted(ROUNDING)))


def check_signals(rep, reg):
    if reg.get('format') != 'skygauge.signals/1':
        rep.error('signals.json: format must be skygauge.signals/1')
    ranges = []
    for r in reg.get('ranges', []):
        first, last = int(r['first'], 16), int(r['last'], 16)
        if first > last:
            rep.error('range %s: first > last' % r['prefix'])
        for other in ranges:
            if first <= other[2] and other[1] <= last:
                rep.error('range %s overlaps %s' % (r['prefix'], other[0]))
        ranges.append((r['prefix'], first, last, r['allocation']))

    ids, keys = set(), set()
    signals = reg.get('signals', [])
    for s in signals:
        key, sid = s.get('key', ''), s.get('id', '')
        where = 'signal %s' % key
        if not KEY_RE.match(key):
            rep.error('%s: bad key' % where)
        if not ID_RE.match(sid):
            rep.error('%s: id %r must be a 4-digit upper-case hex string' % (where, sid))
            continue
        if sid in ids:
            rep.error('%s: duplicate id %s' % (where, sid))
        if key in keys:
            rep.error('%s: duplicate key' % where)
        ids.add(sid)
        keys.add(key)
        num = int(sid, 16)
        rng = [r for r in ranges if key.startswith(r[0])]
        if len(rng) != 1:
            rep.error('%s: key prefix matches %d ranges' % (where, len(rng)))
        else:
            prefix, first, last, alloc = rng[0]
            if alloc != 'static':
                rep.error('%s: range %s is %s, it must not have registry entries' % (where, prefix, alloc))
            if not first <= num <= last:
                rep.error('%s: id %s outside range %s' % (where, sid, prefix))
        internal = s.get('internal', {})
        if not (isinstance(internal.get('scale'), (int, float)) and internal['scale'] > 0):
            rep.error('%s: internal.scale must be > 0' % where)
        if not (isinstance(internal.get('min'), int) and isinstance(internal.get('max'), int)
                and internal['min'] <= internal['max']):
            rep.error('%s: internal.min/max must be integers with min <= max' % where)
        if not isinstance(s.get('stale_ms'), int) or s['stale_ms'] < 0:
            rep.error('%s: stale_ms must be an integer >= 0' % where)
        units = s.get('profile_units')
        if key.startswith('obd.'):
            if not units:
                rep.error('%s: obd.* signals need profile_units' % where)
            elif s.get('profile_unit_default') not in units:
                rep.error('%s: profile_unit_default not in profile_units' % where)
            elif any(not isinstance(v, (int, float)) or v <= 0 for v in units.values()):
                rep.error('%s: profile_units factors must be > 0' % where)
        elif units is not None:
            rep.error('%s: only obd.* signals take profile_units' % where)
        check_conversion(rep, where + ' delivery', s.get('delivery'))
        for name, leg in (s.get('legacy') or {}).items():
            if isinstance(leg, dict):
                check_conversion(rep, '%s legacy.%s.to_internal' % (where, name), leg.get('to_internal'))
                check_conversion(rep, '%s legacy.%s.from_internal' % (where, name), leg.get('from_internal'))

    editor = sorted((s for s in signals if s.get('editor')), key=lambda s: s['editor']['index'])
    got = [s['key'] for s in editor]
    if got != EDITOR_KEYS:
        rep.error('editor keys/order %s != frozen editor list %s' % (got, EDITOR_KEYS))
    if [s['editor']['index'] for s in editor] != list(range(len(EDITOR_KEYS))):
        rep.error('editor indices must be 0..%d' % (len(EDITOR_KEYS) - 1))
    for r in reg.get('retired', []):
        if r['former_key'] in keys:
            rep.error('retired key %s is still registered' % r['former_key'])
    return [s for s in signals if s['key'].startswith('obd.')]


def check_schema_sync(rep, schema, obd_signals):
    sig = schema['$defs']['signal']
    enum = sig['properties']['id']['anyOf'][0]['enum']
    want = [s['key'] for s in obd_signals]
    if sorted(enum) != sorted(want):
        rep.error('schema signal id enum %s != registry obd keys %s' % (sorted(enum), sorted(want)))
    unit_rules = {}
    for item in sig.get('allOf', []):
        try:
            unit_rules[item['if']['properties']['id']['const']] = item['then']['properties']['unit']['enum']
        except (KeyError, TypeError):
            continue
    for s in obd_signals:
        units = unit_rules.get(s['key'])
        if units is None:
            rep.error('schema has no unit rule for %s' % s['key'])
        elif sorted(units) != sorted(s['profile_units']):
            rep.error('schema units for %s %s != registry %s' % (s['key'], units, sorted(s['profile_units'])))
    for key in unit_rules:
        if key not in want:
            rep.error('schema unit rule for unknown signal %s' % key)


def check_fixtures(rep, root):
    vec = load_json(os.path.join(root, 'fixtures', 'rules', 'vectors.json'))
    names = set()
    steps = 0
    for v in vec:
        if v.get('name') in names:
            rep.error('rules vector %s: duplicate name' % v.get('name'))
        names.add(v.get('name'))
        if v.get('api') not in ('isRuleActive', 'evaluatePageRules'):
            rep.error('rules vector %s: bad api' % v.get('name'))
        if v.get('runtime') not in ('none', 'shared'):
            rep.error('rules vector %s: bad runtime' % v.get('name'))
        if not v.get('steps'):
            rep.error('rules vector %s: no steps' % v.get('name'))
        steps += len(v.get('steps', []))
    fmt = load_json(os.path.join(root, 'fixtures', 'format', 'label_format.json'))
    for c in fmt.get('cases', []):
        if not all(k in c for k in ('name', 'format', 'divisor', 'raw', 'text')):
            rep.error('format case %s: missing fields' % c.get('name'))
    pin = load_json(os.path.join(root, 'fixtures', 'editor-pin.json'))
    if not re.match(r'^[0-9a-f]{40}$', pin.get('commit', '')):
        rep.error('editor-pin.json: commit must be a full 40-hex sha')
    print('fixtures: rules vectors=%d steps=%d, format cases=%d, editor pin %s'
          % (len(vec), steps, len(fmt.get('cases', [])), pin.get('commit', '')[:7]))


def collect_profiles(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(glob.glob(os.path.join(p, '*.json')))
        elif any(ch in p for ch in '*?['):
            files += sorted(glob.glob(p))
        else:
            files.append(p)
    return files


def check_profiles(rep, schema, paths):
    try:
        import jsonschema
    except ImportError:
        rep.error('profile validation needs the jsonschema package (pip install jsonschema)')
        return
    validator = jsonschema.Draft202012Validator(schema)
    files = collect_profiles(paths)
    for path in files:
        try:
            doc = load_json(path)
        except (OSError, ValueError) as e:
            rep.error('%s: %s' % (path, e))
            continue
        errs = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
        for e in errs[:20]:
            rep.error('%s: /%s: %s' % (path, '/'.join(str(x) for x in e.absolute_path), e.message[:300]))
        if not errs:
            ids = [s.get('id') for s in doc.get('signals', [])]
            dup = sorted({i for i in ids if ids.count(i) > 1})
            if dup:
                rep.error('%s: duplicate signal ids %s' % (path, dup))
            if sum(1 for i in ids if str(i).startswith('veh.')) > 32:
                rep.error('%s: more than 32 veh.* signals' % path)
    print('profiles: %d file(s) checked' % len(files))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--profiles', nargs='*', default=[],
                    help='vehicle profile files, directories or globs to validate against the schema')
    args = ap.parse_args()

    rep = Report()
    reg = load_json(os.path.join(HERE, 'signals.json'))
    schema = load_json(os.path.join(HERE, 'vehicle-profile.schema.json'))
    obd = check_signals(rep, reg)
    print('signals: %d entries (%d obd.*)' % (len(reg.get('signals', [])), len(obd)))
    check_schema_sync(rep, schema, obd)
    try:
        import jsonschema
        jsonschema.Draft202012Validator.check_schema(schema)
        print('schema: valid JSON Schema draft 2020-12')
    except ImportError:
        print('schema: meta-validation skipped (jsonschema not installed)')
    except Exception as e:  # jsonschema.SchemaError
        rep.error('vehicle-profile.schema.json is not a valid draft 2020-12 schema: %s' % e)
    check_fixtures(rep, HERE)
    if args.profiles:
        check_profiles(rep, schema, args.profiles)
    if rep.errors:
        print('check_contracts: %d error(s)' % len(rep.errors))
        return 1
    print('check_contracts: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
