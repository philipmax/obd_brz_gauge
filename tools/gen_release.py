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
current git HEAD, so the source must be committed *before* building, and the
build reconfigured so CMake re-reads HEAD (tools/release.sh does both). The
script refuses to run when the firmware's embedded build tag does not match
HEAD.

Checks:
  - firmware image <= 0x300000 (ota slot), warning above 90 %;
  - bootmedia.bin is the raw layout [manifest padded with 0xFF to 4 KB][bin]
    (not a SPIFFS image), distributed for installation on SD.
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(ROOT, "build")
RELEASE_DIR = os.path.join(ROOT, "firmware", "release")

# Partition layout v2 CRC32 (from partitions.csv, all 9 entries)
LAYOUT_ID = 0x94752AA3

# Size limits (partition layout v2)
MAX_FIRMWARE_SIZE = 0x300000      # 3 MB per OTA slot
MAX_BOOTMEDIA_SIZE = 0          # retired writable Flash media capacity
BOOTMEDIA_MANIFEST_MAX = 0x1000   # raw bootmedia: manifest padded with 0xFF to 4 KB
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
    # Validate the complete build before changing any published artifact.
    bins = {key: rel for key, rel in BINS.items()
            if key != "bootmedia" or os.path.isfile(os.path.join(BUILD_DIR, rel))}
    for rel in bins.values():
        src = os.path.join(BUILD_DIR, rel)
        if not os.path.isfile(src) or os.path.getsize(src) == 0:
            sys.exit(f"missing or empty build artifact: {src}\nRun `idf.py build` first.")

    # 1a. Validate firmware size (REFACTOR_PLAN.md P6)
    fw_path = os.path.join(BUILD_DIR, "obd_brz_gauge.bin")
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

    # Legacy raw animation is optional: current media lives on SD.
    if "bootmedia" in bins:
        bm_path = os.path.join(BUILD_DIR, "bootmedia.bin")
        bm_size = os.path.getsize(bm_path)
        with open(bm_path, "rb") as f:
            bm_head = f.read(BOOTMEDIA_MANIFEST_MAX)
        if bm_size <= BOOTMEDIA_MANIFEST_MAX or not (0x61 <= bm_head[0] <= 0x7A):
            sys.exit("ERROR: bootmedia.bin is not the raw [manifest 4 KB][bin] layout "
                     "(SPIFFS image?) - rebuild it with tools/make_bootmedia_raw.py")
        print(f"✓ bootmedia size: {bm_size} bytes (raw)")

    # 2. Build the version metadata from the current git HEAD.
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    count = int(git("rev-list", "--count", "HEAD"))
    short = git("rev-parse", "--short=12", "HEAD")
    branch_safe = re.sub(r"[^A-Za-z0-9._-]", "_", branch)
    build_tag = f"{branch_safe}-{count}-{short}"

    # 2a. The firmware embeds OBD_GAUGE_BUILD_TAG from CMake's configure-time
    #     HEAD. A mismatch means it was built before the last commit (the old
    #     "one release behind" bug) - never publish that.
    with open(fw_path, "rb") as f:
        if build_tag.encode() + b"\0" not in f.read():
            sys.exit(f"ERROR: firmware was not built from HEAD ({build_tag}).\n"
                     "Commit first, then `idf.py reconfigure build` (or use tools/release.sh).")

    # built timestamp from the freshly copied firmware image (its build mtime).
    fw_path = os.path.join(BUILD_DIR, "obd_brz_gauge.bin")
    built = time.strftime("%b %d %Y %H:%M:%S", time.localtime(os.path.getmtime(fw_path)))

    files = {}
    for key, rel in bins.items():
        p = os.path.join(BUILD_DIR, rel)
        files[key] = {"path": rel, "size": os.path.getsize(p), "sha256": sha256(p)}

    # Complete-flash map (offsets from partitions.csv). Custom media is SD-only.
    flash_files = {
        "0x0": "bootloader/bootloader.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0xf000": "ota_data_initial.bin",
        "0x20000": "obd_brz_gauge.bin",
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
            "build_tag": build_tag,
            "git": short,
            "branch": branch,
            "count": count,
            "built": built,
            "idf": IDF_VER,
            "slot": SLOT,
        },
        "files": files,
        "flash_files": flash_files,
        "resource_storage": "sd",
        "sd_files": {"/OBD/BOOT/bootmedia.raw": "bootmedia.bin"} if "bootmedia" in bins else {},
    }

    # Stage all bytes first. Publish the manifest last; failed validation above
    # leaves the existing release untouched. The watcher also checks every hash.
    os.makedirs(RELEASE_DIR, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".release-", dir=os.path.dirname(RELEASE_DIR)) as stage:
        for rel in bins.values():
            dst = os.path.join(stage, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(BUILD_DIR, rel), dst)
        for key, rel in bins.items():
            if sha256(os.path.join(stage, rel)) != files[key]["sha256"]:
                sys.exit("ERROR: build artifacts changed while preparing release")
        with open(os.path.join(stage, "latest.json"), "w") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        for rel in bins.values():
            dst = os.path.join(RELEASE_DIR, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.replace(os.path.join(stage, rel), dst)
        if "bootmedia" not in bins:
            stale = os.path.join(RELEASE_DIR, "bootmedia.bin")
            if os.path.exists(stale):
                os.unlink(stale)
        os.replace(os.path.join(stage, "latest.json"), os.path.join(RELEASE_DIR, "latest.json"))

    print(f"wrote latest.json  count={count}  build_tag={manifest['firmware']['build_tag']}")
    print("next: git add firmware/release/ && git commit -m \"build: update release firmware binaries\"")


if __name__ == "__main__":
    main()
