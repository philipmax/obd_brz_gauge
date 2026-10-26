#!/usr/bin/env python3
"""Convert the safely representable subset of Car Scanner vehicle profiles."""

import argparse
import importlib.util
import json
import math
import os
import re
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
REQUEST_RE = re.compile(r"^(?:21[0-9A-F]{2}|22[0-9A-F]{4})$", re.I)
HEADER_RE = re.compile(
    r"^(?:7DF|7E[0-7]|6[0-9A-F]{2}|18DB33F1|18DA[0-9A-F]{2}F1)$", re.I)
YEAR_RE = re.compile(
    r"(?<!\d)(19\d{2}|20\d{2})"
    r"(?:\s*(?:->|[-–—])\s*(?:(19\d{2}|20\d{2})|present(?:\s+day)?|n\.d\.)?)?"
    r"(?!\d)",
    re.I,
)
EXPR_COMPILER_PATH = os.path.join(HERE, "expr2rpn.py")
EXPR_SPEC = importlib.util.spec_from_file_location("vehicle_expr2rpn",
                                                   EXPR_COMPILER_PATH)
EXPR_COMPILER = importlib.util.module_from_spec(EXPR_SPEC)
EXPR_SPEC.loader.exec_module(EXPR_COMPILER)


def ascii_text(value, limit):
    value = unicodedata.normalize("NFKD", str(value or ""))
    value = value.encode("ascii", "ignore").decode("ascii")
    value = re.sub(r"[^ -~]", " ", value)
    value = re.sub(r"\s+", " ", value).strip()
    return value[:limit].strip() or "Imported PID"


def slug(value):
    value = unicodedata.normalize("NFKD",
                                  str(value or "")).encode("ascii",
                                                           "ignore").decode()
    value = re.sub(r"[^a-zA-Z0-9]+", ".", value).strip(".").lower()
    return value or "unknown"


def number(value, default=None):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    if not math.isfinite(value):
        return default
    return value


def vehicle_years(profile):
    text = " ".join(
        str(profile.get(key) or "") for key in ("Name", "Description"))
    found = []
    for match in YEAR_RE.finditer(text):
        first = int(match.group(1))
        end = match.group(2)
        last = int(end) if end else None
        if last is None or last >= first:
            found.append((first, last))
    if not found:
        return None, None
    end_years = [item[1] for item in found]
    year_to = None if any(year is None
                          for year in end_years) else max(end_years)
    return min(item[0] for item in found), year_to


def _split_call_args(text):
    args, start, depth = [], 0, 0
    for index, char in enumerate(text):
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            args.append(text[start:index].strip())
            start = index + 1
    args.append(text[start:].strip())
    return args


def car_scanner_expression(text):
    """Translate the arithmetic subset whose semantics match the firmware RPN."""
    text = str(text or "").strip()
    if not text:
        raise ValueError("empty_formula")
    if re.search(r"\b(?:if|FLOAT32|UNSIGNED|BYTE|WORD)\s*\(", text, re.I):
        raise ValueError("unsupported_formula_construct")

    def rewrite_calls(value):
        match = re.search(r"\b(And|Shr|SIGNED)\s*\(", value, re.I)
        if not match:
            return value
        open_at = value.find("(", match.start())
        depth, close_at = 1, open_at + 1
        while close_at < len(value) and depth:
            if value[close_at] == "(":
                depth += 1
            elif value[close_at] == ")":
                depth -= 1
            close_at += 1
        if depth:
            raise ValueError("unsupported_formula_syntax")
        args = _split_call_args(value[open_at + 1:close_at - 1])
        args = [rewrite_calls(arg) for arg in args]
        name = match.group(1).lower()
        if name == "and" and len(args) == 2:
            replacement = f"({args[0]} & {args[1]})"
        elif name == "shr" and len(args) == 2:
            replacement = f"({args[0]} >> {args[1]})"
        elif name == "signed" and len(args) == 1:
            replacement = f"sext8({args[0]})"
        else:
            raise ValueError("unsupported_formula_construct")
        return rewrite_calls(value[:match.start()] + replacement +
                             value[close_at:])

    text = rewrite_calls(text)
    # Car Scanner names response bytes A..Z, AA..AZ, etc.; the firmware
    # expression compiler addresses the same bytes with d[index].
    byte_indices = []

    def replace_byte(match):
        token = match.group(0).upper()
        index = 0
        for char in token:
            index = index * 26 + ord(char) - ord("A") + 1
        index -= 1
        byte_indices.append(index)
        return f"d[{index}]"

    text = re.sub(r"\b[A-Z]{1,2}\b", replace_byte, text, flags=re.I)
    try:
        return EXPR_COMPILER._Parser(text).parse(), max(byte_indices,
                                                        default=-1) + 1
    except (EXPR_COMPILER.ExprError, ValueError, IndexError) as error:
        raise ValueError("unsupported_formula_syntax") from error


