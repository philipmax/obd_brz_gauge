#!/usr/bin/env python3
"""Generate the partition layout description from partitions.csv.

    python3 tools/gen_layout.py                      # write build/partition_layout.{json,h}
    python3 tools/gen_layout.py --check              # verify docs/FLASH.md + docs/FLASH.en.md, write nothing
    python3 tools/gen_layout.py --csv X.csv --expect-crc 0x94752aa3

PARTITION_LAYOUT_CRC is the CRC32 (zlib) over the binary 32-byte partition entries exactly as
ESP-IDF gen_esp32part.py encodes them ('<2sBBLL16sL': magic AA 50, type, subtype, offset, size,
16-byte NUL-padded label, flags), in CSV order, without the MD5 entry and the 0xFF padding.

The CSV parser is self-contained (same rules as gen_esp32part.py v5.5: keyword types/subtypes,
k/m suffixes, empty offsets auto-aligned, negative size = end address, flags encrypted/readonly).
When gen_esp32part.py is found ($IDF_PATH or ~/.espressif) the encoding is cross-checked
against it; pass --no-idf to skip.
"""

import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAGIC = b'\xaa\x50'
ENTRY_FMT = '<2sBBLL16sL'
TABLE_OFFSET = 0x8000
TABLE_SIZE = 0x1000
TYPES = {'bootloader': 0x02, 'partition_table': 0x03, 'app': 0x00, 'data': 0x01}
SUBTYPES = {
    0x00: dict({'factory': 0x00, 'test': 0x20},
               **{'ota_%d' % i: 0x10 + i for i in range(16)},
               **{'tee_%d' % i: 0x30 + i for i in range(2)}),
    0x01: {'ota': 0x00, 'phy': 0x01, 'nvs': 0x02, 'coredump': 0x03, 'nvs_keys': 0x04, 'efuse': 0x05,
           'undefined': 0x06, 'esphttpd': 0x80, 'fat': 0x81, 'spiffs': 0x82, 'littlefs': 0x83, 'tee_ota': 0x90},
}
OFFSET_ALIGN = {0x00: 0x10000}  # app; everything else 0x1000
FLAGS = {'encrypted': 0, 'readonly': 1}

# docs: which flashed file must sit at which partition's offset
DOC_FILE_TO_PART = [
    (re.compile(r'obd_brz_gauge\.bin$'), 'ota_0'),
    (re.compile(r'ota_data_initial\.bin$'), 'otadata'),
    (re.compile(r'bootmedia\.bin$'), 'bootmedia'),
    (re.compile(r'theme[^/\s]*\.bin$'), 'theme_0'),
]


class LayoutError(Exception):
    pass


def parse_int(text, keywords=None):
    v = text.strip()
    for letter, mult in (('k', 1024), ('m', 1024 * 1024)):
        if v.lower().endswith(letter):
            return parse_int(v[:-1], keywords) * mult
    try:
        return int(v, 0)
    except ValueError:
        if keywords and v.lower() in keywords:
            return keywords[v.lower()]
        raise LayoutError('invalid value %r' % text)


def parse_csv(text):
    parts = []
    for line_no, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        f = [x.strip() for x in (line + ',,,,').split(',')]
        try:
            if not f[1]:
                raise LayoutError("type can't be empty")
            ptype = parse_int(f[1], TYPES)
            if f[2]:
                subtype = parse_int(f[2], SUBTYPES.get(ptype, {}))
            elif ptype == TYPES['app']:
                raise LayoutError('app partition needs a subtype')
            else:
                subtype = SUBTYPES[0x01]['undefined']
            offset = parse_int(f[3]) if f[3] else None
            if not f[4]:
                raise LayoutError("size can't be empty")
            size = parse_int(f[4])
            flags = 0
            for flag in f[5].split(':'):
                if flag in FLAGS:
                    flags |= 1 << FLAGS[flag]
                elif flag:
                    raise LayoutError('unknown flag %r' % flag)
        except LayoutError as e:
            raise LayoutError('line %d: %s' % (line_no, e))
        parts.append({'name': f[0], 'type': ptype, 'subtype': subtype, 'offset': offset,
                      'size': size, 'flags': flags, 'line': line_no})
    last_end = TABLE_OFFSET + TABLE_SIZE
    for p in parts:
        if p['offset'] is not None and p['offset'] < last_end:
            raise LayoutError('line %d: %s at 0x%x overlaps the previous partition (ends 0x%x)'
                              % (p['line'], p['name'], p['offset'], last_end))
        if p['offset'] is None:
            align = OFFSET_ALIGN.get(p['type'], 0x1000)
            p['offset'] = (last_end + align - 1) // align * align
        if p['size'] < 0:
            p['size'] = -p['size'] - p['offset']
        last_end = p['offset'] + p['size']
    return parts


