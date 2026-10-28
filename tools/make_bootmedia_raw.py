#!/usr/bin/env python3
"""Build the raw bootmedia partition image.

    python3 tools/make_bootmedia_raw.py --src bootmedia/slot_a --out build/bootmedia.bin

Legacy bootmedia layout (also used by SD bootmedia.raw):
    0x0000  boot_block.txt, padded with 0xFF to 4 KB
    0x1000  boot_block.bin

The image must fit the bootmedia partition (0x4C0000, partitions.csv).
"""

import argparse
import os
import sys

MANIFEST_MAX = 0x1000
PARTITION_SIZE = 0x4C0000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True, help='directory with boot_block.txt + boot_block.bin')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    with open(os.path.join(args.src, 'boot_block.txt'), 'rb') as f:
        manifest = f.read()
    with open(os.path.join(args.src, 'boot_block.bin'), 'rb') as f:
        data = f.read()
    if not manifest or not (0x61 <= manifest[0] <= 0x7A):
        print('boot_block.txt must start with a lowercase key (a-z)', file=sys.stderr)
        return 1
    if len(manifest) > MANIFEST_MAX:
        print('boot_block.txt is %d B, max %d B' % (len(manifest), MANIFEST_MAX), file=sys.stderr)
        return 1
    image = manifest + b'\xff' * (MANIFEST_MAX - len(manifest)) + data
    if len(image) > PARTITION_SIZE:
        print('bootmedia image %d B exceeds partition 0x%X' % (len(image), PARTITION_SIZE),
              file=sys.stderr)
        return 1
    with open(args.out, 'wb') as f:
        f.write(image)
    print('bootmedia raw image: %d B (manifest %d B, bin %d B)' % (len(image), len(manifest), len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
