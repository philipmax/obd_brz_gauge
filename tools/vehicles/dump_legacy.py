#!/usr/bin/env python3
"""
dump_legacy.py — parse vehicle_profiles.c and vehicle_custom_config.h and
emit an intermediate JSON array for legacy_to_v1.py.

Usage:
    python3 tools/vehicles/dump_legacy.py > /tmp/legacy.json   # from git 4b056c0
    # or read files from disk:
    python3 tools/vehicles/dump_legacy.py --rev '' \
        --profiles /tmp/vehicle_profiles.c \
        --overrides /tmp/vehicle_custom_config.h

No C compiler, ESP-IDF, or FreeRTOS required.  The parser is intentionally
simple: it looks for the known struct patterns rather than being a full C parser.
If the source file changes structurally, update the patterns here.
"""

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Optional

# ------------------------------------------------------------------ #
#  OIL_TEMP_MODE enum values (must match vehicle_profiles.h)         #
# ------------------------------------------------------------------ #

OIL_TEMP_MODE = {
    "OIL_TEMP_MODE_NONE": 0xFF,
    "OIL_TEMP_MODE_PID_5C": 0,
    "OIL_TEMP_MODE_UDS_22_10_17": 1,
    "OIL_TEMP_MODE_TOYOTA_21_01": 2,
    "OIL_TEMP_MODE_MAZDA_22_111F": 3,
    "OIL_TEMP_MODE_MAZDA_22_1310": 4,
    "OIL_TEMP_MODE_MINI_22_5822": 5,
    "OIL_TEMP_MODE_BMW_22_4402": 6,
    "OIL_TEMP_MODE_BMW_22_03F3": 7,
    "OIL_TEMP_MODE_BMW_G_22_4402": 8,
    "OIL_TEMP_MODE_BMW_22_D002": 9,
    "OIL_TEMP_MODE_BMW_22_111F": 10,
}


# ------------------------------------------------------------------ #
#  Generic C literal parsers                                          #
# ------------------------------------------------------------------ #

def _parse_float(s: str) -> float:
    s = s.strip().rstrip("f")
    # Handle simple division expressions like 100.0/255 or 191.25/255.0
    if "/" in s:
        parts = s.split("/", 1)
        try:
            return float(parts[0].strip().rstrip("f")) / \
                float(parts[1].strip().rstrip("f"))
        except (ValueError, ZeroDivisionError):
            return 0.0
    try:
        return float(s)
    except ValueError:
        return 0.0


def _parse_int(s: str) -> int:
    s = s.strip()
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    try:
        return int(s)
    except ValueError:
        return 0


def _parse_bool(s: str) -> bool:
    return s.strip().lower() == "true"


def _field(text: str, name: str, default=None):
    """Extract .name = value from a struct literal fragment."""
    # Handles: .name = value, .name = {val}, .name = "str"
    pat = rf"\.{re.escape(name)}\s*=\s*([^,\n}}]+)"
    m = re.search(pat, text)
    if not m:
        return default
    return m.group(1).strip()


# ------------------------------------------------------------------ #
#  Parse vehicle_profiles.c → list of profile dicts                  #
# ------------------------------------------------------------------ #

def _remove_comments(src: str) -> str:
    """Strip // and /* */ comments to simplify parsing."""
    # Remove /* ... */
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.DOTALL)
    # Remove // ...
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def _parse_gear_ratios(text: str) -> list:
    """Extract .gear_ratios = {0, a, b, ...}."""
    m = re.search(r"\.gear_ratios\s*=\s*\{([^}]*)\}", text)
    if not m:
        return []
    vals = [_parse_float(v) for v in m.group(1).split(",")]
    return vals