def verify(parts, flash_size):
    errors = []
    names = set()
    for p in parts:
        if len(p['name'].encode()) > 16:
            errors.append('%s: label longer than 16 bytes' % p['name'])
        if p['name'] in names:
            errors.append('%s: duplicate label' % p['name'])
        names.add(p['name'])
        align = OFFSET_ALIGN.get(p['type'], 0x1000)
        if p['offset'] % align:
            errors.append('%s: offset 0x%x not aligned to 0x%x' % (p['name'], p['offset'], align))
        if p['size'] <= 0 or p['size'] % 0x1000:
            errors.append('%s: size 0x%x not a positive multiple of 0x1000' % (p['name'], p['size']))
        if p['offset'] + p['size'] > flash_size:
            errors.append('%s: ends at 0x%x beyond flash size 0x%x' % (p['name'], p['offset'] + p['size'], flash_size))
    return errors


def encode(parts):
    return b''.join(struct.pack(ENTRY_FMT, MAGIC, p['type'], p['subtype'], p['offset'], p['size'],
                                p['name'].encode(), p['flags']) for p in parts)


def find_gen_esp32part():
    cands = []
    if os.environ.get('IDF_PATH'):
        cands.append(os.path.join(os.environ['IDF_PATH'], 'components', 'partition_table', 'gen_esp32part.py'))
    cands += sorted(glob.glob(os.path.expanduser('~/.espressif/v*/esp-idf/components/partition_table/gen_esp32part.py')),
                    reverse=True)
    return next((c for c in cands if os.path.isfile(c)), None)


def idf_crosscheck(tool, csv_path, flash_size, ours):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'pt.bin')
        res = subprocess.run([sys.executable, tool, '-q', '--flash-size', '%dMB' % (flash_size >> 20), csv_path, out],
                             capture_output=True, text=True)
        if res.returncode != 0:
            return 'gen_esp32part.py failed: %s' % (res.stderr.strip() or res.stdout.strip())
        with open(out, 'rb') as f:
            data = f.read()
    entries = b''
    for i in range(0, len(data), 32):
        if data[i:i + 2] != MAGIC:
            break
        entries += data[i:i + 32]
    if entries != ours:
        return 'binary entries differ from gen_esp32part.py output'
    return None


def layout_crc(blob):
    return zlib.crc32(blob) & 0xFFFFFFFF


def display_path(path):
    rel = os.path.relpath(os.path.abspath(path), REPO)
    return os.path.abspath(path) if rel.startswith('..') else rel


def macro_name(label):
    return re.sub(r'[^A-Z0-9]', '_', label.upper())


def to_json(parts, crc, flash_size, csv_rel):
    return {
        'format': 'skygauge.partition_layout/1',
        'generator': 'tools/gen_layout.py',
        'source': csv_rel,
        'flash_size': flash_size,
        'crc32': '0x%08x' % crc,
        'crc_definition': 'zlib CRC32 over the 32-byte gen_esp32part entries (<2sBBLL16sL), CSV order, no MD5 entry',
        'partitions': [{
            'name': p['name'], 'type': p['type'], 'subtype': p['subtype'],
            'offset': p['offset'], 'size': p['size'], 'flags': p['flags'],
            'offset_hex': '0x%X' % p['offset'], 'size_hex': '0x%X' % p['size'],
        } for p in parts],
    }