def convert_pid(pid):
    request = str(pid.get("CMD") or "").strip().upper()
    if not REQUEST_RE.fullmatch(request):
        return None, "unsupported_request"
    if pid.get("ACT") is True:
        return None, "action_pid"
    if pid.get("BCM") or pid.get("ACM"):
        return None, "ecu_init_commands"
    formula = str(pid.get("FR") or "").strip()

    header = str(pid.get("HDR") or "").strip().upper()
    if header and not HEADER_RE.fullmatch(header):
        return None, "unsupported_header"
    if header in ("7DF", "18DB33F1"):
        return None, "functional_header_for_private_service"

    start = pid.get("SBI")
    width = pid.get("DL")
    if (isinstance(start, bool) or not isinstance(start, int) or start < 0
            or isinstance(width, bool) or not isinstance(width, int)
            or not 1 <= width <= 4 or start + width > 64):
        return None, "unsupported_data_range"

    formula_rpn = None
    formula_len = 0
    if formula:
        try:
            formula_rpn, formula_len = car_scanner_expression(formula)
        except ValueError as error:
            return None, str(error)
        if formula_len > 64:
            return None, "unsupported_data_range"

    multiplier = number(pid.get("MUL"), 0)
    divisor = number(pid.get("DIV"), 0)
    offset = number(pid.get("OFS"), 0)
    if divisor < 0 or (divisor == 0 and multiplier != 0 and multiplier != 1):
        return None, "unsupported_scaling"
    if multiplier == 0:
        if divisor != 0 or offset not in (0, 0.0):
            return None, "unsupported_scaling"
        multiplier = 1
    if any(abs(value) > 1e7 for value in (multiplier, divisor, offset)):
        return None, "unsupported_scaling"

    if formula_rpn is not None and (multiplier not in (0, 1)
                                    or divisor not in (0, 1) or offset != 0):
        return None, "formula_with_external_scaling"

    if formula_rpn is not None:
        rpn = formula_rpn
    else:
        bits = width * 8
        raw = ["R", start * 8, bits, "le" if pid.get("RBS") is True else "be"]
        if pid.get("SIG") is True:
            raw.append("s")
        rpn = [raw]
        if multiplier != 1:
            rpn.extend([["k", multiplier], "*"])
        if divisor not in (0, 1):
            rpn.extend([["k", divisor], "/"])
        if offset:
            rpn.extend([["k", offset], "+"])

    if len(rpn) > 32 or any(
            isinstance(instruction, list) and len(instruction) == 2
            and instruction[0] == "k" and abs(instruction[1]) > 1e7
            for instruction in rpn):
        return None, "unsupported_formula_complexity"

    source = {
        "req": request,
        "len": max(start + width, formula_len),
        "rpn": rpn,
    }
    if header:
        source["hdr"] = header
    name = ascii_text(pid.get("SNM") or pid.get("NM"), 23)
    signal = {
        "id":
        "veh." + slug(pid.get("SNM") or pid.get("NM")).replace(".", "_")[:27],
        "name": name,
        "rate": "round",
        "period_ms": 1000,
        "src": [source],
    }
    # Keep IDs unique even when source PIDs reuse a label; the caller adds an index.
    return signal, None


def profile_id(profile):
    brand = next(iter(profile.get("Brands") or []), "")
    return (slug(brand) + ".cs" +
            slug(profile.get("profile_key")))[:31].strip(".")


