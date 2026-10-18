#!/usr/bin/env python3
"""Generate test/host/theme/fixtures/overlay_vectors.json.

Expected values follow ECMA-262 directly (no node in this environment):
  - Number.prototype.toFixed(f): exact decimal of the double, n / 10^f
    closest to |x|, ties pick the larger n; "-" when x < 0; |x| >= 1e21
    falls back to Number::toString.
  - Number::toString(10): shortest round-trip digits (Python repr gives the
    same digit string), then the JS exponent layout rules.
  - transformDataValue / getElementRange / clamp01 / pointer angle with the
    same double operations in the same order as FabricCanvas.tsx@5a7484e.
Cross-check against node before promoting to contracts/fixtures.

  python3 test/host/theme/tools/gen_overlay_vectors.py
"""
import json
import math
import os
import random
from decimal import Decimal, ROUND_HALF_UP, getcontext

getcontext().prec = 2000


def js_num_to_str(x):
    if math.isnan(x):
        return "NaN"
    if x == 0:
        return "0"
    if math.isinf(x):
        return "-Infinity" if x < 0 else "Infinity"
    sign = "-" if x < 0 else ""
    x = abs(x)
    d = Decimal(repr(x)).normalize()
    t = d.as_tuple()
    digits = "".join(str(c) for c in t.digits)
    k = len(digits)
    n = t.exponent + k
    if k <= n <= 21:
        s = digits + "0" * (n - k)
    elif 0 < n <= 21:
        s = digits[:n] + "." + digits[n:]
    elif -6 < n <= 0:
        s = "0." + "0" * (-n) + digits
    else:
        e = n - 1
        es = ("+" if e >= 0 else "-") + str(abs(e))
        s = digits[0] + ("." + digits[1:] if k > 1 else "") + "e" + es
    return sign + s


def js_to_fixed(x, f):
    if math.isnan(x):
        return "NaN"
    if abs(x) >= 1e21:
        return js_num_to_str(x)
    neg = x < 0
    d = Decimal(abs(x))  # exact binary value
    q = d.quantize(Decimal(1).scaleb(-f), rounding=ROUND_HALF_UP)
    s = format(q, "f")
    if f == 0 and "." in s:
        s = s.split(".")[0]
    return ("-" if neg else "") + s


def js_min(a, b):
    if math.isnan(a) or math.isnan(b):
        return math.nan
    return a if a < b else b


def js_max(a, b):
    if math.isnan(a) or math.isnan(b):
        return math.nan
    return a if a > b else b


def transform(dd, raw):
    if dd is None:
        return raw * 1 + 0
    scale = dd.get("scale")
    offset = dd.get("offset")
    v = raw * (1 if scale is None else scale) + (0 if offset is None else offset)
    keys = ("inputMin", "inputMax", "displayMin", "displayMax")
    if all(k in dd for k in keys) and dd["inputMax"] != dd["inputMin"]:
        p = (v - dd["inputMin"]) / (dd["inputMax"] - dd["inputMin"])
        v = dd["displayMin"] + p * (dd["displayMax"] - dd["displayMin"])
    return v


def rng(dd, lo, hi):
    if dd is not None:
        if dd.get("displayMin") is not None:
            lo = dd["displayMin"]
        if dd.get("displayMax") is not None:
            hi = dd["displayMax"]
    return lo, hi


def div(a, b):
    try:
        return a / b
    except ZeroDivisionError:
        if a == 0 or math.isnan(a):
            return math.nan
        return math.copysign(math.inf, a) * math.copysign(1, b)


def progress(v, lo, hi):
    return js_max(0.0, js_min(1.0, div(v - lo, hi - lo)))


def pointer_angle(v, lo, hi, amin, amax):
    c = js_max(lo, js_min(hi, v))
    vr = hi - lo
    ar = amax - amin
    return amin + ((c - lo) * ar) / vr if vr > 0 else amin


def num(x):
    """JSON-safe number: NaN/inf as strings."""
    if isinstance(x, float) and (math.isnan(x) or math.isinf(x)):
        return "NaN" if math.isnan(x) else ("Infinity" if x > 0 else "-Infinity")
    return x


