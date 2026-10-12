#!/usr/bin/env python3
"""
legacy_to_v1.py — convert dump_legacy JSON output to skygauge.vehicle/1 profile files.

Usage:
    ./dump_legacy > /tmp/legacy.json
    python3 legacy_to_v1.py /tmp/legacy.json --out ../../vehicles/catalog/

Each legacy profile produces one <id>.vehicle.json file.  The OBD2 Generic
profile is skipped here because it is built into the firmware as a compiled
blob (vp_builtin_generic).

Oil-temp encoding: the legacy firmware used an enum strategy plus a separate
override table (vehicle_custom_config.h).  dump_legacy already merged the two;
this script maps the merged formula objects to skygauge.vehicle/1 signal sources.
"""

import argparse
import json
import math
import os
import re
import sys
from typing import Dict, List, Optional


# ------------------------------------------------------------------ #
#  ID derivation                                                       #
# ------------------------------------------------------------------ #

_KNOWN_IDS = {
    "OBD2 Generic":   "obd2.generic",
    "ZN/C6 CAN":      "subaru.zc6.can",
    "ZN/C6 PID":      "subaru.zc6.pid",
    "ZD8 OBD":        "subaru.zd8.obd",
    "ZD8":            "subaru.zd8.pid",
    "MX-5 ND":        "mazda.mx5nd",
    "BMW F/G":        "bmw.fg",
    "Supra A90":      "toyota.supra_a90",
    "BMW G OBD":      "bmw.g.obd",
    "BMW E":          "bmw.e",
    "JCW F56":        "mini.jcw_f56",
    "MINI R55":       "mini.r55",
    "POS 997.2":      "porsche.997_2",
    "POS 997.1":      "porsche.997_1",
    "GIULIA 2.0T":    "alfa.giulia_20t",
    "jeep":           "jeep.generic",
    "Honda Integra":  "honda.integra",
}


def _make_id(name: str) -> str:
    if name in _KNOWN_IDS:
        return _KNOWN_IDS[name]
    slug = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return f"unknown.{slug}"


# ------------------------------------------------------------------ #
#  ELM327 protocol → CAN description                                  #
# ------------------------------------------------------------------ #

def _protocol_notes(p: int) -> str:
    return {
        6: "ISO 15765-4 CAN 11-bit 500k",
        7: "ISO 15765-4 CAN 29-bit 500k",
        0: "auto-detect",
    }.get(p, f"protocol {p}")


# ------------------------------------------------------------------ #
#  Oil formula → signal source                                        #
# ------------------------------------------------------------------ #

_OIL_SPECIAL_ID_TO_REQ = {
    0: "21 01",   # Toyota/Subaru Mode 21 01
}


def _oil_formula_to_source(f: dict, uds_hdr: Optional[str]) -> Optional[dict]:
    """Convert a merged oil_formula_t dict to a vp signal source object."""
    if f is None:
        return None
    ftype = f["type"]
    if ftype == "std_pid":
        return {
            "req": f"01 {f['pid']}",
            "scale": f["scale"],
            "offset": f["offset"],
            "bytes": f["resp_bytes"],
            "byte_index": f["resp_byte"],
        }
    if ftype == "uds_22":
        src: dict = {
            "req": f"22 {f['pid']}",
            "scale": f["scale"],
            "offset": f["offset"],
            "bytes": f["resp_bytes"],
            "byte_index": f["resp_byte"],
        }
        if uds_hdr:
            # Strip ATSH prefix and \r suffix → bare hex address
            hdr = uds_hdr.replace("ATSH", "").replace("\\r", "").strip()
            src["hdr"] = hdr
        return src
    if ftype == "special":
        sid = f.get("special_id", 0)
        req = _OIL_SPECIAL_ID_TO_REQ.get(sid)
        if req is None:
            return None
        return {
            "req": req,
            "scale": f["scale"],
            "offset": f["offset"],
            "bytes": f["resp_bytes"],
            "byte_index": f["resp_byte"],
            "_note": "Toyota/Subaru Mode 21 — multi-frame special parsing",
        }
    return None


def _oil_sources(profile: dict) -> list[dict]:
    uds_hdr = profile.get("uds_header_cmd")
    sources = []
    for key in ("oil_primary", "oil_secondary"):
        f = profile.get(key)
        if f:
            src = _oil_formula_to_source(f, uds_hdr)
            if src:
                sources.append(src)
    # Deduplicate identical consecutive sources
    deduped = []
    for s in sources:
        if not deduped or deduped[-1] != s:
            deduped.append(s)
    return deduped


