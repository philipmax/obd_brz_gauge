#!/usr/bin/env python3
"""
legacy_to_v1.py - generate vehicles/catalog/*.vehicle.json from the legacy
compile-time vehicle tables.

Usage:
    python3 tools/vehicles/legacy_to_v1.py            # (re)write vehicles/catalog/
    python3 tools/vehicles/legacy_to_v1.py --check    # exit 1 if a file is stale
    python3 tools/vehicles/legacy_to_v1.py --out DIR  # write somewhere else

The legacy tables (main/app_obd_dsp/vehicle_profiles.c and
vehicle_custom_config.h) are read from git at dump_legacy.LEGACY_REV through
dump_legacy.load(); --rev '' reads the working tree instead. Every legacy
entry becomes one skygauge.vehicle/1 profile in the layout of
contracts/vehicle-rpn.md:

  - signals in legacy poll-slot order (010C 010F 010D 0105 0104 0111 oil
    0142 010B 0144 oil-pressure gear), obd.rpm with rate "every" (the legacy
    RPM insert after every slot);
  - oil temperature: the override formulas (oil_primary/oil_secondary) when
    the profile has them, otherwise the enum strategy chain; 22xx requests on
    a functional bus get the physical header the legacy code switched to;
  - CAN broadcast rules (ZN/C6 CAN) become monitor-probe CAN sources and
    replace the OBD request of the same signal, as the legacy poll skipped it;
  - drivetrain from the gear table; obd.gear derived from the ratio unless
    the profile reads a gear DID.

Intentional differences from the legacy behaviour are written into the
profile's "notes" and listed in test/host/vehicle/fixtures/golden/README.md.
"""

import argparse
import json
import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))

import dump_legacy  # noqa: E402

SCHEMA = "skygauge.vehicle/1"
CATALOG_DIR = REPO_ROOT / "vehicles" / "catalog"

