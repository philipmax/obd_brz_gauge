#!/usr/bin/env python3
"""Generate firmware/release/latest.json from git metadata + the release binaries.

Run from anywhere (uses the repo root relative to this file). It assumes the
project has already been built, so `build/` holds the fresh *.bin files:

    idf.py build
    python3 tools/gen_release.py

It does two things:
  1. copies build/{bootloader,partition_table,ota_data_initial,obd_brz_gauge,bootmedia}
     into firmware/release/ (the static release directory the app reads);
  2. writes firmware/release/latest.json with the git build tag and the
     sha256/size of every release binary.

The companion app compares `firmware.count` here against the device's own
manifest to decide whether an update is available. `count` comes from the
current git HEAD, so remember to `git commit` your code *before* building
(see docs/DEVELOPMENT.md).

REFACTOR_PLAN.md P6 additions:
  - Adds `layout` (partition layout_id) and `flash_files` (web flasher map) to manifest
  - Validates firmware image ≤ 3MB (0x300000), warns at 90%
  - Validates bootmedia ≤ 4.75MB (0x4C0000) if present
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(ROOT, "build")
RELEASE_DIR = os.path.join(ROOT, "firmware", "release")

# Partition layout v2 CRC32 (from partitions.csv, all 9 entries)
LAYOUT_ID = 0x94752AA3

# Size limits (partition layout v2)
MAX_FIRMWARE_SIZE = 0x300000      # 3 MB per OTA slot
MAX_BOOTMEDIA_SIZE = 0x4C0000     # 4.75 MB bootmedia partition
FIRMWARE_WARN_PCT = 90            # warn if firmware exceeds this % of MAX

# Device block must stay in sync with main/app_obd_dsp/device_identity.c.
DEVICE = {
    "board": "Waveshare ESP32-S3-Touch-LCD-1.85",
    "variant": "obd_brz_gauge",
    "lcd": "ST77916",
    "screen": {"w": 360, "h": 360, "bpp": 16},
    "flash_mb": 16,
    "psram_mb": 8,
    "ota_slots": 2,
    "bootmedia_slots": 1,
    "bootmedia_format": 1,
}

# Firmware metadata not derivable from git. Update IDF_VER when you upgrade ESP-IDF.
PROJECT = "obd_brz_gauge"
VERSION = "1"
IDF_VER = "v5.5.3"
SLOT = "ota_0"

# build/ source -> firmware/release/ destination (relative to RELEASE_DIR).
BINS = {
    "firmware": "obd_brz_gauge.bin",
    "bootmedia": "bootmedia.bin",
    "partition_table": "partition_table/partition-table.bin",
    "bootloader": "bootloader/bootloader.bin",
    "ota_data_initial": "ota_data_initial.bin",
}


def git(*args):
    return subprocess.check_output(["git", "-C", ROOT, *args], text=True).strip()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    # 1. Copy the fresh build artifacts into the release directory.
    for rel in BINS.values():
        src = os.path.join(BUILD_DIR, rel)
        dst = os.path.join(RELEASE_DIR, rel)
        if not os.path.isfile(src):
            sys.exit(f"missing build artifact: {src}\nRun `idf.py build` first.")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        print(f"copied {rel}")

    # 1a. Validate firmware size (REFACTOR_PLAN.md P6)
    fw_path = os.path.join(RELEASE_DIR, "obd_brz_gauge.bin")
    fw_size = os.path.getsize(fw_path)
    if fw_size > MAX_FIRMWARE_SIZE:
        sys.exit(f"ERROR: Firmware {fw_size} bytes exceeds {MAX_FIRMWARE_SIZE} bytes (3 MB OTA slot limit)")

    warn_threshold = int(MAX_FIRMWARE_SIZE * FIRMWARE_WARN_PCT / 100)
    if fw_size > warn_threshold:
        pct = fw_size * 100.0 / MAX_FIRMWARE_SIZE
        print(f"⚠️  WARNING: Firmware is {fw_size} bytes ({pct:.1f}% of 3 MB limit)")
    else:
        pct = fw_size * 100.0 / MAX_FIRMWARE_SIZE
        print(f"✓ Firmware size: {fw_size} bytes ({pct:.1f}% of 3 MB limit)")

    # 1b. Validate bootmedia size if present
    bootmedia_path = os.path.join(BUILD_DIR, "bootmedia.bin")
    if os.path.isfile(bootmedia_path):
        bm_size = os.path.getsize(bootmedia_path)
        if bm_size > MAX_BOOTMEDIA_SIZE:
            print(f"⚠️  WARNING: bootmedia.bin {bm_size} bytes exceeds {MAX_BOOTMEDIA_SIZE} bytes (4.75 MB partition)")
        else:
            print(f"✓ bootmedia size: {bm_size} bytes")

    # 2. Build the version metadata from the current git HEAD.
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    count = int(git("rev-list", "--count", "HEAD"))
    short = git("rev-parse", "--short=12", "HEAD")
    branch_safe = re.sub(r"[^A-Za-z0-9._-]", "_", branch)

    # built timestamp from the freshly copied firmware image (its build mtime).
    fw_path = os.path.join(RELEASE_DIR, "obd_brz_gauge.bin")
    built = time.strftime("%b %d %Y %H:%M:%S", time.localtime(os.path.getmtime(fw_path)))

    files = {}
    for key, rel in BINS.items():
        p = os.path.join(RELEASE_DIR, rel)
        files[key] = {"path": rel, "size": os.path.getsize(p), "sha256": sha256(p)}

    # Flash addresses for web flasher complete mode (REFACTOR_PLAN.md P6)
    flash_files = {
        "0x0": "bootloader/bootloader.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0xf000": "ota_data_initial.bin",
        "0x20000": "obd_brz_gauge.bin",
        # Optional files (web flasher may skip these):
        # "0x620000": "theme.bin",        # user-provided theme
        # "0xA20000": "bootmedia.bin",    # boot animation
    }

    manifest = {
        "device": DEVICE,
        "layout": {
            "id": LAYOUT_ID,
            "version": 2,
            "app_slot_size": MAX_FIRMWARE_SIZE,
            "bootmedia_max": MAX_BOOTMEDIA_SIZE,
        },
        "firmware": {
            "project": PROJECT,
            "version": VERSION,
            "build_tag": f"{branch_safe}-{count}-{short}",
            "git": short,
            "branch": branch,
            "count": count,
            "built": built,
            "idf": IDF_VER,
            "slot": SLOT,
        },
        "files": files,
        "flash_files": flash_files,
    }

    out = os.path.join(RELEASE_DIR, "latest.json")
    with open(out, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"wrote latest.json  count={count}  build_tag={manifest['firmware']['build_tag']}")
    print("next: git add firmware/release/ && git commit -m \"build: update release firmware binaries\"")


if __name__ == "__main__":
    main()