# ------------------------------------------------------------------ #
#  CAN broadcast rules → skygauge.vehicle/1 can_sources              #
# ------------------------------------------------------------------ #

_CH_TO_SIGNAL = {
    "rpm":      "obd.rpm",
    "speed":    "obd.speed",
    "oil_temp": "obd.oil_temp",
    "coolant":  "obd.coolant_temp",
    "tps":      "obd.tps",
    "load":     "obd.load",
    "intake":   "obd.intake_temp",
    "boost":    "obd.boost",
    "gear":     "obd.gear",
}


def _can_rules_to_sources(rules: list[dict]) -> list[dict]:
    out = []
    for r in rules:
        sig = _CH_TO_SIGNAL.get(r["channel"])
        if not sig:
            continue
        out.append({
            "signal": sig,
            "can_id": r["can_id"],
            "bit_off": r["bit_off"],
            "bit_len": r["bit_len"],
            "scale": r["scale"],
            "offset": r["offset"],
        })
    return out


# ------------------------------------------------------------------ #
#  Gear signal source (Mode 22 gear DID or EGS raw frame)            #
# ------------------------------------------------------------------ #

def _gear_source(profile: dict) -> Optional[dict]:
    gear_did = profile.get("obd_gear_did")
    raw_frame = profile.get("obd_gear_raw_frame")
    hdr_cmd = profile.get("obd_gear_header_cmd")
    rx_cmd = profile.get("obd_gear_rx_filter_cmd")
    if not gear_did and not raw_frame:
        return None
    src: dict = {}
    if raw_frame:
        src["raw"] = raw_frame.replace("\\r", "").strip()
    elif gear_did:
        did_int = int(gear_did, 16)
        src["req"] = f"22 {(did_int >> 8) & 0xFF:02X} {did_int & 0xFF:02X}"
    if hdr_cmd:
        src["hdr"] = hdr_cmd.replace("ATSH", "").replace("\\r", "").strip()
    if rx_cmd:
        src["rx"] = rx_cmd.replace("ATCRA", "").replace("\\r", "").strip()
    return src


# ------------------------------------------------------------------ #
#  Oil pressure source (Mode 22 DID)                                 #
# ------------------------------------------------------------------ #

def _oil_pressure_source(profile: dict) -> Optional[dict]:
    did = profile.get("obd_oil_pressure_did")
    if not did:
        return None
    did_int = int(did, 16)
    # hPa, raw×1 for both known DIDs (B58 4436, N55 586F)
    return {
        "req": f"22 {(did_int >> 8) & 0xFF:02X} {did_int & 0xFF:02X}",
        "scale": 1.0,
        "offset": 0.0,
        "unit": "hPa",
    }


# ------------------------------------------------------------------ #
#  Convert one legacy profile dict → skygauge.vehicle/1 dict         #
# ------------------------------------------------------------------ #