KNOWN_IDS = {
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

DEFAULT_HDR = "7E0"
DEFAULT_ATST = 0x19
OIL_SIG_VALID = [-20, 150]     # on_parsed_oil_temp drops anything outside
OIL_SRC_VALID = [-40, 215]     # 5C / formula / 22xx parsers count these as failures

# legacy OIL_TEMP_MODE_* -> (request, data len, expr, rpn, source valid, physical header)
_A2 = [["A", 0], ["k", 256], "*", ["A", 1], "+"]
OIL_ENUM = {
    0:  ("015C",   1, "A-40", [["A", 0], ["k", 40], "-"], OIL_SRC_VALID, False),
    3:  ("22111F", 1, "A-50", [["A", 0], ["k", 50], "-"], OIL_SRC_VALID, False),
    4:  ("221310", 2, "(A*256+B)/100-40", _A2 + [["k", 100], "/", ["k", 40], "-"], OIL_SRC_VALID, False),
    5:  ("225822", 1, "A-60", [["A", 0], ["k", 60], "-"], OIL_SRC_VALID, False),
    6:  ("224402", 2, "B-64", [["A", 1], ["k", 64], "-"], OIL_SRC_VALID, False),
    7:  ("2203F3", 1, "A-40", [["A", 0], ["k", 40], "-"], OIL_SRC_VALID, True),
    8:  ("224402", 2, "(A*256+B)*0.75-48", _A2 + [["k", 0.75], "*", ["k", 48], "-"], [-48, 143], True),
    9:  ("22D002", 2, "(A*256+B)*0.75-48", _A2 + [["k", 0.75], "*", ["k", 48], "-"], [-48, 143], True),
    10: ("22111F", 1, "A-50", [["A", 0], ["k", 50], "-"], OIL_SRC_VALID, True),
}
OIL_MODE_TOYOTA_21_01 = 2

CAN_CHANNEL_SIGNAL = {
    "rpm": "obd.rpm", "speed": "obd.speed", "oil_temp": "obd.oil_temp",
    "coolant": "obd.coolant_temp", "tps": "obd.throttle", "load": "obd.load",
    "intake": "obd.intake_temp", "boost": "obd.boost", "gear": "obd.gear",
}

# Per-profile notes on intentional differences (keyed by catalog id).
EXTRA_NOTES = {
    "mini.r55": ["225822 goes to the physical header 7E0: the legacy enum path sent it on the "
                 "functional header 7DF, which v1 refuses for service 22."],
    "subaru.zd8.obd": ["Same polling as subaru.zd8.pid; both legacy entries are kept so the "
                       "legacy profile index maps 1:1."],
    "subaru.zd8.pid": ["Same polling as subaru.zd8.obd; both legacy entries are kept so the "
                       "legacy profile index maps 1:1."],
}


class ConvertError(Exception):
    pass


# ------------------------------------------------------------------ #
#  helpers                                                            #
# ------------------------------------------------------------------ #

def _cmd_arg(cmd, prefix):
    """'ATSH6F1\\r' -> '6F1' (accepts a literal backslash-r or a CR)."""
    if not cmd:
        return None
    s = cmd.replace("\\r", "").replace("\r", "").strip()
    if not s.upper().startswith(prefix):
        raise ConvertError("expected %s..., got %r" % (prefix, cmd))
    return s[len(prefix):].strip().upper()


def _num(x):
    """Shortest JSON number for a legacy float (3.0 -> 3, 0.314 -> 0.314)."""
    x = round(float(x), 6)
    return int(x) if x == int(x) else x


def _letters(i):
    return "ABCDEFGH"[i] if i < 8 else "d[%d]" % i


def _formula_rpn(resp_byte, resp_bytes, scale, offset):
    """oil_formula_t -> (len, expr, rpn): raw * scale + offset, raw = A or A*256+B."""
    a = resp_byte
    if resp_bytes == 2:
        rpn = [["A", a], ["k", 256], "*", ["A", a + 1], "+"]
        expr = "(%s*256+%s)" % (_letters(a), _letters(a + 1))
        n = a + 2
    else:
        rpn = [["A", a]]
        expr = _letters(a)
        n = a + 1
    if scale != 1.0:
        rpn += [["k", _num(scale)], "*"]
        expr += "*%s" % _num(scale)
    if offset:
        rpn += [["k", _num(abs(offset))], "-" if offset < 0 else "+"]
        expr += "%s%s" % ("-" if offset < 0 else "+", _num(abs(offset)))
    return n, expr, rpn


def _req_source(req, n, expr, rpn, hdr=None, bus_hdr=DEFAULT_HDR, valid=None, **extra):
    src = {"req": req}
    if hdr and hdr != bus_hdr:
        src["hdr"] = hdr
    src["len"] = n
    src.update(extra)
    if valid:
        src["valid"] = list(valid)
    src["expr"] = expr
    src["rpn"] = rpn
    return src


def _mode21_source():
    """ZN/C6 Mode 21 01: oil temp = 5th byte from the end - 40, jump hold."""
    return {"req": "2101", "multiframe": True, "byte_from_end": 5, "valid": [-10, 150],
            "hold": {"delta": 8, "max_hold": 30, "gap_ms": 3000},
            "expr": "X-40", "rpn": [["X"], ["k", 40], "-"]}


def _can_source(rule):
    can_id = int(rule["can_id"], 16)
    src = {"can": ("%03X" % can_id) if can_id <= 0x7FF else ("%08X" % can_id),
           "bit": rule["bit_off"], "bits": rule["bit_len"]}
    rpn, expr = [["X"]], "X"
    scale, offset = rule["scale"], rule["offset"]
    if abs(scale - 100.0 / 255.0) < 1e-9:
        rpn += [["k", 100], "*", ["k", 255], "/"]
        expr += "*100/255"
    elif scale != 1.0:
        rpn += [["k", _num(scale)], "*"]
        expr += "*%s" % _num(scale)
    if offset:
        rpn += [["k", _num(abs(offset))], "-" if offset < 0 else "+"]
        expr += "%s%s" % ("-" if offset < 0 else "+", _num(abs(offset)))
    src["expr"] = expr
    src["rpn"] = rpn
    return src


# ------------------------------------------------------------------ #
#  oil temperature                                                    #
# ------------------------------------------------------------------ #

def _oil_sources(p, bus_hdr, functional):
    out = []
    uds_hdr = _cmd_arg(p.get("uds_header_cmd"), "ATSH")
    if p.get("has_override") and p.get("oil_primary"):
        for f in (p["oil_primary"], p.get("oil_secondary")):
            if not f:
                continue
            if f["type"] == "special":
                if f.get("special_id", 0) != 0 or not p["name"].startswith("ZN/C6"):
                    raise ConvertError("%s: unsupported special oil formula" % p["name"])
                src = _mode21_source()
            else:
                pid = f["pid"].replace(" ", "").upper()
                n, expr, rpn = _formula_rpn(f["resp_byte"], f["resp_bytes"], f["scale"], f["offset"])
                if f["type"] == "std_pid":    # the legacy uds_header_cmd wrap covers any type
                    src = _req_source("01" + pid, n, expr, rpn, uds_hdr, bus_hdr, OIL_SRC_VALID)
                elif f["type"] == "uds_22":
                    hdr = uds_hdr or ("7E0" if p.get("ov_functional_addr") else bus_hdr)
                    src = _req_source("22" + pid, n, expr, rpn, hdr, bus_hdr, OIL_SRC_VALID)
                else:
                    raise ConvertError("%s: unknown oil formula type %r" % (p["name"], f["type"]))
            if src not in out:
                out.append(src)
        return out

    st = p["legacy_oil_strategy"]
    for key in ("primary", "secondary", "tertiary", "quaternary"):
        mode = st[key]
        if mode == 0xFF:
            continue
        if mode == OIL_MODE_TOYOTA_21_01:
            if not p["name"].startswith("ZN/C6"):
                raise ConvertError("%s: Mode 21 oil outside ZN/C6" % p["name"])
            src = _mode21_source()
        elif mode in OIL_ENUM:
            req, n, expr, rpn, valid, phys = OIL_ENUM[mode]
            hdr = "7E0" if phys else bus_hdr
            if req.startswith("22") and functional and not phys:
                hdr = "7E0"    # see EXTRA_NOTES: v1 refuses 22 on a functional header
            src = _req_source(req, n, expr, [list(i) if isinstance(i, list) else i for i in rpn],
                              hdr, bus_hdr, valid)
        else:
            raise ConvertError("%s: unsupported oil mode %d" % (p["name"], mode))
        if src not in out:
            out.append(src)
    return out


# ------------------------------------------------------------------ #
#  one profile                                                        #
# ------------------------------------------------------------------ #

def convert(p, idx, rev=dump_legacy.LEGACY_REV):
    name = p["name"]
    if name not in KNOWN_IDS:
        raise ConvertError("no catalog id for legacy profile %r" % name)
    vid = KNOWN_IDS[name]
    functional = bool(p.get("obd_functional_addr"))
    bus_hdr = ("18DB33F1" if p.get("obd_29bit_functional") else "7DF") if functional else DEFAULT_HDR

    bus = {}
    if p.get("forced_protocol"):
        bus["proto"] = p["forced_protocol"]
    if bus_hdr != DEFAULT_HDR:
        bus["hdr"] = bus_hdr
    if p.get("obd_timeout") and p["obd_timeout"] != DEFAULT_ATST:
        bus["atst"] = p["obd_timeout"]
    if p.get("poll_gap_ms"):
        bus["gap_ms"] = p["poll_gap_ms"]

    can = {}
    if p.get("can_broadcast_mode"):
        for r in p.get("can_rules") or []:
            sig = CAN_CHANNEL_SIGNAL.get(r["channel"])
            if not sig:
                raise ConvertError("%s: unknown CAN channel %r" % (name, r["channel"]))
            can.setdefault(sig, []).append(_can_source(r))
    if "obd.rpm" in can:
        raise ConvertError("%s: CAN rpm (ATMA-primary mode) is not converted" % name)

    def std(sig_id, req, n, expr, rpn, **kw):
        s = {"id": sig_id}
        s.update(kw)
        s["src"] = can.get(sig_id) or [_req_source(req, n, expr, rpn)]
        return s

    signals = [
        std("obd.rpm", "010C", 2, "(A*256+B)/4",
            [["A", 0], ["k", 256], "*", ["A", 1], "+", ["k", 4], "/"], rate="every"),
        std("obd.intake_temp", "010F", 1, "A-40", [["A", 0], ["k", 40], "-"]),
        std("obd.speed", "010D", 1, "A", [["A", 0]],
            post=({"zero_le": 2, "zero_if_rpm_lt": 800} if can else {"zero_le": 2})),
        std("obd.coolant_temp", "0105", 1, "A-40", [["A", 0], ["k", 40], "-"]),
        std("obd.load", "0104", 1, "A*100/255", [["A", 0], ["k", 100], "*", ["k", 255], "/"]),
        std("obd.throttle", "0111", 1, "A*100/255", [["A", 0], ["k", 100], "*", ["k", 255], "/"]),
    ]

    oil = {"id": "obd.oil_temp", "valid": list(OIL_SIG_VALID)}
    if "obd.oil_temp" in can:
        oil["src"] = can["obd.oil_temp"]           # legacy CAN path bypasses the filter
    else:
        oil["filter"] = "temp_hyst"
        oil["src"] = _oil_sources(p, bus_hdr, functional)
    signals.append(oil)

    signals.append(std("obd.battery_voltage", "0142", 2, "A*256+B", [["R", 0, 16, "be"]], unit="mV"))
    if p.get("has_boost"):
        signals.append(std("obd.boost", "010B", 1, "A-100", [["A", 0], ["k", 100], "-"],
                           unit="kPa_rel", post={"clamp": [0, 155]}))
    afr = std("obd.afr", "0144", 2, "(A*256+B)*14.7/32768",
              [["R", 0, 16, "be"], ["k", 14.7], "*", ["k", 32768], "/"])
    afr["src"][0]["valid"] = [8, 22]
    afr["src"][0] = {k: afr["src"][0][k] for k in ("req", "len", "valid", "expr", "rpn")}
    signals.append(afr)

    uds_hdr = _cmd_arg(p.get("uds_header_cmd"), "ATSH")
    if p.get("obd_oil_pressure_did"):
        did = int(p["obd_oil_pressure_did"], 16)
        signals.append({"id": "obd.oil_pressure", "unit": "hPa", "post": {"clamp": [0, 20000]},
                        "src": [_req_source("22%04X" % did, 2, "A*256+B", [["R", 0, 16, "be"]],
                                            uds_hdr or "7E0", bus_hdr)]})

    gear_count = p.get("gear_count", 0)
    if p.get("obd_gear_did"):
        did = int(p["obd_gear_did"], 16)
        hdr = _cmd_arg(p.get("obd_gear_header_cmd"), "ATSH") or uds_hdr or "7E0"
        src = {}
        raw = p.get("obd_gear_raw_frame")
        if raw:
            src["raw"] = raw.replace("\\r", "").replace("\r", "").strip().upper()
        else:
            src["req"] = "22%04X" % did
        src["hdr"] = hdr
        rx = _cmd_arg(p.get("obd_gear_rx_filter_cmd"), "ATCRA")
        if rx:
            src["rx"] = rx
        src.update({"expect": "62%04X" % did, "len": 1, "valid": [0, 8], "fail_after": 1,
                    "expr": "A", "rpn": [["A", 0]]})
        gear = {"id": "obd.gear"}
        if gear_count > 0:
            gear["fallback"] = "ratio"
        gear["src"] = [src]
        signals.append(gear)
    elif gear_count > 0:
        signals.append({"id": "obd.gear", "derive": "ratio"})

    dt = {"fd": _num(p["final_drive_ratio"]), "tire_m": _num(p["tire_rolling_radius_m"])}
    if gear_count > 0:
        dt["gears"] = [_num(r) for r in p["gear_ratios"][1:gear_count + 1]]
    if p.get("gear_tolerance"):
        dt["tol"] = _num(p["gear_tolerance"])

    prof = {
        "schema": SCHEMA,
        "id": vid,
        "rev": 1,
        "name": name,
        "origin": {"from": "legacy %s" % rev if rev else "legacy (working tree)",
                   "legacy_idx": idx, "legacy_name": name},
    }
    notes = list(EXTRA_NOTES.get(vid, []))
    if notes:
        prof["notes"] = notes
    if can:
        prof["requires"] = {"elm": ["ATMA"]}
    prof["bus"] = bus
    prof["drivetrain"] = dt
    if can:
        prof["monitor"] = {"mode": "probe", "every_ms": 500, "window_ms": 120, "stale_ms": 15000}
    prof["signals"] = signals
    return prof


# ------------------------------------------------------------------ #
#  output                                                             #
# ------------------------------------------------------------------ #

def _flat(v):
    return not isinstance(v, (dict, list)) or (
        isinstance(v, list) and all(_flat(x) and not isinstance(x, dict) for x in v))


def dumps(v, ind=0):
    """JSON with objects expanded and scalar/nested-scalar arrays on one line."""
    pad = "  " * ind
    if isinstance(v, dict):
        if not v:
            return "{}"
        items = ['%s  %s: %s' % (pad, json.dumps(k), dumps(x, ind + 1)) for k, x in v.items()]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(v, list):
        if _flat(v):
            return json.dumps(v, ensure_ascii=True, separators=(", ", ": "))
        items = [pad + "  " + dumps(x, ind + 1) for x in v]
        return "[\n" + ",\n".join(items) + "\n" + pad + "]"
    return json.dumps(v, ensure_ascii=True)


def render_all(rev):
    legacy = dump_legacy.load(REPO_ROOT, rev=rev or None)
    out = {}
    for i, p in enumerate(legacy):
        prof = convert(p, i, rev)
        out[prof["id"] + ".vehicle.json"] = dumps(prof) + "\n"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(CATALOG_DIR), metavar="DIR",
                    help="output directory (default: vehicles/catalog)")
    ap.add_argument("--rev", default=dump_legacy.LEGACY_REV,
                    help="git revision of the legacy tables ('' = working tree)")
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if a file in --out differs")
    args = ap.parse_args()

    try:
        files = render_all(args.rev)
    except (ConvertError, KeyError, ValueError) as e:
        print("legacy_to_v1: error: %s" % e, file=sys.stderr)
        return 2

    stale = 0
    if not args.check:
        os.makedirs(args.out, exist_ok=True)
    for fname, text in files.items():
        path = os.path.join(args.out, fname)
        if args.check:
            try:
                with open(path, encoding="utf-8") as f:
                    cur = f.read()
            except OSError:
                cur = None
            if cur != text:
                print("legacy_to_v1: stale: %s" % path, file=sys.stderr)
                stale += 1
            continue
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    if args.check:
        if stale:
            print("legacy_to_v1: %d stale file(s); run python3 tools/vehicles/legacy_to_v1.py" % stale,
                  file=sys.stderr)
            return 1
        print("legacy_to_v1: %d profile(s) up to date" % len(files))
        return 0
    print("legacy_to_v1: wrote %d profile(s) to %s" % (len(files), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
