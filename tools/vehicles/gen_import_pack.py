#!/usr/bin/env python3
"""Generate editable, firmware-importable copies of the vehicle catalog.

Run from any directory:
    python3 tools/vehicles/gen_import_pack.py
    python3 tools/vehicles/gen_import_pack.py --check

Output: vehicles/import/OBD/IMPORT/custom.*.vehicle.json. Copy a file elsewhere
before personalizing it: regeneration restores the distributed template. This
script does not install or activate profiles and does not validate hardware
compatibility; validate outputs with the firmware's validate_catalog tool.
"""

import argparse
import copy
import json
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
CATALOG = REPO_ROOT / "vehicles/catalog"
OUTPUT = REPO_ROOT / "vehicles/import/OBD/IMPORT"

# Minimum intervals, not guaranteed sampling frequencies. All requests still
# run serially through the firmware engine. CAN-only and derived signals retain
# their original scheduling/monitor configuration.
PERIOD_MS = {
    "obd.rpm": 100,
    "obd.speed": 200,
    "obd.gear": 200,
    "obd.load": 200,
    "obd.throttle": 200,
    "obd.boost": 200,
    "obd.afr": 500,
    "obd.oil_pressure": 500,
    "obd.intake_temp": 1000,
    "obd.coolant_temp": 1000,
    "obd.oil_temp": 1000,
    "obd.battery_voltage": 1000,
}
BUS_DEFAULTS = {"proto": 0, "hdr": "7E0", "atst": 25, "gap_ms": 30}


def import_profile(original, source_path, *, gear_variant=False):
    profile = copy.deepcopy(original)
    profile["id"] = "custom.bmw.g.gear" if gear_variant else "custom." + original["id"]
    profile["name"] = "BMW G gear cfg" if gear_variant else original["name"] + " cfg"
    # Custom profiles must not claim aliases of the catalog/built-in profiles.
    profile.pop("aliases", None)
    profile.pop("force", None)
    profile["origin"] = {
        "from": source_path.relative_to(REPO_ROOT).as_posix(),
        "profile_id": original["id"],
        "profile_rev": original["rev"],
        "generator": "tools/vehicles/gen_import_pack.py",
    }
    notes = profile.get("notes", [])
    if isinstance(notes, str):
        notes = [notes]
    profile["notes"] = notes + [
        "Editable catalog-derived template; hardware compatibility requires vehicle testing.",
        "period_ms is a minimum interval; actual rates depend on serial polling and adapter latency.",
        "expr is informational; edit rpn to change decoding.",
    ]
    if gear_variant:
        profile["notes"].append(
            "BMW G direct gear template reuses bmw.fg protocol 6, raw EGS DID DA2E and ratio fallback. "
            "The protocol 7 bmw.g.obd profile remains a separate variant."
        )
    for key, value in BUS_DEFAULTS.items():
        profile["bus"].setdefault(key, value)
    for signal in profile["signals"]:
        polled = False
        for source in signal.get("src", []):
            if "req" in source or "raw" in source:
                polled = True
                source.setdefault("hdr", profile["bus"]["hdr"])
                if "expect" not in source:
                    if "raw" in source:
                        raise ValueError(f'{profile["id"]}: raw source needs an explicit expect')
                    req = bytes.fromhex(source["req"])
                    source["expect"] = bytes([req[0] + 0x40, *req[1:]]).hex().upper()
                source.setdefault("fail_after", 5)
            elif "can" in source:
                source.setdefault("endian", "be")
                source.setdefault("signed", False)
                source.setdefault("fail_after", 5)
        if polled:
            signal["rate"] = "round"
            signal.pop("div", None)
            signal["period_ms"] = PERIOD_MS.get(signal["id"], 1000)
    if len(profile["id"]) > 31 or len(profile["name"]) > 23:
        raise ValueError(f'{profile["id"]}: firmware id/name limit exceeded')
    return profile


def render_profiles():
    outputs = {}
    for path in sorted(CATALOG.glob("*.vehicle.json")):
        original = json.loads(path.read_text(encoding="utf-8"))
        variants = [False, True] if original["id"] == "bmw.fg" else [False]
        for gear_variant in variants:
            profile = import_profile(original, path, gear_variant=gear_variant)
            name = profile["id"] + ".vehicle.json"
            text = json.dumps(profile, ensure_ascii=False, indent=2) + "\n"
            if len(text.encode("utf-8")) > 16384:
                raise ValueError(f"{name}: exceeds firmware JSON import limit")
            if name in outputs:
                raise ValueError(f"Duplicate profile: {name}")
            outputs[name] = text
    if not outputs or "custom.bmw.g.gear.vehicle.json" not in outputs:
        raise ValueError("Catalog is empty or BMW F/G gear source is missing")
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check generated files without writing")
    args = parser.parse_args()
    outputs = render_profiles()
    unexpected = {p.name for p in OUTPUT.glob("*.vehicle.json")} - outputs.keys()
    if unexpected:
        print("Unexpected profiles in generated pack: " + ", ".join(sorted(unexpected)), file=sys.stderr)
        return 1
    if args.check:
        stale = [name for name, text in outputs.items()
                 if not (OUTPUT / name).exists() or (OUTPUT / name).read_text(encoding="utf-8") != text]
        if stale:
            print("Import templates need regeneration: " + ", ".join(stale), file=sys.stderr)
            return 1
        print(f"Import pack: {len(outputs)} profiles are current")
    else:
        OUTPUT.mkdir(parents=True, exist_ok=True)
        for name, text in outputs.items():
            (OUTPUT / name).write_text(text, encoding="utf-8")
        print(f"Generated {len(outputs)} profiles in {OUTPUT.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
