#!/usr/bin/env python3
"""
expr2rpn.py - compile a profile formula ("expr", infix) to the "rpn" array
the device executes (contracts/vehicle-rpn.md section 2).

Usage:
    python3 tools/vehicles/expr2rpn.py "(A*256+B)/4"
        -> [["A", 0], ["k", 256], "*", ["A", 1], "+", ["k", 4], "/"]
    python3 tools/vehicles/expr2rpn.py --check vehicles/catalog/*.vehicle.json
        every source that has both "expr" and "rpn": compile expr and check
        that both evaluate to the same value over a set of test vectors

Expression syntax (C precedence, lowest first):  |   &   << >>   + -   * /
unary -   primaries:
    A..H          data byte 0..7            -> ["A", i]
    d[i]          data byte i               -> ["A", i]
    X             the source's raw field    -> ["X"]
    R(start, bits[, le|be][, u|s])          -> ["R", start, bits, ...]
    123  1.5  0x1F                          -> ["k", c]
    min(a,b) max(a,b) clamp(x,lo,hi) sext8(x) sext16(x) lut(x, n)
    ( expr )
"""

import argparse
import json
import math
import re
import struct
import sys

TOKEN = re.compile(r"\s*(?:(0[xX][0-9A-Fa-f]+|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)|"
                   r"([A-Za-z_][A-Za-z0-9_]*)|(<<|>>|[-+*/&|(),\[\]]))")
FUNCS = {"min": 2, "max": 2, "clamp": 3, "sext8": 1, "sext16": 1}
BINOPS = [["|"], ["&"], ["<<", ">>"], ["+", "-"], ["*", "/"]]


class ExprError(Exception):
    pass


def _tokens(text):
    pos, out = 0, []
    text = text.rstrip()
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise ExprError("bad character at %d: %r" % (pos, text[pos:pos + 8]))
        num, name, op = m.groups()
        if num is not None:
            v = int(num, 16) if num[:2] in ("0x", "0X") else float(num)
            out.append(("num", int(v) if float(v).is_integer() else v))
        elif name is not None:
            out.append(("name", name))
        else:
            out.append(("op", op))
        pos = m.end()
    out.append(("end", None))
    return out


class _Parser:
    def __init__(self, text):
        self.t = _tokens(text)
        self.i = 0

    def peek(self):
        return self.t[self.i]

    def take(self, kind=None, val=None):
        k, v = self.t[self.i]
        if (kind and k != kind) or (val is not None and v != val):
            raise ExprError("expected %s, got %r" % (val or kind, v))
        self.i += 1
        return v

    def parse(self):
        out = self.binary(0)
        if self.peek()[0] != "end":
            raise ExprError("unexpected %r" % (self.peek()[1],))
        return out

    def binary(self, level):
        if level == len(BINOPS):
            return self.unary()
        out = self.binary(level + 1)
        while self.peek()[0] == "op" and self.peek()[1] in BINOPS[level]:
            op = self.take()
            out += self.binary(level + 1) + [op]
        return out

    def unary(self):
        if self.peek() == ("op", "-"):
            self.take()
            if self.peek()[0] == "num":
                return [["k", -self.take()]]
            return self.unary() + ["neg"]
        if self.peek() == ("op", "+"):
            self.take()
            return self.unary()
        return self.primary()

    def int_arg(self):
        v = self.take("num")
        if not isinstance(v, int):
            raise ExprError("integer expected, got %r" % v)
        return v

    def primary(self):
        k, v = self.peek()
        if k == "num":
            self.take()
            return [["k", v]]
        if k == "op" and v == "(":
            self.take()
            out = self.binary(0)
            self.take("op", ")")
            return out
        if k != "name":
            raise ExprError("unexpected end of expression" if k == "end" else "unexpected %r" % (v,))
        self.take()
        if len(v) == 1 and v in "ABCDEFGH":
            return [["A", "ABCDEFGH".index(v)]]
        if v == "X":
            return [["X"]]
        if v == "d":
            self.take("op", "[")
            i = self.int_arg()
            self.take("op", "]")
            return [["A", i]]
        if v == "R":
            self.take("op", "(")
            ins = ["R", self.int_arg()]
            self.take("op", ",")
            ins.append(self.int_arg())
            while self.peek() == ("op", ","):
                self.take()
                opt = self.take("name")
                if opt not in ("le", "be", "u", "s"):
                    raise ExprError("R option must be le/be/u/s, got %r" % opt)
                ins.append(opt)
            self.take("op", ")")
            return [ins]
        if v == "lut":
            self.take("op", "(")
            out = self.binary(0)
            self.take("op", ",")
            n = self.int_arg()
            self.take("op", ")")
            return out + [["lut", n]]
        if v in FUNCS:
            self.take("op", "(")
            out = []
            for i in range(FUNCS[v]):
                if i:
                    self.take("op", ",")
                out += self.binary(0)
            self.take("op", ")")
            return out + [v]
        raise ExprError("unknown name %r" % v)


def compile_expr(text):
    """Infix expression -> rpn list (raises ExprError)."""
    return _Parser(text).parse()


# ------------------------------------------------------------------ #
#  reference evaluator (for --check; float32 like the device)          #
# ------------------------------------------------------------------ #