def to_header(parts, crc, flash_size, csv_rel):
    width = max(len('PARTITION_%s_OFFSET' % macro_name(p['name'])) for p in parts)
    width = max(width, len('PARTITION_LAYOUT_FLASH_SIZE')) + 1
    lines = [
        '/* Generated by tools/gen_layout.py from %s. Do not edit. */' % csv_rel,
        '#pragma once',
        '',
        '/* CRC32 over the binary partition entries (gen_esp32part encoding, CSV order). */',
        '#define %-*s 0x%08xu' % (width, 'PARTITION_LAYOUT_CRC', crc),
        '#define %-*s %d' % (width, 'PARTITION_LAYOUT_COUNT', len(parts)),
        '#define %-*s 0x%Xu' % (width, 'PARTITION_LAYOUT_FLASH_SIZE', flash_size),
        '',
    ]
    for p in parts:
        m = macro_name(p['name'])
        lines.append('#define %-*s "%s"' % (width, 'PARTITION_%s_LABEL' % m, p['name']))
        lines.append('#define %-*s 0x%Xu' % (width, 'PARTITION_%s_OFFSET' % m, p['offset']))
        lines.append('#define %-*s 0x%Xu' % (width, 'PARTITION_%s_SIZE' % m, p['size']))
    return '\n'.join(lines) + '\n'


def parse_doc_size(text):
    t = text.strip().replace(',', '')
    if re.match(r'^0x[0-9a-fA-F]+$', t):
        return int(t, 16)
    m = re.match(r'^([0-9]+(?:\.[0-9]+)?)\s*(B|KB|KiB|K|MB|MiB|M)?$', t)
    if not m:
        return None
    mult = {None: 1, 'B': 1, 'KB': 1024, 'KiB': 1024, 'K': 1024, 'MB': 1 << 20, 'MiB': 1 << 20, 'M': 1 << 20}[m.group(2)]
    value = float(m.group(1)) * mult
    return int(value) if value == int(value) else None


