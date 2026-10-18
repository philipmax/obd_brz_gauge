#!/usr/bin/env python3
"""
legacy_golden.py - write the legacy-behaviour golden fixtures
test/host/vehicle/fixtures/golden/<id>.json.

Usage:
    python3 tools/vehicles/legacy_golden.py            # (re)write the fixtures
    python3 tools/vehicles/legacy_golden.py --check    # exit 1 if one is stale

Each fixture describes what the pre-refactor firmware (dump_legacy.LEGACY_REV)
sent for one legacy vehicle profile: the ELM327 init sequence and the poll
requests per class, simulated from the legacy tables with the logic of
elm327_ble_client.c (do_elm_init, the 12-slot poll and its oil-temperature
paths). This script deliberately does not share code with legacy_to_v1.py:
test_golden.c compiles the catalog profile and compares it with the fixture,
so a conversion mistake shows up as a mismatch.

Intentional differences are applied on top of the simulation in DEVIATIONS
and recorded in the fixture's "notes". The format is specified in
test/host/vehicle/fixtures/golden/README.md.
"""

import argparse
import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))

import dump_legacy  # noqa: E402
from legacy_to_v1 import KNOWN_IDS, dumps  # noqa: E402  (id map and formatting only)

GOLDEN_DIR = REPO_ROOT / "test" / "host" / "vehicle" / "fixtures" / "golden"

OBD_POLL_SLOT_GAP_MS = 30
ZC6_PROBE = {"mode": "probe", "every_ms": 500, "window_ms": 120, "stale_ms": 15000}

# OIL_TEMP_MODE_* -> (Mode 22 DID or the full request, wrapped in ATSH7E0 ... fixed header)
OIL_MODE_REQ = {
    0: ("015C", False), 1: ("221017", False), 2: ("2101", False), 3: ("22111F", False),
    4: ("221310", False), 5: ("225822", False), 6: ("224402", False), 7: ("2203F3", True),
    8: ("224402", True), 9: ("22D002", True), 10: ("22111F", True),
}
CAN_CHANNEL_SIGNAL = {
    "rpm": "obd.rpm", "speed": "obd.speed", "oil_temp": "obd.oil_temp",
    "coolant": "obd.coolant_temp", "tps": "obd.throttle", "load": "obd.load",
    "intake": "obd.intake_temp", "boost": "obd.boost", "gear": "obd.gear",
}


def _arg(cmd, prefix):
    if not cmd:
        return None
    s = cmd.replace("\\r", "").replace("\r", "").replace(" ", "").upper()
    assert s.startswith(prefix), cmd
    return s[len(prefix):]


def _num(x):
    x = round(float(x), 6)
    return int(x) if x == int(x) else x


def simulate(p):
    """Legacy init + poll requests of one merged legacy profile."""
    func, b29 = bool(p["obd_functional_addr"]), bool(p["obd_29bit_functional"])
    fixed = ("18DB33F1" if b29 else "7DF") if func else "7E0"
    atst = p["obd_timeout"] or 0x19
    init = ["ATZ", "ATE0", "ATL0", "ATS1", "ATH0", "ATAT1", "ATST%02X" % atst,
            "ATSP%X" % p["forced_protocol"], "ATSH" + fixed]

    channels = {r["channel"] for r in (p["can_rules"] or [])} if p["can_broadcast_mode"] else set()
    assert "rpm" not in channels, "ATMA-primary CAN mode is not simulated"
    uds_hdr = _arg(p["uds_header_cmd"], "ATSH")
    reqs = []

    def add(sig, chain):
        for i, (hdr, tx, extra) in enumerate(chain):
            r = {"sig": sig, "src": i, "hdr": hdr, "tx": tx,
                 "class": "fast" if sig == "obd.rpm" else "round"}
            r.update(extra)
            reqs.append(r)

    add("obd.rpm", [(fixed, "010C", {})])                      # slot 0 + insert after every slot
    add("obd.intake_temp", [(fixed, "010F", {})])              # 1
    add("obd.speed", [(fixed, "010D", {})])                    # 2
    if "coolant" not in channels:
        add("obd.coolant_temp", [(fixed, "0105", {})])         # 3
    add("obd.load", [(fixed, "0104", {})])                     # 4
    if "tps" not in channels:
        add("obd.throttle", [(fixed, "0111", {})])             # 5
    if "oil_temp" not in channels:                             # 6
        chain = []
        if p["oil_primary"]:                                   # s_oil_use_override
            for f in (p["oil_primary"], p["oil_secondary"]):
                if not f:
                    continue
                pid = f["pid"].replace(" ", "").upper()
                if f["type"] == "special":
                    assert f["special_id"] == 0
                    item = (fixed, "2101", {"multi": True})
                elif f["type"] == "std_pid":                   # uds_header_cmd wraps any type
                    item = (uds_hdr or fixed, "01" + pid, {})
                else:
                    phys = not uds_hdr and p["ov_functional_addr"]
                    item = (uds_hdr or ("7E0" if phys else fixed), "22" + pid, {})
                if item not in chain:
                    chain.append(item)
        else:                                                  # get_next_oil_query_mode
            st = p["legacy_oil_strategy"]
            for key in ("primary", "secondary", "tertiary", "quaternary"):
                if st[key] == 0xFF:
                    continue
                tx, phys = OIL_MODE_REQ[st[key]]
                item = ("7E0" if phys else fixed, tx, {"multi": True} if tx == "2101" else {})
                if item not in chain:
                    chain.append(item)
        add("obd.oil_temp", chain)
    add("obd.battery_voltage", [(fixed, "0142", {})])          # 7
    if p["has_boost"]:
        add("obd.boost", [(fixed, "010B", {})])                # 8
    add("obd.afr", [(fixed, "0144", {})])                      # 9
    if p["obd_oil_pressure_did"]:                              # 10
        did = int(p["obd_oil_pressure_did"], 16)
        add("obd.oil_pressure", [(uds_hdr or "7E0", "22%04X" % did, {})])
    if p["obd_gear_did"]:                                      # 11
        did = int(p["obd_gear_did"], 16)
        hdr = _arg(p["obd_gear_header_cmd"], "ATSH") or uds_hdr or "7E0"
        extra = {}
        raw = p["obd_gear_raw_frame"]
        if raw:
            tx = raw.replace("\\r", "").replace("\r", "").replace(" ", "").upper()
            extra["raw"] = True
        else:
            tx = "22%04X" % did
        rx = _arg(p["obd_gear_rx_filter_cmd"], "ATCRA")
        if rx:
            extra["rx"] = rx
        add("obd.gear", [(hdr, tx, extra)])

    can = []
    for r in (p["can_rules"] or []) if channels else []:
        cid = int(r["can_id"], 16)
        can.append({"sig": CAN_CHANNEL_SIGNAL[r["channel"]],
                    "id": ("%03X" % cid) if cid <= 0x7FF else ("%08X" % cid),
                    "bit": r["bit_off"], "bits": r["bit_len"]})

    dt = {"fd": _num(p["final_drive_ratio"]), "tire_m": _num(p["tire_rolling_radius_m"]),
          "gears": [_num(g) for g in p["gear_ratios"][1:p["gear_count"] + 1]],
          "tol": _num(p["gear_tolerance"])}

    return {
        "init": init,
        "gap_ms": p["poll_gap_ms"] or OBD_POLL_SLOT_GAP_MS,
        "requests": reqs,
        "requires": ["ATMA"] if can else [],
        "monitor": dict(ZC6_PROBE) if can else None,
        "can": can,
        "drivetrain": dt,
    }


