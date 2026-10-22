#!/usr/bin/env python3
"""Gate static internal-DRAM growth between two ESP-IDF link maps.

    python3 tools/check_bss.py BASE NEW [--threshold 256] [--sections .dram0.bss .dram0.data]
    python3 tools/check_bss.py --write-baseline baseline.json build/obd_brz_gauge.map

BASE and NEW are GNU ld .map files (build/<project>.map) or a baseline JSON written with
--write-baseline. The check fails (exit 1) when the summed growth of the watched output
sections exceeds --threshold bytes (default 256, REFACTOR_PLAN.md P4 acceptance). The biggest
per-object changes are listed to point at the culprit. Exit 2 means a usage or parse error.
"""

import argparse
import json
import os
import re
import sys
from collections import Counter

DEFAULT_SECTIONS = ['.dram0.data', '.dram0.bss']
BASELINE_FORMAT = 'skygauge.dram-baseline/1'

OUT_SECTION = re.compile(r'^(\.\S+)(?:\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+))?')
ADDR_SIZE = re.compile(r'^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)(?:\s+(\S.*?))?\s*$')
IN_SECTION = re.compile(r'^ (\.\S+|COMMON|\*fill\*)(?:\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)(?:\s+(\S.*?))?)?\s*$')


class MapError(Exception):
    pass


def object_name(path):
    """'esp-idf/main/libmain.a(ui.c.obj)' -> 'libmain.a(ui.c.obj)'."""
    if not path:
        return '*fill*'
    m = re.match(r'^(.*?)(\([^()]*\))?$', path.strip())
    return os.path.basename(m.group(1)) + (m.group(2) or '')


def parse_map(path, sections):
    sizes = {}
    objects = {s: Counter() for s in sections}
    in_memory_map = False
    current = None
    pending_out = None
    pending_in = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if not in_memory_map:
                in_memory_map = line.startswith('Linker script and memory map')
                continue
            if pending_out is not None:
                m = ADDR_SIZE.match(line)
                if m:
                    sizes.setdefault(pending_out, int(m.group(2), 16))
                pending_out = None
                continue
            if line[:1] == '.':
                m = OUT_SECTION.match(line)
                current = m.group(1)
                pending_in = None
                if m.group(3):
                    sizes.setdefault(current, int(m.group(3), 16))
                else:
                    pending_out = current
                continue
            if line[:1] not in ('', ' ', '\t'):
                current = None
                pending_in = None
                continue
            if current not in objects:
                continue
            m = IN_SECTION.match(line)
            if m:
                pending_in = None
                if m.group(3):
                    objects[current][object_name(m.group(4)) if m.group(1) != '*fill*' else '*fill*'] += int(m.group(3), 16)
                else:
                    pending_in = m.group(1)
                continue
            if pending_in is not None:
                m = ADDR_SIZE.match(line)
                if m and m.group(3) is not None:
                    objects[current][object_name(m.group(3))] += int(m.group(2), 16)
                pending_in = None
    if not in_memory_map:
        raise MapError('%s: no "Linker script and memory map" section (not a GNU ld map?)' % path)
    missing = [s for s in sections if s not in sizes]
    if missing:
        raise MapError('%s: output section(s) not found: %s' % (path, ', '.join(missing)))
    return {'sizes': {s: sizes[s] for s in sections}, 'objects': {s: dict(objects[s]) for s in sections}}


def load(path, sections):
    if path.endswith('.json'):
        with open(path, encoding='utf-8') as f:
            doc = json.load(f)
        if doc.get('format') != BASELINE_FORMAT:
            raise MapError('%s: not a %s file' % (path, BASELINE_FORMAT))
        missing = [s for s in sections if s not in doc.get('sections', {})]
        if missing:
            raise MapError('%s: baseline lacks section(s) %s' % (path, ', '.join(missing)))
        return {'sizes': {s: doc['sections'][s] for s in sections},
                'objects': {s: doc.get('objects', {}).get(s, {}) for s in sections}}
    return parse_map(path, sections)


def write_baseline(out, map_path, sections):
    data = parse_map(map_path, sections)
    doc = {'format': BASELINE_FORMAT, 'map': os.path.basename(map_path),
           'sections': data['sizes'], 'objects': data['objects']}
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write('\n')
    for s in sections:
        print('%-14s %8d B' % (s, data['sizes'][s]))
    print('wrote %s' % out)


def compare(base, new, sections, threshold, top):
    total = 0
    print('%-14s %10s %10s %10s' % ('section', 'base', 'new', 'delta'))
    for s in sections:
        d = new['sizes'][s] - base['sizes'][s]
        total += d
        print('%-14s %10d %10d %+10d' % (s, base['sizes'][s], new['sizes'][s], d))
    print('%-14s %10s %10s %+10d   (threshold %d B)' % ('total', '', '', total, threshold))

    changes = []
    for s in sections:
        b, n = base['objects'].get(s, {}), new['objects'].get(s, {})
        for obj in set(b) | set(n):
            d = n.get(obj, 0) - b.get(obj, 0)
            if d:
                changes.append((d, s, obj))
    if changes and top > 0:
        changes.sort(key=lambda c: (-abs(c[0]), c[1], c[2]))
        print('largest per-object changes:')
        for d, s, obj in changes[:top]:
            print('  %+8d  %-12s %s' % (d, s, obj))
    if total > threshold:
        print('FAIL: static internal DRAM grew by %d B (> %d B). Move the new state to PSRAM '
              '(heap_caps_malloc(MALLOC_CAP_SPIRAM)) or justify a new baseline.' % (total, threshold))
        return 1
    print('PASS: static internal DRAM growth %+d B (<= %d B)' % (total, threshold))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base', nargs='?', help='baseline .map or baseline .json')
    ap.add_argument('new', nargs='?', help='new .map (or .json)')
    ap.add_argument('--threshold', type=int, default=256, help='allowed total growth in bytes (default 256)')
    ap.add_argument('--sections', nargs='+', default=DEFAULT_SECTIONS,
                    help='output sections to sum (default: %s)' % ' '.join(DEFAULT_SECTIONS))
    ap.add_argument('--top', type=int, default=10, help='per-object changes to list (default 10)')
    ap.add_argument('--write-baseline', metavar='OUT_JSON',
                    help='record the sizes of the single map given as BASE into OUT_JSON and exit')
    args = ap.parse_args()
    try:
        if args.write_baseline:
            if not args.base or args.new:
                ap.error('--write-baseline takes exactly one map file')
            write_baseline(args.write_baseline, args.base, args.sections)
            return 0
        if not args.base or not args.new:
            ap.error('need BASE and NEW')
        base = load(args.base, args.sections)
        new = load(args.new, args.sections)
    except (OSError, ValueError, MapError) as e:
        print('error: %s' % e)
        return 2
    return compare(base, new, args.sections, args.threshold, args.top)


if __name__ == '__main__':
    sys.exit(main())
