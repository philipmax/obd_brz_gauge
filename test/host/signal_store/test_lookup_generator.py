#!/usr/bin/env python3
"""Compile generated mappings for reordered, sparse and reduced registries.
The linear sig_registry table is the oracle for all 65,536 numeric IDs.
"""
import copy
import importlib.util
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("gen_signals", ROOT / "tools/gen_signals.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
registry = json.loads((ROOT / "contracts/signals.json").read_text())
variants = [("current", registry)]
for label in ["reversed", "rotated", "shuffled", "sparse", "sparse_shuffled", "subset", "ids_reversed"]:
    reg = copy.deepcopy(registry)
    if label in ("sparse", "sparse_shuffled"):
        for prefix, first, stride in [("obd.", 1, 7), ("sys.", 0x80, 3)]:
            entries = [s for s in reg["signals"] if s["key"].startswith(prefix)]
            for i, signal in enumerate(entries):
                signal["id"] = f"0x{first + i * stride:04X}"
    if label == "ids_reversed":
        for prefix in ["obd.", "sys."]:
            entries = [s for s in reg["signals"] if s["key"].startswith(prefix)]
            ids = [s["id"] for s in entries][::-1]
            for signal, num_id in zip(entries, ids):
                signal["id"] = num_id
    if label == "reversed":
        reg["signals"].reverse()
    if label == "rotated":
        reg["signals"] = reg["signals"][5:] + reg["signals"][:5]
    if label in ("shuffled", "sparse_shuffled"):
        random.Random(0x1764).shuffle(reg["signals"])
    if label == "subset":
        reg["signals"] = reg["signals"][::3]
    variants.append((label, reg))

source = r'''
#include <stdio.h>
#include "signal_registry.h"
int main(void) {
    for (unsigned id = 0; id < 65536; id++) {
        int expected = -1;
        for (int slot = 0; slot < SIG_STD_COUNT; slot++)
            if (sig_registry[slot].num_id == id) { expected = slot; break; }
        if (sig_registry_slot_by_id((uint16_t)id) != expected) {
            fprintf(stderr, "id=%u expected=%d\n", id, expected); return 1;
        }
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="obd-signal-generator-") as work:
    work = Path(work)
    for label, reg in variants:
        header, table = generator.generate(reg)
        (work / "signal_registry.h").write_text(header)
        (work / "signal_registry.c").write_text(table)
        (work / "main.c").write_text(source)
        subprocess.run([sys.argv[1], "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(work / "main.c"), str(work / "signal_registry.c"), "-o", str(work / "lookup")],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(work / "lookup")], check=True, capture_output=True, text=True)
print(f"signal_lookup_generator: {len(variants)} registries, {65536 * len(variants)} IDs checked")