def _parse_oil_strategy(text: str) -> dict:
    """Extract the oil_temp_strategy struct."""
    m = re.search(r"\.oil_temp_strategy\s*=\s*\{([^}]*)\}", text)
    if not m:
        return {
            "primary": 0xFF,
            "secondary": 0xFF,
            "tertiary": 0xFF,
            "quaternary": 0xFF}
    inner = m.group(1)

    def get_mode(field):
        fm = re.search(rf"\.{field}\s*=\s*(\w+)", inner)
        if not fm:
            return 0xFF
        return OIL_TEMP_MODE.get(fm.group(1), 0xFF)
    return {
        "primary": get_mode("primary"),
        "secondary": get_mode("secondary"),
        "tertiary": get_mode("tertiary"),
        "quaternary": get_mode("quaternary"),
    }


def parse_profiles(src: str) -> list:
    """Parse the s_profiles[] array from vehicle_profiles.c source."""
    src = _remove_comments(src)

    # Find the s_profiles[] array body
    m = re.search(r"s_profiles\s*\[\s*\]\s*=\s*\{(.+?)^\s*\}\s*;", src,
                  re.DOTALL | re.MULTILINE)
    if not m:
        raise ValueError("Could not find s_profiles[] in vehicle_profiles.c")

    array_body = m.group(1)

    # Split into individual profile blocks at top-level { }
    profiles = []
    depth = 0
    start = None
    for i, ch in enumerate(array_body):
        if ch == "{":
            if depth == 0:
                start = i + 1
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None:
                block = array_body[start:i]
                p = _parse_profile_block(block)
                if p.get("name"):
                    profiles.append(p)
                start = None
    return profiles


def _parse_profile_block(block: str) -> dict:
    def flt(name, default=0.0):
        v = _field(block, name)
        return _parse_float(v) if v else default

    def integer(name, default=0):
        v = _field(block, name)
        return _parse_int(v) if v else default

    def boolean(name, default=False):
        v = _field(block, name)
        return _parse_bool(v) if v else default

    def string(name, default=""):
        v = _field(block, name)
        if v is None:
            return default
        # Strip quotes if present
        v = v.strip().strip('"')
        return v

    name = string("name")
    gear_count = integer("gear_count")
    gear_ratios = _parse_gear_ratios(block)

    # hex fields
    obd_gear_did = 0
    m = re.search(r"\.obd_gear_did\s*=\s*(0x[0-9A-Fa-f]+)", block)
    if m:
        obd_gear_did = int(m.group(1), 16)

    obd_oil_pressure_did = 0
    m = re.search(r"\.obd_oil_pressure_did\s*=\s*(0x[0-9A-Fa-f]+)", block)
    if m:
        obd_oil_pressure_did = int(m.group(1), 16)

    obd_timeout = 0
    m = re.search(r"\.obd_timeout\s*=\s*(0x[0-9A-Fa-f]+|\d+)", block)
    if m:
        v = m.group(1)
        obd_timeout = int(v, 16 if v.startswith("0x") else 10)

    return {
        "name": name,
        "final_drive_ratio": flt("final_drive_ratio"),
        "tire_rolling_radius_m": flt("tire_rolling_radius_m"),
        "gear_count": gear_count,
        "gear_ratios": gear_ratios,
        "gear_tolerance": flt("gear_tolerance"),
        "forced_protocol": integer("forced_protocol"),
        "obd_functional_addr": boolean("obd_functional_addr"),
        "obd_29bit_functional": boolean("obd_29bit_functional"),
        "obd_timeout": obd_timeout,
        "poll_gap_ms": integer("poll_gap_ms"),
        "has_boost": boolean("has_boost"),
        "can_broadcast_mode": boolean("can_broadcast_mode"),
        "obd_gear_did": f"0x{obd_gear_did:04X}" if obd_gear_did else None,
        "obd_oil_pressure_did": f"0x{obd_oil_pressure_did:04X}" if obd_oil_pressure_did else None,
        "legacy_oil_strategy": _parse_oil_strategy(block),
    }


# ------------------------------------------------------------------ #
#  Parse vehicle_custom_config.h → dict of overrides keyed by name  #
# ------------------------------------------------------------------ #