# ------------------------------------------------------------------ #
#  intentional differences                                            #
# ------------------------------------------------------------------ #

def _dev_r55(g):
    for r in g["requests"]:
        if r["tx"] == "225822" and r["hdr"] == "7DF":
            r["hdr"] = "7E0"
            return ("225822: legacy sent it on the functional header 7DF (enum path, no ATSH7E0 "
                    "wrap); v1 refuses service 22 on a functional header, so it goes to 7E0.")
    raise AssertionError("R55 deviation no longer applies")


DEVIATIONS = {
    "mini.r55": [_dev_r55],
}


def common_notes(p, g):
    notes = []
    if p["forced_protocol"] == 0:
        notes.append("ATSP0: bus.proto is 0. Legacy sent ATSP with the saved or auto-detected "
                     "protocol (nvs_user_cfg_t.protocol); v1 does the same (vp_engine_cfg_t.proto).")
    if p["has_boost"]:
        notes.append("obd.boost: legacy truncated (MAP-100)/10 to 0.1 bar; v1 stores 0.01 bar.")
    if g["can"]:
        notes.append("CAN coolant/oil after stale_ms without frames: legacy wrote coolant -40 and "
                     "oil invalid; v1 invalidates both.")
    if p["obd_gear_did"]:
        notes.append("obd.gear: legacy fell back to the ratio gear on every out-of-range sample; "
                     "v1 uses fail_after 1 with fallback ratio.")
    return notes


def build_all(rev):
    legacy = dump_legacy.load(REPO_ROOT, rev=rev or None)
    files = {}
    for idx, p in enumerate(legacy):
        vid = KNOWN_IDS[p["name"]]
        g = simulate(p)
        notes = common_notes(p, g)
        for dev in DEVIATIONS.get(vid, []):
            notes.append(dev(g))
        doc = {"id": vid, "legacy": {"rev": rev or "working tree", "idx": idx, "name": p["name"]}}
        doc.update(g)
        doc["notes"] = notes
        files[vid + ".json"] = dumps(doc) + "\n"
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(GOLDEN_DIR), metavar="DIR")
    ap.add_argument("--rev", default=dump_legacy.LEGACY_REV,
                    help="git revision of the legacy tables ('' = working tree)")
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if a fixture in --out differs")
    args = ap.parse_args()

    files = build_all(args.rev)
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
                print("legacy_golden: stale: %s" % path, file=sys.stderr)
                stale += 1
            continue
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    if args.check:
        if stale:
            print("legacy_golden: %d stale fixture(s); run python3 tools/vehicles/legacy_golden.py"
                  % stale, file=sys.stderr)
            return 1
        print("legacy_golden: %d fixture(s) up to date" % len(files))
        return 0
    print("legacy_golden: wrote %d fixture(s) to %s" % (len(files), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