def convert(p: dict) -> dict:
    name = p["name"]
    vid = _make_id(name)
    gear_count = p["gear_count"]
    ratios = p["gear_ratios"]  # index 0 is neutral placeholder (0.0)

    # Bus configuration
    bus: dict = {}
    forced = p.get("forced_protocol", 0)
    if forced:
        bus["protocol"] = forced
        bus["_protocol_note"] = _protocol_notes(forced)
    if p.get("obd_functional_addr"):
        if p.get("obd_29bit_functional"):
            bus["hdr"] = "18DB33F1"
        else:
            bus["hdr"] = "7DF"
    obd_to = p.get("obd_timeout", 0)
    if obd_to and obd_to != 0x19:  # 0x19 is ELM327 default
        bus["atst"] = obd_to
    poll = p.get("poll_gap_ms", 0)
    if poll and poll > 0:
        bus["poll_gap_ms"] = poll

    # Signals list
    signals = []

    # Standard OBD signals (present on all profiles)
    std_signals = [
        {"key": "obd.rpm",         "req": "01 0C", "scale": 0.25,  "offset": 0.0},
        {"key": "obd.speed",       "req": "01 0D", "scale": 1.0,   "offset": 0.0},
        {"key": "obd.coolant_temp", "req": "01 05",
         "scale": 1.0,   "offset": -40.0},
        {"key": "obd.tps",         "req": "01 11",
         "scale": 100.0/255.0, "offset": 0.0},
        {"key": "obd.load",        "req": "01 04",
         "scale": 100.0/255.0, "offset": 0.0},
        {"key": "obd.intake_temp", "req": "01 0F", "scale": 1.0,   "offset": -40.0},
        {"key": "obd.battery",     "req": "01 42", "scale": 0.001, "offset": 0.0},
    ]
    if p.get("has_boost"):
        std_signals.append({"key": "obd.boost", "req": "01 0B", "scale": 1.0, "offset": -101.3,
                            "_note": "MAP absolute kPa − 101.3 ≈ boost gauge kPa"})

    # CAN broadcast sources (ZN/C6 CAN)
    can_sources = _can_rules_to_sources(p.get("can_rules") or [])
    can_by_signal = {s["signal"]: s for s in can_sources}

    for s in std_signals:
        sig_entry: dict = {"key": s["key"]}
        src_list = []
        # CAN broadcast takes priority when present
        if s["key"] in can_by_signal:
            src_list.append(can_by_signal[s["key"]])
        src_list.append(
            {"req": s["req"], "scale": s["scale"], "offset": s["offset"]})
        if "_note" in s:
            src_list[-1]["_note"] = s["_note"]
        sig_entry["src"] = src_list
        signals.append(sig_entry)

    # Oil temperature
    oil_srcs = _oil_sources(p)
    if not oil_srcs:
        oil_srcs = [{"req": "01 5C", "scale": 1.0, "offset": -40.0}]
    signals.append({"key": "obd.oil_temp", "src": oil_srcs})

    # Oil pressure (Mode 22 DID, if present)
    op_src = _oil_pressure_source(p)
    if op_src:
        signals.append({"key": "obd.oil_pressure", "src": [op_src]})
    else:
        # Default analog via external sensor (firmware ADS1115 path)
        signals.append({"key": "obd.oil_pressure",
                        "src": [{"req": "adc", "_note": "external ADS1115 sensor"}]})

    # Gear
    gear_src = _gear_source(p)
    if gear_src:
        signals.append({"key": "obd.gear", "src": [gear_src]})

    # Drivetrain — gear ratios as list of {gear, ratio} objects
    gear_def = []
    if gear_count > 0:
        for g in range(1, gear_count + 1):
            if g < len(ratios):
                gear_def.append({"gear": g, "ratio": round(ratios[g], 6)})

    profile_v1: dict = {
        "schema": "skygauge.vehicle/1",
        "id": vid,
        "rev": 1,
        "name": name,
        "drivetrain": {
            "final_drive": round(p["final_drive_ratio"], 6),
            "tire_radius_m": round(p["tire_rolling_radius_m"], 6),
            "gear_tolerance": round(p["gear_tolerance"], 4),
            "gears": gear_def,
        },
        "bus": bus,
        "signals": signals,
    }

    # Add can_broadcast flag for ATMA profiles
    if p.get("can_broadcast_mode"):
        profile_v1["bus"]["can_broadcast"] = True

    return profile_v1


# ------------------------------------------------------------------ #
#  CLI                                                                 #
# ------------------------------------------------------------------ #

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="JSON output from dump_legacy")
    ap.add_argument("--out", default=".", metavar="DIR",
                    help="output directory (default: current directory)")
    ap.add_argument("--skip-generic", action="store_true", default=True,
                    help="skip OBD2 Generic (built-in, default: true)")
    ap.add_argument("--include-generic", dest="skip_generic", action="store_false",
                    help="also emit obd2.generic.vehicle.json")
    args = ap.parse_args()

    with open(args.input, encoding="utf-8") as fh:
        legacy = json.load(fh)

    os.makedirs(args.out, exist_ok=True)
    written = []
    for p in legacy:
        name = p.get("name", "")
        if args.skip_generic and name == "OBD2 Generic":
            print(f"  skip  {name} (built-in generic)", file=sys.stderr)
            continue
        v1 = convert(p)
        vid = v1["id"]
        out_path = os.path.join(args.out, f"{vid}.vehicle.json")
        with open(out_path, "w", encoding="utf-8") as fh:
            json.dump(v1, fh, indent=2, ensure_ascii=False)
            fh.write("\n")
        written.append(out_path)
        print(f"  wrote {out_path}", file=sys.stderr)

    print(f"\nWrote {len(written)} profile(s) to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