OIL_TYPE_MAP = {
    "OIL_STD_PID": "std_pid",
    "OIL_UDS_22": "uds_22",
    "OIL_SPECIAL": "special",
}

# Known formula variable names → parsed formula dicts
# We parse the static formula definitions at the top of the file.


def _parse_oil_formula_defs(src: str) -> dict:
    """Return {variable_name: formula_dict} for each oil_formula_t definition.

    Uses a brace-depth-aware scanner so that nested braces in the pid byte
    array (e.g. ``{0x5C}``) don't confuse the outer struct boundary.
    """
    formulas = {}
    for hdr_m in re.finditer(
        r"static\s+const\s+oil_formula_t\s+(\w+)\s*=\s*", src
    ):
        varname = hdr_m.group(1)
        brace_pos = src.find("{", hdr_m.end())
        if brace_pos == -1:
            continue
        # Walk forward tracking depth to find the matching outer '}'
        depth = 0
        end = brace_pos
        for idx in range(brace_pos, len(src)):
            if src[idx] == "{":
                depth += 1
            elif src[idx] == "}":
                depth -= 1
                if depth == 0:
                    end = idx
                    break
        outer_body = src[brace_pos + 1: end]

        # type enum
        tm = re.search(r"(OIL_STD_PID|OIL_UDS_22|OIL_SPECIAL)", outer_body)
        ftype = OIL_TYPE_MAP.get(tm.group(1), "std_pid") if tm else "std_pid"

        # pid bytes from the inner { 0xAA, 0xBB, ... }
        pm = re.search(r"\{([^}]*)\}", outer_body)
        pid_bytes = []
        if pm:
            for pb in pm.group(1).split(","):
                pb = pb.strip()
                if pb:
                    try:
                        pid_bytes.append(
                            int(pb, 16 if pb.startswith("0x") else 10)
                        )
                    except ValueError:
                        pass
        pid_str = " ".join(f"{b:02X}" for b in pid_bytes)
        pid_len = len(pid_bytes)

        # Scalars after the inner brace close:
        #   index 0 = pid_len, 1 = resp_byte, 2 = resp_bytes,
        #   3 = scale, 4 = offset, 5 = special_id
        inner_end = outer_body.rfind("}")
        # extract text after the inner closing brace
        start = inner_end + 1 if inner_end != -1 else 0
        after_brace = outer_body[start:]
        scalars = [t.strip() for t in after_brace.split(",") if t.strip()]

        def get_scalar(i, default):
            # literals like 1.0f, -40.0f and 191.25f/255.0f
            if i < len(scalars):
                try:
                    num, _, den = scalars[i].partition("/")
                    v = float(num.strip().rstrip("fF"))
                    return v / float(den.strip().rstrip("fF")) if den else v
                except (ValueError, ZeroDivisionError):
                    pass
            return default

        formulas[varname] = {
            "type": ftype,
            "pid": pid_str,
            "pid_len": pid_len,
            "resp_byte": int(get_scalar(1, 0)),
            "resp_bytes": int(get_scalar(2, 1)),
            "scale": get_scalar(3, 1.0),
            "offset": get_scalar(4, 0.0),
            "special_id": int(get_scalar(5, 0)),
        }
    return formulas