def doc_table(text):
    """Rows of the first markdown table under the partition-table heading."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(r'^#{2,3}\s*(分区表|Partition table)\b', line.strip(), re.I):
            start = i
            break
    if start is None:
        return None, 'partition-table heading (## 分区表 / ## Partition table) not found'
    rows = []
    header = None
    for line in lines[start + 1:]:
        s = line.strip()
        if s.startswith('#'):
            break
        if not s.startswith('|'):
            if header is not None:
                break
            continue
        cells = [c.strip() for c in s.strip('|').split('|')]
        if header is None:
            header = cells
            continue
        if all(re.match(r'^:?-+:?$', c) for c in cells if c):
            continue
        rows.append(cells)
    if header is None:
        return None, 'no table under the partition-table heading'
    try:
        off_col = next(i for i, h in enumerate(header) if re.search(r'偏移|offset', h, re.I))
        size_col = next(i for i, h in enumerate(header) if re.search(r'大小|size', h, re.I))
    except StopIteration:
        return None, 'table header needs offset and size columns: %s' % header
    out = []
    for r in rows:
        name = r[0].strip('` ')
        try:
            off = int(r[off_col].strip('` '), 0)
        except (ValueError, IndexError):
            off = None
        size = parse_doc_size(r[size_col].strip('` ')) if size_col < len(r) else None
        out.append((name, off, size, r))
    return out, None


def check_doc(path, parts):
    errors = []
    with open(path, encoding='utf-8') as f:
        text = f.read()
    rows, err = doc_table(text)
    if err:
        return ['%s: %s' % (path, err)]
    by_name = {p['name']: p for p in parts}
    doc_names = [r[0] for r in rows]
    for name, off, size, raw in rows:
        p = by_name.get(name)
        if p is None:
            errors.append('%s: row %r is not in partitions.csv' % (path, name))
            continue
        if off != p['offset']:
            errors.append('%s: %s offset %s, partitions.csv has 0x%X' % (path, name, raw[1], p['offset']))
        if size != p['size']:
            errors.append('%s: %s size %r, partitions.csv has 0x%X (%s)' % (path, name, raw[2], p['size'], human_size(p['size'])))
    for p in parts:
        if p['name'] not in doc_names:
            errors.append('%s: partition %s (0x%X, %s) missing from the table' % (path, p['name'], p['offset'], human_size(p['size'])))
    common = [n for n in doc_names if n in by_name]
    csv_order = [p['name'] for p in parts if p['name'] in doc_names]
    if common != csv_order:
        errors.append('%s: row order %s differs from partitions.csv order %s' % (path, common, csv_order))
    # flash command lines: "0xADDR  path/to/file.bin"
    for m in re.finditer(r'(0x[0-9a-fA-F]+)\s+(\S+\.bin)\b', text):
        addr, fname = int(m.group(1), 16), m.group(2)
        for pat, part in DOC_FILE_TO_PART:
            if pat.search(fname) and part in by_name and addr != by_name[part]['offset']:
                errors.append('%s: %s flashed at 0x%X but %s is at 0x%X' % (path, fname, addr, part, by_name[part]['offset']))
    return errors


def human_size(n):
    if n % (1 << 20) == 0 or (n >= (1 << 20) and (n * 1000) % (1 << 20) == 0):
        return ('%g MB' % (n / float(1 << 20)))
    return '%g KB' % (n / 1024.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--csv', default=os.path.join(REPO, 'partitions.csv'))
    ap.add_argument('--json', default=os.path.join(REPO, 'build', 'partition_layout.json'))
    ap.add_argument('--header', default=os.path.join(REPO, 'build', 'partition_layout.h'))
    ap.add_argument('--flash-size', default='16MB', help='flash size for bounds checks (default 16MB)')
    ap.add_argument('--check', action='store_true',
                    help='verify the partition tables in the docs; do not write outputs')
    ap.add_argument('--docs', nargs='*',
                    default=[os.path.join(REPO, 'docs', 'FLASH.md'), os.path.join(REPO, 'docs', 'FLASH.en.md')])
    ap.add_argument('--no-idf', action='store_true', help='skip the gen_esp32part.py cross-check')
    ap.add_argument('--expect-crc', help='fail unless PARTITION_LAYOUT_CRC equals this value')
    args = ap.parse_args()

    flash_size = parse_int(args.flash_size.replace('MB', 'm').replace('KB', 'k'))
    csv_rel = display_path(args.csv)
    errors = []
    try:
        with open(args.csv, encoding='utf-8') as f:
            parts = parse_csv(f.read())
    except (OSError, LayoutError) as e:
        print('error: %s: %s' % (args.csv, e))
        return 1
    if not parts:
        print('error: %s has no partitions' % args.csv)
        return 1
    errors += verify(parts, flash_size)
    blob = encode(parts)
    crc = layout_crc(blob)
    print('%s: %d partitions, PARTITION_LAYOUT_CRC = 0x%08x' % (csv_rel, len(parts), crc))
    for p in parts:
        print('  %-10s type 0x%02x subtype 0x%02x offset 0x%07X size 0x%07X (%s)'
              % (p['name'], p['type'], p['subtype'], p['offset'], p['size'], human_size(p['size'])))

    if not args.no_idf:
        tool = find_gen_esp32part()
        if tool:
            err = idf_crosscheck(tool, os.path.abspath(args.csv), flash_size, blob)
            if err:
                errors.append(err)
            else:
                print('gen_esp32part cross-check: identical entries (%s)' % tool)
        else:
            print('gen_esp32part cross-check: skipped (ESP-IDF not found)')

    if args.expect_crc is not None and crc != int(args.expect_crc, 16):
        errors.append('PARTITION_LAYOUT_CRC 0x%08x != expected %s' % (crc, args.expect_crc))

    if args.check:
        for doc in args.docs:
            doc_errors = check_doc(doc, parts)
            errors += doc_errors
            if not doc_errors:
                print('%s: partition table matches' % display_path(doc))
    elif not errors:
        for path, content in ((args.json, json.dumps(to_json(parts, crc, flash_size, csv_rel), indent=2) + '\n'),
                              (args.header, to_header(parts, crc, flash_size, csv_rel))):
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            with open(path, 'w', encoding='utf-8') as f:
                f.write(content)
            print('wrote %s' % display_path(path))

    for e in errors:
        print('error: %s' % e)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
