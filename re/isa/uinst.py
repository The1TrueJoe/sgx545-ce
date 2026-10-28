"""Build USEASM instructions (libusc's USE_INST) for oracle.py.

  from uinst import U, A, enc
  enc(U(0x0, A("r", 1), A("pa", 2), A("sa", 3), A("c", 52)))    -> (lo, hi) or raises

USE_INST, as libusc's assembler reads it (useasm.c, libusc + 0x109f50):
  dw0 opcode, dw1 flags1, dw2 flags2, dw3 flags3, dw4 test,
  then 5 dwords per argument from dw5: number, type, flags, index, (unused).
flags1: b0 skip-invalid (h23), b2 h20, b3 h11 ("nosched"), b4-8 repeat count,
b9-12 write mask, b13-16 predicate, b17 end (h18), b25 dual-issue pair,
b30 test (writes a predicate).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle  # noqa: E402

# USEASM register types (libusc numbering); names are ours
RT = {"r": 0, "o": 1, "pa": 2, "sa": 3, "x": 4, "g": 5, "c": 6, "i": 7, "#": 8, "p": 9}

_o = None


def oracle():
    global _o
    if _o is None:
        _o = Oracle(os.environ.get("LIBUSC"))
    return _o


def A(t, n=0, flags=0, index=0, fifth=0):
    """one argument: register type (name or number), number, flags, index"""
    return [n, RT.get(t, t) if isinstance(t, str) else t, flags, index, fifth]


def U(op, *args, f1=0, f2=0, f3=0, test=0):
    w = [op, f1, f2, f3, test]
    for a in args:
        w += a
    return w


def enc(inst, strict=True, offset=0, pair=None):
    """-> (lo, hi); with strict, an encoder error report raises ValueError.
    pair: second instruction of a dual-issue pair (set f1 bit 25 on inst)"""
    ret, lo, hi, errs, calls = oracle().encode(inst, offset=offset, pair=pair)
    if strict and errs:
        raise ValueError(f"encoder reported {errs} error(s) (ret {ret})")
    return lo, hi


def try_enc(inst, offset=0, pair=None):
    """-> (lo, hi) or None if the encoder rejects it (error report or a fault)"""
    try:
        ret, lo, hi, errs, calls = oracle().encode(inst, offset=offset, pair=pair)
    except Exception:
        return None
    return None if errs else (lo, hi)


def hexpair(lo, hi):
    return f"{hi:08x} {lo:08x}"