def _parse_can_rule_defs(src: str) -> dict:
    """Return {variable_name: [rule_dict, ...]} for each can_rule_t array."""
    arrays = {}
    for m in re.finditer(
        r"static\s+const\s+can_rule_t\s+(\w+)\s*\[\s*\]\s*=\s*\{([^;]+)\}\s*;",
        src, re.DOTALL
    ):
        varname = m.group(1)
        body = m.group(2)
        rules = []
        # Each rule: { can_id, bit_off, bit_len, scale, offset, channel }
        for rm in re.finditer(r"\{([^}]*)\}", body):
            parts = [p.strip() for p in rm.group(1).split(",")]
            if len(parts) < 6:
                continue
            can_id_s = parts[0].strip()
            base = 16 if can_id_s.startswith("0x") else 10
            can_id = int(can_id_s, base)
            ch_raw = parts[5].strip()
            ch_map = {
                "CH_RPM": "rpm",
                "CH_SPEED": "speed",
                "CH_OIL_TEMP": "oil_temp",
                "CH_COOLANT": "coolant",
                "CH_TPS": "tps",
                "CH_LOAD": "load",
                "CH_INTAKE": "intake",
                "CH_BOOST": "boost",
                "CH_GEAR": "gear",
            }
            rules.append({
                "can_id": f"0x{can_id:03X}",
                "bit_off": int(parts[1]),
                "bit_len": int(parts[2]),
                "scale": _parse_float(parts[3]),
                "offset": _parse_float(parts[4]),
                "channel": ch_map.get(ch_raw, ch_raw),
            })
        arrays[varname] = rules
    return arrays


def _strip_string(s: Optional[str]) -> Optional[str]:
    if s is None:
        return None
    s = s.strip()
    if s == "NULL":
        return None
    return s.strip('"').replace("\\r", "\r").rstrip("\r")


def parse_overrides(src: str) -> dict:
    """Parse s_vehicle_overrides[] → {match_name: override_dict}."""
    src_clean = _remove_comments(src)

    formulas = _parse_oil_formula_defs(src_clean)
    can_arrays = _parse_can_rule_defs(src_clean)

    # Find s_vehicle_overrides array
    m = re.search(
        r"s_vehicle_overrides\s*\[\s*\]\s*=\s*\{(.+?)^\s*\}\s*;",
        src_clean, re.DOTALL | re.MULTILINE
    )
    if not m:
        return {}

    array_body = m.group(1)

    overrides = {}
    depth = 0
    start = None
    for i, ch in enumerate(array_body):
        if ch == "{":
            if depth == 0:
                start = i + 1
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None:
                block = array_body[start:i]
                ov = _parse_override_block(block, formulas, can_arrays)
                name = ov.get("match_name")
                if name:
                    overrides[name] = ov
                start = None
    return overrides


def _parse_override_block(
        block: str,
        formulas: dict,
        can_arrays: dict) -> dict:
    def fstr(name):
        m = re.search(rf'\.{re.escape(name)}\s*=\s*"([^"]*)"', block)
        return m.group(1) if m else None

    def fptr(name):
        """Get the variable name a pointer field points to (or None for NULL)."""
        m = re.search(rf'\.{re.escape(name)}\s*=\s*&?(\w+)', block)
        if not m:
            return None
        v = m.group(1)
        return None if v == "NULL" else v

    def fbool(name):
        m = re.search(rf'\.{re.escape(name)}\s*=\s*(true|false)', block)
        return m.group(1) == "true" if m else False

    def fhex(name):
        pat = rf'\.{re.escape(name)}\s*=\s*(0x[0-9A-Fa-f]+|\d+)'
        m = re.search(pat, block)
        if not m:
            return 0
        v = m.group(1)
        return int(v, 16) if v.startswith("0x") else int(v)

    match_name_m = re.search(r'\.match_name\s*=\s*"([^"]*)"', block)
    if not match_name_m:
        return {}

    can_ptr = fptr("can_rules")
    oil_pri_ptr = fptr("oil_primary")
    oil_sec_ptr = fptr("oil_secondary")

    return {
        "match_name": match_name_m.group(1),
        "can_rules": can_arrays.get(can_ptr, []) if can_ptr else [],
        "oil_primary": formulas.get(oil_pri_ptr) if oil_pri_ptr else None,
        "oil_secondary": formulas.get(oil_sec_ptr) if oil_sec_ptr else None,
        "forced_protocol": fhex("forced_protocol"),
        "obd_functional_addr": fbool("functional_addr"),
        "obd_timeout": fhex("obd_timeout"),
        "has_boost": fbool("has_boost"),
        "poll_gap_ms": fhex("poll_gap_ms"),
        "uds_header_cmd": fstr("uds_header_cmd"),
        "obd_gear_header_cmd": fstr("obd_gear_header_cmd"),
        "obd_gear_rx_filter_cmd": fstr("obd_gear_rx_filter_cmd"),
        "obd_gear_raw_frame": fstr("obd_gear_raw_frame"),
    }


