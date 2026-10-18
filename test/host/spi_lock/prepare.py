#!/usr/bin/env python3
"""Extract the actual SDK functions, with no rewritten scheduling algorithm."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/idf_compat"))
from patch_spi_bus_lock import patched_source


def function(source, name):
    match = re.search(r"(?:SPI_BUS_LOCK_ISR_ATTR|IRAM_ATTR) static inline [^{;]*\b"
                      + re.escape(name) + r"\([^;{]*\)\s*\{", source)
    if not match:
        raise ValueError(f"cannot locate SDK function {name}")
    end = match.end()
    depth = 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[match.start():end] + "\n"


def inputs(source):
    return "\n".join(function(source, name) for name in
                     ("lock_status_fetch", "schedule_core", "bg_exit_core"))


if __name__ == "__main__":
    source = Path(sys.argv[1]).read_text()
    patch = (ROOT / "tools/idf_compat/spi_bus_lock.patch").read_text()
    output = Path(sys.argv[2])
    # The baseline must be the actual affected release, not an already-fixed SDK.
    import hashlib
    if hashlib.sha256(source.encode()).hexdigest() != "e40cccc457a4fa4f2bb170c58031699beccbd384238459533e2cdf44bab5159b":
        raise ValueError("negative control requires stock ESP-IDF v5.5.3 spi_bus_lock.c")
    (output / "baseline.h").write_text(inputs(source))
    (output / "patched.h").write_text(inputs(patched_source(source, patch)))