def convert_profile(profile, max_signals):
    stats = {
        "input": len(profile.get("pids") or []),
        "eligible": 0,
        "emitted": 0,
        "skipped": {}
    }
    signals = []
    for index, pid in enumerate(profile.get("pids") or []):
        signal, reason = convert_pid(pid)
        if reason:
            stats["skipped"][reason] = stats["skipped"].get(reason, 0) + 1
            continue
        stats["eligible"] += 1
        signal["id"] = signal["id"][:23] + "_" + str(index)
        if len(signals) >= max_signals:
            stats["skipped"]["signal_limit"] = stats["skipped"].get(
                "signal_limit", 0) + 1
            continue
        signals.append(signal)

    stats["emitted"] = len(signals)
    if not signals:
        return None, stats

    brands = [
        ascii_text(brand, 80) for brand in profile.get("Brands") or []
        if str(brand).strip()
    ]
    primary_brand = ascii_text(profile.get("BrandForDTC"),
                               80) if profile.get("BrandForDTC") else None
    make = primary_brand or (brands[0] if len(brands) == 1 else None)
    year_from, year_to = vehicle_years(profile)
    meta = {
        "source": "Car Scanner import",
        "source_profile_key": str(profile.get("profile_key", "")),
        "model": ascii_text(profile.get("Name"), 120),
        "brands": brands,
    }
    if make:
        meta["make"] = make
    if year_from:
        meta["year_from"] = year_from
        meta["year_to"] = year_to

    result = {
        "schema":
        "skygauge.vehicle/1",
        "id":
        profile_id(profile),
        "rev":
        1,
        "name":
        ascii_text(f"{make + ' ' if make else ''}{profile.get('Name') or ''}",
                   23),
        "origin": {
            "from": "Car Scanner",
            "profile_key": str(profile.get("profile_key", ""))
        },
        "meta":
        meta,
        "bus": {
            "proto": 0,
            "gap_ms": 30
        },
        "defaults":
        "obd2",
        "signals":
        signals,
    }
    if len(
            json.dumps(result, ensure_ascii=True,
                       separators=(",", ":")).encode("utf-8")) > 16 * 1024:
        while result["signals"] and len(
                json.dumps(result, ensure_ascii=True,
                           separators=(",", ":")).encode("utf-8")) > 16 * 1024:
            result["signals"].pop()
            stats["skipped"]["profile_size_limit"] = stats["skipped"].get(
                "profile_size_limit", 0) + 1
        stats["emitted"] = len(result["signals"])
    if not result["signals"]:
        return None, stats
    return result, stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profiles",
                        required=True,
                        help="Car Scanner vehicle-profiles.json")
    parser.add_argument(
        "--output",
        required=True,
        help="Output directory for importable vehicle JSON files")
    parser.add_argument("--max-signals",
                        type=int,
                        default=12,
                        choices=range(1, 33))
    parser.add_argument("--catalog",
                        default=os.path.join(ROOT, "contracts",
                                             "vehicle-profile.schema.json"))
    args = parser.parse_args()

    with open(args.profiles, encoding="utf-8") as stream:
        profiles = json.load(stream)
    if not isinstance(profiles, list):
        parser.error("--profiles must contain a JSON array")

    try:
        import jsonschema
    except ImportError:
        parser.error(
            "Install jsonschema to validate outputs: python3 -m pip install jsonschema"
        )
    with open(args.catalog, encoding="utf-8") as stream:
        schema = json.load(stream)

    os.makedirs(args.output, exist_ok=True)
    summary = {
        "input_profiles": len(profiles),
        "written_profiles": 0,
        "profiles": [],
        "skipped": {},
        "pid_skip_reasons": {}
    }
    used_names = set()
    for source in profiles:
        if not isinstance(source, dict):
            continue
        result, stats = convert_profile(source, args.max_signals)
        for reason, count in stats["skipped"].items():
            summary["pid_skip_reasons"][
                reason] = summary["pid_skip_reasons"].get(reason, 0) + count
        if result is None:
            summary["skipped"][
                "no_supported_signals"] = summary["skipped"].get(
                    "no_supported_signals", 0) + 1
            continue
        base = result["id"]
        if base in used_names:
            result["id"] = (base[:26] + "." +
                            slug(source.get("profile_key"))[:4])[:31]
        used_names.add(result["id"])
        try:
            jsonschema.validate(result, schema)
        except jsonschema.ValidationError as error:
            summary["skipped"]["schema_validation"] = summary["skipped"].get(
                "schema_validation", 0) + 1
            stats["validation_error"] = error.message
            summary["profiles"].append({"id": result["id"], **stats})
            continue
        if len(
                json.dumps(result, ensure_ascii=True,
                           indent=2).encode("utf-8")) > 16 * 1024:
            summary["skipped"]["profile_size_limit"] = summary["skipped"].get(
                "profile_size_limit", 0) + 1
            continue
        filename = result["id"] + ".vehicle.json"
        with open(os.path.join(args.output, filename), "w",
                  encoding="utf-8") as stream:
            json.dump(result, stream, ensure_ascii=True, indent=2)
            stream.write("\n")
        summary["written_profiles"] += 1
        summary["profiles"].append({
            "id": result["id"],
            "file": filename,
            **stats
        })

    with open(os.path.join(args.output, "summary.json"), "w",
              encoding="utf-8") as stream:
        json.dump(summary, stream, ensure_ascii=True, indent=2)
        stream.write("\n")
    print(
        json.dumps(
            {
                "input_profiles": summary["input_profiles"],
                "written_profiles": summary["written_profiles"],
                "output": os.path.abspath(args.output),
                "skipped": summary["skipped"],
                "pid_skip_reasons": summary["pid_skip_reasons"],
            },
            ensure_ascii=False,
            indent=2))
    return 0 if summary["written_profiles"] else 1


if __name__ == "__main__":
    sys.exit(main())