def main():
    r = random.Random(20261012)
    to_fixed = []
    fixed_cases = [
        (0.125, 2), (1.005, 2), (-1.005, 2), (0.5, 0), (1.5, 0), (2.5, 0), (-2.5, 0),
        (-0.0, 2), (-0.001, 2), (0.0, 0), (123.456, 1), (1.45, 1), (8.345, 2), (1e-6, 7),
        (1e-10, 20), (0.1, 100), (999999999999999900000.0, 2), (1e21, 2), (-1e21, 3),
        (1.7976931348623157e308, 2), (5e-324, 100), (45.0, 0), (45.6789, 3), (-45.6789, 0),
        (0.05, 1), (0.15, 1), (0.25, 1), (0.35, 1), (2.675, 2), (1.0000000000000002, 16),
        (4.35, 1), (-4.35, 1), (100.0, 2), (13.5, 0), (-0.5, 0), (-0.4, 0),
    ]
    for x, f in fixed_cases:
        to_fixed.append([x, f, js_to_fixed(x, f)])
    for _ in range(300):
        e = r.randint(-12, 22)
        x = r.uniform(-1, 1) * (10.0 ** e)
        f = r.randint(0, 20)
        to_fixed.append([x, f, js_to_fixed(x, f)])
    for _ in range(100):
        # decimal ties of a few digits, the interesting toFixed cases
        x = r.randint(-100000, 100000) / 1000.0
        f = r.randint(0, 3)
        to_fixed.append([x, f, js_to_fixed(x, f)])

    to_str = []
    str_cases = [0.1, 0.2 + 0.1, 1e21, 1e20, 123e-20, 1.5e300, 5e-324, 100.0, 1e-6, 1e-7,
                 -1e-7, 2.0 ** 53, 1 / 3, -1 / 3, 1234567890123456789012.0, 0.000001234,
                 1.7976931348623157e308, -0.0, 25.5, 4.35, 1e100, 9007199254740993.0, 12.0,
                 -45.0, 1.5e-6, 1e-5]
    for x in str_cases:
        to_str.append([x, js_num_to_str(x)])
    for _ in range(300):
        e = r.randint(-30, 30)
        x = r.uniform(-1, 1) * (10.0 ** e)
        to_str.append([x, js_num_to_str(x)])

    dds = [
        None,
        {},
        {"scale": 0.1},
        {"scale": 0.01, "offset": -40},
        {"offset": 273.15},
        {"inputMin": 0, "inputMax": 100, "displayMin": 0, "displayMax": 8000},
        {"scale": 2, "inputMin": 10, "inputMax": 10, "displayMin": 0, "displayMax": 1},
        {"inputMin": -20, "inputMax": 120, "displayMin": 1, "displayMax": 0},
        {"displayMin": 0.5, "displayMax": 2.5},
        {"scale": 1.8, "offset": 32, "displayMin": -40, "displayMax": 300},
    ]
    raws = [0.0, 1.0, -1.0, 0.1, 99.9, 3456.0, 10.0, -40.0, 1e6, 7.25, -0.5]
    transforms = []
    for dd in dds:
        for raw in raws:
            v = transform(dd, raw)
            lo, hi = rng(dd, 0.0, 8000.0)
            transforms.append({
                "dd": dd, "raw": raw, "value": num(v),
                "range": [num(lo), num(hi)],
                "progress": num(progress(v, lo, hi)),
                "angle": num(pointer_angle(v, lo, hi, -135.0, 135.0)),
            })
    # degenerate ranges
    for v, lo, hi in [(5.0, 5.0, 5.0), (6.0, 5.0, 5.0), (4.0, 5.0, 5.0), (3.0, 10.0, 0.0)]:
        transforms.append({"dd": None, "raw": v, "value": v, "range": [lo, hi],
                           "progress": num(progress(v, lo, hi)),
                           "angle": num(pointer_angle(v, lo, hi, -135.0, 135.0))})

    labels = []
    lab_dds = [
        ({"precision": 1, "unit": "bar"}, [0.0, 1.25, -1.25, 0.05, 2.35]),
        ({"precision": 2, "scale": 0.01}, [12345.0, -5.0, 0.5]),
        ({"precision": 0}, [2.5, -2.5, 0.49]),
        ({"unit": "RPM"}, [0.0, 3456.0, 7999.0]),
        ({"valueMap": {"0": "N", "1": "1st", "-1": "R"}, "unit": "gear"}, [0.0, 1.0, 1.9, -1.0, 2.0]),
        ({"valueMap": {"1": True, "2": 3.5, "3": None}}, [1.0, 2.0, 3.0, 4.0]),
        ({"scale": 0.1, "precision": 1, "unit": "V"}, [143.0, 120.0]),
    ]
    for dd, vals in lab_dds:
        for raw in vals:
            v = transform(dd, raw)
            vm = dd.get("valueMap")
            key = js_num_to_str(float(math.trunc(v))) if not math.isnan(v) else "NaN"
            if vm is not None and key in vm:
                m = vm[key]
                text = ("true" if m is True else "false" if m is False else "null" if m is None
                        else m if isinstance(m, str) else js_num_to_str(float(m)))
            elif "precision" in dd:
                text = js_to_fixed(v, int(dd["precision"]))
            else:
                text = str(math.trunc(v))  # format "%d", divisor 0 (no -0 cases above)
            if dd.get("unit"):
                text += " " + dd["unit"]
            labels.append({"dd": dd, "format": "%d", "divisor": 0, "raw": raw, "text": text})
    labels.append({"dd": {"precision": 1, "unit": "bar"}, "format": "%d", "divisor": 0,
                   "raw": "NaN", "text": "--"})

    out = {
        "generator": "test/host/theme/tools/gen_overlay_vectors.py (ECMA-262 via Python Decimal)",
        "to_fixed": to_fixed,
        "num_to_str": to_str,
        "transform": transforms,
        "label": labels,
    }
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures",
                        "overlay_vectors.json")
    with open(path, "w") as fp:
        json.dump(out, fp, indent=1)
        fp.write("\n")
    print(f"wrote {os.path.normpath(path)}: to_fixed={len(to_fixed)} num_to_str={len(to_str)} "
          f"transform={len(transforms)} label={len(labels)}")


if __name__ == "__main__":
    main()