def _f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _bits(data, start, bits, be, signed):
    v = 0
    for j in range(bits):
        k = start + j
        bit = (data[k // 8] >> ((7 - k % 8) if be else (k % 8))) & 1
        v = (v << 1 | bit) if be else (v | bit << j)
    if signed and v >> (bits - 1):
        v -= 1 << bits
    return v


def _u32(x):
    x = math.trunc(x)
    if not -2 ** 31 <= x <= 2 ** 32 - 1:
        raise ArithmeticError("integer operand out of range")
    return x & 0xFFFFFFFF


def evaluate(rpn, data, xval=None, luts=()):
    st = []
    for ins in rpn:
        if isinstance(ins, list):
            op = ins[0]
            if op == "A":
                st.append(float(data[ins[1]]))
            elif op == "R":
                st.append(float(_bits(data, ins[1], ins[2], "be" in ins[3:], "s" in ins[3:])))
            elif op == "X":
                st.append(float(xval))
            elif op == "k":
                st.append(_f32(ins[1]))
            elif op == "lut":
                x = st.pop()
                for e in luts[ins[1]]:
                    lo, hi, out = (e[0], e[0], e[1]) if len(e) == 2 else e
                    if _f32(lo) <= x <= _f32(hi):
                        st.append(_f32(out))
                        break
                else:
                    raise ArithmeticError("lut miss")
            continue
        if ins in ("neg", "sext8", "sext16"):
            a = st.pop()
            if ins == "neg":
                st.append(-a)
            else:
                n = 8 if ins == "sext8" else 16
                u = _u32(a) & ((1 << n) - 1)
                st.append(float(u - (1 << n) if u >> (n - 1) else u))
            continue
        if ins == "clamp":
            hi, lo, x = st.pop(), st.pop(), st.pop()
            if lo > hi:
                raise ArithmeticError("clamp lo > hi")
            st.append(min(max(x, lo), hi))
            continue
        b, a = st.pop(), st.pop()
        if ins == "+":
            r = a + b
        elif ins == "-":
            r = a - b
        elif ins == "*":
            r = a * b
        elif ins == "/":
            if b == 0:
                raise ArithmeticError("division by zero")
            r = a / b
        elif ins == "min":
            r = min(a, b)
        elif ins == "max":
            r = max(a, b)
        elif ins in ("&", "|"):
            r = float(_u32(a) & _u32(b) if ins == "&" else _u32(a) | _u32(b))
        elif ins in ("<<", ">>"):
            n = math.trunc(b)
            if not 0 <= n <= 31:
                raise ArithmeticError("shift out of range")
            r = float((_u32(a) << n) & 0xFFFFFFFF if ins == "<<" else _u32(a) >> n)
        else:
            raise ExprError("unknown op %r" % (ins,))
        st.append(_f32(r))
    if len(st) != 1:
        raise ExprError("stack has %d values at the end" % len(st))
    return st[0]


VECTORS = [bytes([0] * 8), bytes([0x40] * 8), bytes([0xFF] * 8),
           bytes([0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0]),
           bytes([0x80, 0x01, 0x7F, 0xFE, 0x03, 0x30, 0xC0, 0x0C])]


def _xval(src, data):
    if "can" in src:
        return _bits(data, src["bit"], src["bits"], src.get("endian") == "be", bool(src.get("signed")))
    return data[0]   # byte_from_end: d[] is the response tail, X = its first byte


def check_profile(path):
    with open(path, encoding="utf-8") as f:
        prof = json.load(f)
    luts, errors, n = prof.get("luts", []), [], 0
    for sig in prof.get("signals", []):
        for i, src in enumerate(sig.get("src", [])):
            if "expr" not in src or "rpn" not in src:
                continue
            where = "%s src %d" % (sig.get("id"), i)
            try:
                mine = compile_expr(src["expr"])
            except ExprError as e:
                errors.append("%s: expr %r: %s" % (where, src["expr"], e))
                continue
            n += 1
            for data in VECTORS:
                x = _xval(src, data)
                try:
                    a = evaluate(src["rpn"], data, x, luts)
                except ArithmeticError:
                    a = None
                try:
                    b = evaluate(mine, data, x, luts)
                except ArithmeticError:
                    b = None
                if (a is None) != (b is None) or (a is not None and abs(a - b) > 1e-4 * max(1.0, abs(a))):
                    errors.append("%s: expr %r = %s but rpn = %s for data %s"
                                  % (where, src["expr"], b, a, data.hex()))
                    break
    return n, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="arguments are profile files: check expr against rpn")
    ap.add_argument("args", nargs="+", metavar="EXPR|FILE")
    a = ap.parse_args()

    if not a.check:
        try:
            print(json.dumps(compile_expr(" ".join(a.args))))
        except ExprError as e:
            print("expr2rpn: error: %s" % e, file=sys.stderr)
            return 1
        return 0

    total, bad = 0, 0
    for path in a.args:
        n, errors = check_profile(path)
        total += n
        for e in errors:
            print("expr2rpn: %s: %s" % (path, e), file=sys.stderr)
        bad += bool(errors)
    print("expr2rpn: %d file(s), %d formula(s), %d file(s) with mismatches" % (len(a.args), total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
