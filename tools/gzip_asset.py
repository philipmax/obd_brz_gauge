#!/usr/bin/env python3
"""Gzip a web asset for firmware embedding, failing when it exceeds a size cap.

    python3 tools/gzip_asset.py --max 49152 IN OUT

mtime is pinned to 0 so the output is reproducible across builds.
"""

import argparse
import gzip
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--max', type=int, default=0, help='max gzip size in bytes (0 = no cap)')
    ap.add_argument('src')
    ap.add_argument('dst')
    args = ap.parse_args()
    with open(args.src, 'rb') as f:
        raw = f.read()
    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    if args.max and len(gz) > args.max:
        print('%s: gzip size %d B exceeds cap %d B' % (args.src, len(gz), args.max), file=sys.stderr)
        return 1
    with open(args.dst, 'wb') as f:
        f.write(gz)
    print('%s: %d B -> %d B gzip' % (args.src, len(raw), len(gz)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