# ------------------------------------------------------------------ #
#  Merge profiles + overrides → output array                         #
# ------------------------------------------------------------------ #

def merge(profiles: list, overrides: dict) -> list:
    """Combine each profile with its override the way the legacy firmware did.

    elm327_ble_client.c took only obd_timeout and poll_gap_ms from the
    override (when non-zero); forced_protocol, functional addressing and
    has_boost always came from the profile table. The override's own
    functional_addr flag only decided whether a UDS 22 oil formula was
    wrapped in ATSH7E0 / ATSH7DF, so it is kept as ov_functional_addr.
    """
    out = []
    for p in profiles:
        ov = overrides.get(p["name"], {})
        merged = dict(p)
        for key in ("obd_timeout", "poll_gap_ms"):
            if ov.get(key):
                merged[key] = ov[key]
        merged["ov_functional_addr"] = bool(ov.get("obd_functional_addr"))
        merged["has_override"] = bool(ov)
        merged["can_rules"] = ov.get("can_rules", [])
        merged["oil_primary"] = ov.get("oil_primary")
        merged["oil_secondary"] = ov.get("oil_secondary")
        merged["uds_header_cmd"] = ov.get("uds_header_cmd")
        merged["obd_gear_header_cmd"] = ov.get("obd_gear_header_cmd")
        merged["obd_gear_rx_filter_cmd"] = ov.get("obd_gear_rx_filter_cmd")
        merged["obd_gear_raw_frame"] = ov.get("obd_gear_raw_frame")
        out.append(merged)
    return out


# ------------------------------------------------------------------ #
#  CLI                                                                 #
# ------------------------------------------------------------------ #

LEGACY_REV = "4b056c0"   # last commit with the compile-time profile table
PROFILES_PATH = "main/app_obd_dsp/vehicle_profiles.c"
OVERRIDES_PATH = "main/app_obd_dsp/vehicle_custom_config.h"


def read_source(repo_root: Path, path: str, rev: Optional[str]) -> str:
    if rev:
        import subprocess
        return subprocess.run(["git", "-C", str(repo_root), "show", f"{rev}:{path}"],
                              check=True, capture_output=True, text=True).stdout
    return Path(path).read_text(encoding="utf-8")


def load(repo_root: Path, rev: Optional[str] = LEGACY_REV,
         profiles: str = PROFILES_PATH, overrides: str = OVERRIDES_PATH) -> list:
    """Parsed + merged legacy table (importable by the other tools)."""
    return merge(parse_profiles(read_source(repo_root, profiles, rev)),
                 parse_overrides(read_source(repo_root, overrides, rev)))


def main():
    repo_root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rev", default=LEGACY_REV,
                    help="git revision to read the legacy sources from "
                         f"(default {LEGACY_REV}); '' reads --profiles/--overrides from disk")
    ap.add_argument("--profiles", default=PROFILES_PATH)
    ap.add_argument("--overrides", default=OVERRIDES_PATH)
    ap.add_argument("--out", default="-", help="output file path, - for stdout")
    args = ap.parse_args()

    merged = load(repo_root, args.rev or None, args.profiles, args.overrides)
    result = json.dumps(merged, indent=2, ensure_ascii=False) + "\n"
    if args.out == "-":
        sys.stdout.write(result)
    else:
        Path(args.out).write_text(result, encoding="utf-8")
        print(f"Wrote {len(merged)} profiles to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
