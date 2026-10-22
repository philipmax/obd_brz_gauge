#!/usr/bin/env python3
"""Backport the SPI bus-lock fix to a build-local copy; never edit ESP-IDF."""
import argparse
from pathlib import Path


def patched_source(source, patch):
    """Apply exact, unique hunk context, or accept an already applied hunk."""
    hunks = []
    before, after = [], []
    for line in patch.splitlines(keepends=True):
        if line.startswith("@@"):
            if before:
                hunks.append(("".join(before), "".join(after)))
            before, after = [], []
        elif line.startswith(("---", "+++", "#")):
            continue
        elif line.startswith(" "):
            before.append(line[1:])
            after.append(line[1:])
        elif line.startswith("-"):
            before.append(line[1:])
        elif line.startswith("+"):
            after.append(line[1:])
    if before:
        hunks.append(("".join(before), "".join(after)))
    if not hunks:
        raise ValueError("empty SPI bus-lock patch")
    for index, (old, new) in enumerate(hunks, 1):
        if source.count(new) == 1:
            continue
        if source.count(old) != 1:
            raise ValueError(f"SPI bus-lock hunk {index} does not match this SDK; review its fix before building")
        source = source.replace(old, new, 1)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    original = args.source.read_text()
    patched = patched_source(original, Path(__file__).with_name("spi_bus_lock.patch").read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != patched:
        args.output.write_text(patched)
    print("SPI bus-lock: " + ("upstream fix already present" if patched == original else "backported upstream lock handoff fix"))


if __name__ == "__main__":
    main()
