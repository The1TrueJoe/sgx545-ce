"""Assemble SGX545 USSE instructions from fields.json, the inverse of isa.decode.

  python3 asm.py 'fmad16 pa9, -pa9, c45, c45 [f1.0 h12]' ...   prints "<hi> <lo>"
  python3 asm.py --check                                       round-trips known encodings

Syntax: the mnemonic as dis.py prints it (without '?'), then operands in
USEASM argument order: optional '-' (negate, argument flag 0) or '|x|' (abs,
flag 1), a register prefix (r o pa sa g c i #) and number, then '.f<n>' for
further argument flag bits (pck: .f7-.f10 destination bytes, .f5/.f6 source
half; fmad16: .f25/.f26). In brackets, op flags as dis.py prints them
(f1.0, f1.17 = end of program, f2.8, ...) and raw bits (h12, l18) for fields
that only show as correlations (repeat counts).

Every field comes from what libusc's encoder was seen to drive (ISA.md), so an
instruction assembles only from fields that were fuzzed; anything else is an
error rather than a guess.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = json.load(open(os.path.join(HERE, "fields.json")))
NAMES = json.load(open(os.path.join(HERE, "names.json")))
PREFIX = {"r": 0, "o": 1, "pa": 2, "sa": 3, "g": 5, "c": 6, "i": 7, "#": 8}


def form(mnemonic):
    keys = [k for k, v in NAMES.items() if isinstance(v, list) and v and v[0] == mnemonic]
    # a form's own name (e.g. 0xa3/1b fdual.mov) wins over its opcode's (0xa3 mov)
    forms = [f for f in TABLE if f != "sweep" and
             (f in keys or (f.split("/")[0] in keys and f not in NAMES))]
    if len(forms) != 1:
        raise ValueError(f"{mnemonic}: {len(forms)} forms in fields.json ({forms})")
    return TABLE[forms[0]]


def setbit(w, name, v=1):
    i = 0 if name[0] == "l" else 1
    b = int(name[1:])
    w[i] = (w[i] & ~(1 << b)) | (v << b)


def operand(text):
    neg = text.startswith("-")
    text = text.lstrip("-")
    absv = text.startswith("|")
    text = text.strip("|")
    parts = text.split(".")
    m = re.fullmatch(r"(pa|sa|r|o|g|c|i|#)(\d+)", parts[0])
    if not m:
        raise ValueError(f"bad operand {text!r}")
    flags = [0] * neg + [1] * absv + [int(p[1:]) for p in parts[1:]]
    return PREFIX[m.group(1)], int(m.group(2)), flags


def put_arg(w, a, typ, num, flags, name):
    nums = {int(b): outs for b, outs in a.get("num", {}).items()}
    numbits = {}
    for b in range(7):
        v = num >> b & 1
        if b in nums:
            if len(nums[b]) != 1:
                raise ValueError(f"{name}: number bit {b} is packed ({nums[b]})")
            numbits[nums[b][0]] = v
        elif v:
            raise ValueError(f"{name}: number {num} needs bit {b}, which has no field")
    bank = a.get("bank")
    if bank and bank["bits"]:
        pats = bank["types"].get(str(typ))
        if not pats:
            raise ValueError(f"{name}: register type {typ} has no bank encoding here")
        # a bank bit that is also a number bit must agree with the number
        ok = [p for p in pats if all(numbits.get(n, int(c)) == int(c) for n, c in zip(bank["bits"], p))]
        if not ok:
            raise ValueError(f"{name}: no bank pattern for type {typ} fits number {num}")
        for n, c in zip(bank["bits"], ok[0]):
            setbit(w, n, int(c))
    for n, v in numbits.items():
        setbit(w, n, v)
    for f in flags:
        outs = a.get("flags", {}).get(str(f))
        if not outs:
            raise ValueError(f"{name}: argument flag {f} has no field")
        for o in outs:
            setbit(w, o, 1)


def assemble(line):
    m = re.fullmatch(r"\s*(\S+)\s*(.*?)\s*(?:\[(.*)\])?\s*", line)
    mn, ops, fl = m.group(1).rstrip("?"), m.group(2), m.group(3) or ""
    e = form(mn)
    w = list(e["value"])
    args = [o.strip() for o in ops.split(",") if o.strip()]
    for i, text in enumerate(args):
        a = e["args"].get(f"a{i}")
        if a is None:
            raise ValueError(f"{mn}: no argument a{i}")
        typ, num, flags = operand(text)
        put_arg(w, a, typ, num, flags, f"{mn} a{i}")
    for f in fl.split():
        if re.fullmatch(r"[lh]\d+", f):
            setbit(w, f, 1)
            continue
        word, bit = f.split(".")
        outs = e["flags"].get(word, {}).get(bit)
        if not outs:
            raise ValueError(f"{mn}: op flag {f} has no field")
        for o in outs:
            setbit(w, o, 1)
    lo, hi = w
    return hi & 0xFFFFFFFF, lo & 0xFFFFFFFF


# Encodings the compiler and the GLES driver produced, with how dis.py reads them.
KNOWN = [
    ("30829126 60009680", "fmad16 pa0, -pa2, c45, pa0 [f1.0 f2.29 f2.31 h12]"),        # corpus sub_m
    ("3081d026 90008030", "fmad16 pa0, -pa2, pa0, c48 [f1.0 f2.29 f2.30 h12]"),        # corpus neg_m
    ("30812226 90004030", "fmad16 pa0, -pa1.f25, pa0.f25, c48 [f1.0]"),                # corpus s_neg_m
    ("40801a1a a1260489", "pck.u8.f16 pa9.f8.f9, pa9.f6, pa9 [f1.0 f2.8 h12]"),        # probe secondary
    ("40841a26 a124850a", "pck.u8.f16 pa9.f7.f10, pa10, pa10.f6 [f1.0 f1.17 f2.8 h12]"),  # probe, last
    ("40831a3e 512416ad", "pck.u8.f16 pa9.f7.f8.f9.f10, c45, c45 [f1.0 f2.8 h12]"),    # corpus c1
    ("40801c1a a1240509", "pck.u8.f32 pa9.f8.f9, pa10, pa9 [f1.0 f2.8 h12]"),          # corpus u_only
    ("28851001 e0000480", "mov o0, sa9 [f1.0 f1.17 h12]"),                              # probe main
    ("28811002 e0000480", "mov pa0, sa9 [f1.0 h12]"),                                  # corpus c1 main
]


def check():
    bad = 0
    for want, line in KNOWN:
        hi, lo = assemble(line)
        got = f"{hi:08x} {lo:08x}"
        ok = got == want
        bad += not ok
        print(f"{'ok ' if ok else 'BAD'} {got} {'' if ok else '(want ' + want + ') '}{line}")
    return bad


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        sys.exit(1 if check() else 0)
    for line in sys.argv[1:]:
        hi, lo = assemble(line)
        print(f"{hi:08x} {lo:08x}  {line}")
