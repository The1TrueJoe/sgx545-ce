"""Oracle sweep for hw_mem.py (smp, ld/st/eld, the hw 1f special class).

Every case is a USEASM instruction run through libusc's own encoder
(oracle.py); the result must decode to the expected form with the expected
field values and operands, and its text must parse and re-encode to the same
64 bits. A few field values no USEASM input reaches (dim 3, size q, ...) are
checked on hand-built words (decode -> text -> parse -> encode only).

  cd $S && LIBUSC=$S/bin/libusc.so $S/venv/bin/python sgx/tools/isa/check_mem.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw  # noqa: E402
import oracle as om  # noqa: E402
from uinst import A, U, oracle  # noqa: E402

FAIL = []
N = [0]
SEEN = {}


def I(n):
    return A(8, n & 0xFFFFFFFF)


def E(n):                       # int-source-select argument
    return A(0x10, n)


def DRC(n=0):
    return A(10, n)


def r(n):
    return {"kind": "r", "n": n}


def k(kind, n):
    return {"kind": kind, "n": n}


def imm(n):
    return {"kind": "#", "n": n}


def roundtrip(desc, lo, hi):
    x = hw.decode(lo, hi)
    if x is None:
        FAIL.append(f"{desc}: {hi:08x} {lo:08x} has no form")
        return None
    if x.unknown:
        FAIL.append(f"{desc}: {hi:08x} {lo:08x} unexplained bits {x.unknown}")
    t = hw.fmt(x)
    try:
        lo2, hi2 = hw.encode(hw.parse(t))
    except Exception as e:  # noqa: BLE001
        FAIL.append(f"{desc}: {t!r} does not parse/encode: {e}")
        return x
    if (lo2, hi2) != (lo, hi):
        FAIL.append(f"{desc}: {t!r} re-encodes to {hi2:08x} {lo2:08x}, not {hi:08x} {lo:08x}")
    return x


def case(desc, inst, form, fields=None, ops=None, offset=0, base=None, allow_err=False, src=None):
    """encode inst with libusc; check form name (and src tag), fields, operands, round trip"""
    N[0] += 1
    o = oracle()
    try:
        if base is None:
            ret, lo, hi, errs, calls = o.encode(inst, offset=offset)
        else:
            ret, lo, hi, errs, calls = o.encode(inst, offset=offset, base=base)
    except Exception as e:  # noqa: BLE001
        FAIL.append(f"{desc}: encoder faulted: {e}")
        return
    if errs and not allow_err:
        FAIL.append(f"{desc}: encoder rejected it ({errs} errors)")
        return
    x = roundtrip(desc, lo, hi)
    if x is None:
        return
    if x.form.name != form or (src and src not in x.form.src):
        FAIL.append(f"{desc}: {hi:08x} {lo:08x} decodes as {x.form.name} ({x.form.src}), want {form} {src or ''}")
        return
    for key, want in (fields or {}).items():
        if x.d.get(key) != want:
            FAIL.append(f"{desc}: field {key} = {x.d.get(key)}, want {want} ({hw.fmt(x)})")
    if ops is not None:
        if len(ops) != len(x.ops):
            FAIL.append(f"{desc}: {len(x.ops)} operands, want {len(ops)} ({hw.fmt(x)})")
        for i, (got, want) in enumerate(zip(x.ops, ops)):
            if want is None:
                continue
            for kk, vv in want.items():
                if got.get(kk) != vv:
                    FAIL.append(f"{desc}: operand {i} {got}, want {want} ({hw.fmt(x)})")
                    break
    SEEN.setdefault(x.form.name + (" " + x.form.src if x.form.name in ("smp", "phas") else ""), 0)
    SEEN[x.form.name + (" " + x.form.src if x.form.name in ("smp", "phas") else "")] += 1


def synth(desc, lo, hi, form, fields):
    """a hand-built word: decode, check fields, round-trip"""
    N[0] += 1
    x = roundtrip(desc, lo, hi)
    if x is None:
        return
    if x.form.name != form:
        FAIL.append(f"{desc}: decodes as {x.form.name}, want {form}")
    for key, want in fields.items():
        if x.d.get(key) != want:
            FAIL.append(f"{desc}: field {key} = {x.d.get(key)}, want {want}")


# --------------------------------------------------------------------------
# hw 1c smp
# --------------------------------------------------------------------------

def smp_checks():
    D, C, S = A("r", 0), A("pa", 2), A("#", 0)
    for lod in range(4):
        for dim in range(3):
            op = 0x6F + 3 * lod + dim
            args = [A("r", 5), C, A("sa", 7)] + ([A("sa", 9)] if lod else []) + [DRC(1), E(0x15)]
            case(f"smp op {op:#x}", U(op, *args, f1=0x200), "smp",
                 {"dim": dim, "drc": 1, "mask": 1},
                 [r(5), k("pa", 2), k("sa", 7)] + ([k("sa", 9)] if lod else []))
    base = lambda *a, **kw: U(0x70, *a, DRC(0), E(0x15), **kw)  # noqa: E731
    case("smp dst pa", base(A("pa", 127), C, S, f1=0x200), "smp", ops=[k("pa", 127), None, None])
    for t in ("r", "o", "pa", "sa"):
        case(f"smp coord {t}", base(D, A(t, 100), S, f1=0x200), "smp", ops=[None, k(t, 100), None])
    case("smp coord i2 = r126", base(D, A("i", 2), S, f1=0x200), "smp", ops=[None, r(126), None])
    for t, want in (("r", r(3)), ("o", k("o", 3)), ("pa", k("pa", 3)), ("sa", k("sa", 3)), ("c", k("c", 3)),
                    ("i", k("i", 3)), ("#", imm(3)), ("g", k("g", 3))):
        case(f"smp state {t}", base(D, C, A(t, 3), f1=0x200), "smp", ops=[None, None, want])
    case("smp state ix", base(D, C, A("pa", 3, 0, 1), f1=0x200), "smp",
         ops=[None, None, {"kind": "ix", "bank": "pa", "idx": 1, "off": 3}])
    for t in ("r", "pa", "sa", "c", "#"):
        case(f"smp.bias lod {t}", U(0x73, D, C, S, A(t, 4), DRC(0), E(0x15), f1=0x200), "smp",
             ops=[None, None, None, {"kind": t if t != "#" else "#", "n": 4}])
    case("smp f16 coord", base(D, A("pa", 2, 1 << 22), S, f1=0x200), "smp", {"cfmt": 1})
    case("smp c10 coord", base(D, A("pa", 2, 1 << 24), S, f1=0x200), "smp", {"cfmt": 2})
    for f3, fld, val in ((1, "pcf", 1), (2, "pcf", 2), (8, "ret", 1), (4, "ret", 2)):
        case(f"smp f3 {f3:#x}", base(D, C, S, f1=0x200, f3=f3), "smp", {fld: val})
    for f1, fld, val in ((0x201, "skipinv", 1), (0x204, "syncs", 1), (0x208, "nosched", 1)):
        case(f"smp f1 {f1:#x}", base(D, C, S, f1=f1), "smp", {fld: val})
    for p in range(1, 8):
        hwp = {1: 1, 2: 2, 3: 5, 4: 6, 5: 3, 6: 4, 7: 7}[p]
        case(f"smp pred {p}", base(D, C, S, f1=0x200 | p << 13), "smp", {"pred": hwp})
    # rejected on SGX545 but the bits are the encoder's: repeat (h21 + count)
    case("smp rpt 3 (rejected)", base(D, C, S, f1=0x30), "smp", {"rcnt": 1, "rpt": 3}, allow_err=True)
    case("smp mask 0xf (rejected)", base(D, C, S, f1=0x1e00), "smp", {"mask": 0xF}, allow_err=True)
    # field values no USEASM input reaches
    for spec, fld, val in (("h10-11", "dim", 3), ("h14-15", "ret", 3), ("h5-6", "pcf", 3), ("h3-4", "cfmt", 3)):
        v = hw.put(0xE0031404 << 32 | 0xA0008000, spec, val)
        synth(f"smp {fld}={val}", v & 0xFFFFFFFF, v >> 32, "smp", {fld: val})


# --------------------------------------------------------------------------
# hw 1d / 1e
# --------------------------------------------------------------------------

AM = {0: "a", 1: "l", 2: "t"}
SIZE = {0: 2, 1: 1, 2: 0}      # USEASM op % 4 -> h4-5


def ld_checks():
    B31 = 0x80000000
    for i in range(24):
        op = 0x7B + i
        if i % 4 == 3:
            continue
        am, size, store = i // 8, SIZE[i % 4], (i // 4) % 2
        off = A("r", 3) if am == 2 else A("#", 3)
        if store:
            case(f"st op {op:#x}", U(op, A("pa", 1), off, A("r", 4), f1=B31), "st",
                 {"am": am, "size": size, "moe": 0},
                 [k("pa", 1), k("r", 3) if am == 2 else imm(3), r(4)])
        else:
            case(f"ld op {op:#x}", U(op, A("r", 1), A("pa", 2), off, A("#", 0), DRC(1), f1=B31), "ld",
                 {"am": am, "size": size, "drc": 1, "moe": 0, "range": 0},
                 [r(1), k("pa", 2), k("r", 3) if am == 2 else imm(3)])
    LD = lambda *a, **kw: U(0x85, *a, **kw)  # noqa: E731
    std = [A("r", 1), A("r", 2), A("#", 3), A("#", 0), DRC(0)]
    case("ld moe (flags1 b31 clear)", LD(*std), "ld", {"moe": 1})
    for f1, fld, val in ((1, "skipinv", 1), (4, "syncs", 1), (1 << 27, "preinc", 1), (1 << 28, "postinc", 1),
                         (0x20, "rpt", 2), (0xF0, "rpt", 15), (0x100, "rpt", 16), (1 << 13, "pred", 1),
                         (7 << 13, "pred", 7)):
        case(f"ld f1 {f1:#x}", LD(*std, f1=B31 | f1), "ld", {fld: val})
    case("ld range", LD(A("r", 1), A("r", 2), A("#", 3), A("sa", 5), DRC(0), f1=B31 | 1 << 29), "ld",
         {"range": 1}, [r(1), r(2), imm(3), k("sa", 5)])
    for f2, f3, flds in ((4, 0, {"f2_2": 1}), (8, 0, {"f2_3": 1}), (0x200, 0, {"f2_9": 1}),
                         (0, 0x200, {"f2_2": 1, "f2_3": 1})):
        case(f"ld f2 {f2:#x} f3 {f3:#x}", LD(*std, f1=B31, f2=f2, f3=f3), "ld", flds)
    case("ld nosched (rejected)", LD(*std, f1=B31 | 8), "eld", allow_err=True)
    case("ld dst pa", LD(A("pa", 9), A("r", 2), A("#", 3), A("#", 0), DRC(0), f1=B31), "ld",
         ops=[k("pa", 9), None, None])
    for t in ("r", "o", "pa", "sa"):
        case(f"ld base {t}", LD(A("r", 1), A(t, 2), A("#", 3), A("#", 0), DRC(0), f1=B31), "ld",
             ops=[None, k(t, 2), None])
    for t, want in (("r", r(3)), ("o", k("o", 3)), ("pa", k("pa", 3)), ("sa", k("sa", 3)), ("c", k("c", 3)),
                    ("i", k("i", 3)), ("#", imm(127)), ("g", k("g", 3))):
        a = A(t, 127 if t == "#" else 3)
        case(f"ld off {t}", LD(A("r", 1), A("r", 2), a, A("#", 0), DRC(0), f1=B31), "ld", ops=[None, None, want])
    case("ld off ix", LD(A("r", 1), A("r", 2), A("pa", 3, 0, 2), A("#", 0), DRC(0), f1=B31), "ld",
         ops=[None, None, {"kind": "ix", "bank": "pa", "idx": 2, "off": 3}])
    ST = lambda *a, **kw: U(0x89, *a, **kw)  # noqa: E731
    sst = [A("r", 1), A("#", 2), A("r", 3)]
    for f1, fld, val in ((1, "skipinv", 1), (4, "syncs", 1), (8, "nosched", 1), (1 << 27, "preinc", 1),
                         (1 << 28, "postinc", 1), (1 << 29, "range", 1), (0x30, "rpt", 3), (2 << 13, "pred", 2)):
        case(f"st f1 {f1:#x}", ST(*sst, f1=B31 | f1), "st", {fld: val})
    case("st moe", ST(*sst), "st", {"moe": 1})
    for f2, f3, flds in ((4, 0, {"f2_2": 1}), (8, 0, {"f2_3": 1}), (0x200, 0, {"f2_9": 1}),
                         (0, 0x200, {"f2_2": 1, "f2_3": 1}), (0, 0x2000, {"f3_13": 1})):
        case(f"st f2 {f2:#x} f3 {f3:#x}", ST(*sst, f1=B31, f2=f2, f3=f3), "st", flds)
    for t, want in (("r", r(4)), ("o", k("o", 4)), ("pa", k("pa", 4)), ("sa", k("sa", 4)), ("c", k("c", 4)),
                    ("i", k("i", 4)), ("#", imm(4)), ("g", k("g", 4))):
        case(f"st data {t}", ST(A("r", 1), A("#", 2), A(t, 4), f1=B31), "st", ops=[None, None, want])
    # eld
    for op, size in ((0x93, 0), (0x94, 1)):
        case(f"eld {op:#x}", U(op, A("r", 1), A("r", 2), A("pa", 3), A("#", 0x5A5), A("r", 4), DRC(1), f1=B31),
             "eld", {"size": size, "drc": 1}, [r(1), r(2), k("pa", 3), imm(0x5A5), r(4)])
    ELD = lambda **kw: U(0x93, A("r", 1), A("r", 2), A("r", 3), A("#", 0), A("r", 4), DRC(0), **kw)  # noqa: E731
    for f1, f2, flds in ((B31 | 1, 0, {"skipinv": 1}), (B31 | 4, 0, {"syncs": 1}), (B31 | 0x40, 0, {"rpt": 4}),
                         (B31 | 4 << 13, 0, {"pred": 6}), (B31, 4, {"f2_2": 1}), (B31, 8, {"f2_3": 1})):
        case(f"eld f1 {f1:#x} f2 {f2:#x}", ELD(f1=f1, f2=f2), "eld", flds)
    case("eld src kinds", U(0x93, A("pa", 1), A("pa", 2), A("pa", 3), A("#", 0x7FF), A("sa", 4), DRC(0), f1=B31),
         "eld", ops=[k("pa", 1), k("pa", 2), k("pa", 3), imm(0x7FF), k("sa", 4)])
    for spec, val, form, fld in (("h10-11", 3, "ld", "am"), ("h4-5", 3, "ld", "size")):
        v = hw.put(0xE8030000 << 32 | 0xA0208180, spec, val)
        synth(f"ld {fld}={val}", v & 0xFFFFFFFF, v >> 32, form, {fld: val})


# --------------------------------------------------------------------------
# hw 1f
# --------------------------------------------------------------------------

def special_checks():
    # flow control
    pout = om.HEAP + 0x3000
    case("ba label@10", U(0x95, A(0xB, 9)), "ba", ops=[imm(10)], offset=10)
    case("ba label+3", U(0x95, A(0x12, 9, 3)), "ba", ops=[imm(3)])
    case("ba #0x3ffff", U(0x95, A(0x15, 0x3FFFF)), "ba", ops=[imm(0x3FFFF)])
    case("br label, inst 5", U(0x96, A(0xB, 9)), "br", ops=[imm(-5)], base=pout - 8 * 5)
    case("br label+3, inst 5", U(0x96, A(0x12, 9, 3)), "br", ops=[imm(-2)], base=pout - 8 * 5)
    case("br far back", U(0x96, A(0xB, 9)), "br", ops=[imm(-0x20000)], base=pout - 8 * 0x20000)
    case("br #0x1ffff", U(0x96, A(0x15, 0x1FFFF)), "br", ops=[imm(0x1FFFF)])
    B = lambda **kw: U(0x96, A(0x15, 7), **kw)  # noqa: E731
    for f1, fld, val in ((8, "nosched", 1), (1 << 18, "syncend", 1), (1 << 19, "link", 1), (1 << 22, "f1_22", 1),
                         (1 << 13, "pred", 1), (2 << 13, "pred", 2), (3 << 13, "pred", 5), (4 << 13, "pred", 6),
                         (5 << 13, "pred", 3), (6 << 13, "pred", 4)):
        case(f"br f1 {f1:#x}", B(f1=f1), "br", {fld: val})
    case("br xsyncend", B(f3=0x10), "br", {"xsyncend": 1, "syncend": 1})
    case("ba link+pred", U(0x95, A(0x15, 7), f1=1 << 19 | 1 << 13 | 8 | 1 << 22), "ba",
         {"link": 1, "pred": 1, "nosched": 1, "f1_22": 1})
    case("lapc", U(0x97), "lapc", {"pred": 0}, [])
    for f1, f3, flds in ((8, 0, {"nosched": 1}), (1 << 18, 0, {"syncend": 1}), (0, 0x10, {"xsyncend": 1}),
                         (6 << 13, 0, {"pred": 4})):
        case(f"lapc f1 {f1:#x} f3 {f3:#x}", U(0x97, f1=f1, f3=f3), "lapc", flds)
    case("op0x98", U(0x98), "op0x98", {"nosched": 0}, [])
    case("op0x98 nosched", U(0x98, f1=8), "op0x98", {"nosched": 1})
    case("nop", U(0xF1), "nop", {"end": 0})
    for f1, f2, f3, flds in ((4, 0, 0, {"syncs": 1}), (8, 0, 0, {"nosched": 1}), (1 << 17, 0, 0, {"end": 1}),
                             (0, 1 << 26, 0, {"f2_26": 1}), (0, 0, 0x20, {"f3_5": 1})):
        case(f"nop {f1:#x} {f2:#x} {f3:#x}", U(0xF1, f1=f1, f2=f2, f3=f3), "nop", flds)
    # MOE control
    M = lambda op, *a, **kw: U(op, *a, **kw)  # noqa: E731
    case("smoa", M(0xBC, I(-512), I(511), I(-1), I(5), A(0xE, 1), A(0xE, 2), A(0xE, 3), A(0xE, 0)), "smoa",
         {"am0": 1, "am1": 2, "am2": 3, "am3": 0}, [imm(-512), imm(511), imm(-1), imm(5)])
    case("smoa am3", M(0xBC, I(0), I(0), I(0), I(0), A(0xE, 0), A(0xE, 0), A(0xE, 0), A(0xE, 3), f1=8), "smoa",
         {"am3": 3, "nosched": 1})
    case("smlsi incs", M(0xBE, I(-128), I(127), I(-1), I(1), *[I(0)] * 7), "smlsi",
         {"lim0": 0}, [imm(-128), imm(127), imm(-1), imm(1)])
    swz = lambda s: sum("xyzw".index(c) << (3 * i) for i, c in enumerate(s))  # noqa: E731
    for i in range(4):
        vals = [I(1)] * 4
        vals[i] = A(0xF, swz("wzyx"))
        flags = [I(0)] * 4
        flags[i] = I(1)
        want = [imm(1)] * 4
        want[i] = {"kind": "#", "n": 0, "sfx": ["wzyx"]}
        case(f"smlsi swizzle {i}", M(0xBE, *vals, *flags, *[I(0)] * 3), "smlsi", ops=want)
    for i, fld in enumerate(("lim0", "lim1", "lim2")):
        lims = [I(0)] * 3
        lims[i] = I(0x3C)
        case(f"smlsi {fld}", M(0xBE, *[I(0)] * 8, *lims), "smlsi", {fld: 15})
    case("smlsi nosched", M(0xBE, *[I(1)] * 4, *[I(0)] * 7, f1=8), "smlsi", {"nosched": 1})
    for op, name in ((0xBD, "smr"), (0xBF, "smbo")):
        case(name, M(op, I(0xFFF), I(0xABC), I(0x123), I(1)), name, ops=[imm(0xFFF), imm(0xABC), imm(0x123), imm(1)])
        case(name + " nosched", M(op, I(0), I(0), I(0), I(0), f1=8), name, {"nosched": 1})
    case("imo", M(0xC0, I(-32), I(31), I(-1), I(0)), "imo", ops=[imm(-32), imm(31), imm(-1), imm(0)])
    for op, name in ((0xBC, "smoa"), (0xBD, "smr"), (0xBE, "smlsi"), (0xBF, "smbo")):
        case(name + ".r", M(op, A("sa", 4), A("c", 5), f1=8), name, {"nosched": 1}, [k("sa", 4), k("c", 5)],
             src="register form")
    for a, b in ((0, 0), (1, 0), (0, 1), (1, 1)):
        case(f"setfc {a},{b}", M(0xC1, I(a), I(b)), "setfc", ops=[imm(a), imm(b)])
    case("setfc nosched", M(0xC1, I(0), I(0), f1=8), "setfc", {"nosched": 1})
    for t in ("r", "o", "pa", "sa", "g", "x"):
        want = {"x": {"kind": "idx", "n": 3}}.get(t, k(t, 3))
        case(f"op0xfd dst {t}", M(0xFD, A(t, 3), I(8)), "op0xfd", ops=[want, imm(8)])
    case("op0xfd flags", M(0xFD, A("r", 3), I(1), f1=9), "op0xfd", {"skipinv": 1, "nosched": 1})
    # other
    for d, p in ((0, 0), (1, 0), (0, 1), (1, 1)):
        case(f"idf drc{d} #{p}", U(0x99, DRC(d), I(p)), "idf", {"drc": d, "path": p}, [])
    case("idf nosched", U(0x99, DRC(0), I(0), f1=8), "idf", {"nosched": 1})
    case("wdf drc1", U(0x9A, DRC(1), f1=8), "wdf", {"drc": 1, "nosched": 1}, [])
    case("wdf drc0", U(0x9A, DRC(0)), "wdf", {"drc": 0})
    case("op0xd7", U(0xD7, I(15), f1=8), "op0xd7", {"nosched": 1}, [imm(15)])
    case("limm", U(0x9B, A("r", 1), I(0xDEADBEEF)), "limm", ops=[r(1), imm(0xDEADBEEF)])
    case("limm inverted", U(0x9B, A("r", 1), A(8, 5, 0x800)), "limm", ops=[r(1), imm(0xFFFFFFFA)])
    case("limm label@7", U(0x9B, A("pa", 2), A(0xB, 3)), "limm", ops=[k("pa", 2), imm(7)], offset=7)
    for f1, fld, val in ((1, "skipinv", 1), (8, "nosched", 1), (1 << 17, "end", 1), (1 << 13, "pred", 1),
                         (7 << 13, "pred", 7), (3 << 13, "pred", 5)):
        case(f"limm f1 {f1:#x}", U(0x9B, A("r", 1), I(7), f1=f1), "limm", {fld: val})
    for t, want in (("o", k("o", 2)), ("sa", k("sa", 2)), ("i", k("i", 2)), ("g", k("g", 2)),
                    ("x", {"kind": "idx", "n": 2})):
        case(f"limm dst {t}", U(0x9B, A(t, 2), I(7)), "limm", ops=[want, imm(7)])
    # emit
    for op, tgt, f12 in ((0x101, 1, 1), (0x102, 1, 0), (0x103, 3, 1), (0x104, 3, 0)):
        srcs = [A("r", 2), A("sa", 3)] if op < 0x103 else [A("pa", 4)]
        want = [r(0), r(2), k("sa", 3)] if op < 0x103 else [r(0), imm(0), k("pa", 4)]
        case(f"emit {op:#x}", U(op, I(5), *srcs), "emit", {"tgt": tgt, "f12": f12, "n": 5}, want)
    for op, f3, flds in ((0x101, 0x40, {"f10": 1}), (0x103, 0x40, {"f10": 1}), (0x102, 0x80, {"f10": 1}),
                         (0x104, 0x80, {"f10": 1}), (0x101, 0x100, {"f13": 1}), (0x101, 0x100000, {"f3_20": 1}),
                         (0x102, 0x100000, {"f3_20": 1})):
        srcs = [I(0), I(1), I(0)] if op < 0x103 else [I(0), I(1)]
        case(f"emit {op:#x} f3 {f3:#x}", U(op, *srcs, f3=f3), "emit", flds)
    case("emit end+nosched", U(0x101, I(3), I(1), I(0), f1=0x20008), "emit", {"end": 1, "nosched": 1, "n": 3})
    case("emit 0x9c", U(0x9C, I(2), A("pa", 2), A("r", 3)), "emit", {"tgt": 0, "imm": 0x800, "n": 2},
         [k("pa", 2), r(3), imm(0)])
    case("emit 0x9d", U(0x9D, I(1), A("sa", 2), A("r", 3), A("c", 4), I(0x3FFF)), "emit",
         {"tgt": 0, "imm": 0x3FFF}, [k("sa", 2), r(3), k("c", 4)])
    case("emit 0xa2", U(0xA2, I(1), A("r", 2), A("r", 3), A("r", 4), I(0x1234), f2=0x30), "emit",
         {"tgt": 2, "imm": 0x1234, "f12": 1, "f13": 1})
    case("lock", U(0xCD, I(7), f1=8), "lock", {"nosched": 1}, [imm(7)])
    case("release", U(0xCE, I(3)), "release", ops=[imm(3)])
    case("ldr imm", U(0xD0, A("pa", 1), I(0x3FFF), DRC(1)), "ldr", {"drc": 1}, [k("pa", 1), imm(0x3FFF)])
    for t, want in (("r", r(5)), ("o", k("o", 5)), ("pa", k("pa", 5)), ("sa", k("sa", 5)), ("c", k("c", 5)),
                    ("i", k("i", 5)), ("g", k("g", 5))):
        case(f"ldr addr {t}", U(0xD0, A("r", 1), A(t, 5), DRC(0)), "ldr", ops=[r(1), want])
    case("ldr flags", U(0xD0, A("r", 1), I(5), DRC(0), f1=9), "ldr", {"skipinv": 1, "nosched": 1})
    case("str imm", U(0xD1, I(0x2A55), A("sa", 2), f1=9), "str", {"skipinv": 1, "nosched": 1},
         [imm(0x2A55), k("sa", 2)])
    for t, want in (("r", r(2)), ("#", imm(2)), ("g", k("g", 2)), ("i", k("i", 2))):
        case(f"str data {t}", U(0xD1, A("pa", 5), A(t, 2)), "str", ops=[k("pa", 5), want])
    case("op0xd6", U(0xD6, I(5), f1=8, f3=0x40), "op0xd6", {"nosched": 1, "f3_6": 1}, [imm(5)])
    # visibility test
    case("pcoeff", U(0xD2, A("sa", 1), A("c", 2), A("#", 3), f1=0x20008), "pcoeff", {"end": 1, "nosched": 1},
         [k("sa", 1), k("c", 2), imm(3)])
    case("pcoeff o", U(0xD2, A("o", 1), A("r", 2), A("r", 3)), "pcoeff", ops=[k("o", 1), r(2), r(3)])
    case("ptoff", U(0xD3, f1=0x20008), "ptoff", {"end": 1, "nosched": 1}, [])
    D4 = lambda pd=A(0xC, 2), e1=E(0x15), e2=E(0x35), s0=A("r", 2), d=A("r", 1), **kw: U(  # noqa: E731
        0xD4, d, pd, s0, A("r", 3), A("r", 4), e1, e2, **kw)
    case("atst8", D4(), "atst8", {"sel34": 0, "sel36": 0, "sel37": 0},
         [r(1), {"kind": "p", "n": 2}, r(2), r(3), r(4)])
    case("atst8 no write", D4(pd=A(0xC, 3, 0x10000)), "atst8",
         ops=[None, {"kind": "p", "n": 3, "sfx": ["nw"]}, None, None, None])
    for kw, flds in ((dict(e1=E(0x34)), {"sel34": 1}), (dict(e2=E(0x36)), {"sel36": 1}),
                     (dict(e2=E(0x37)), {"sel37": 1}), (dict(f2=1 << 25), {"f2_25": 1}),
                     (dict(f1=4), {"syncs": 1}), (dict(f1=8), {"nosched": 1}), (dict(f1=1 << 17), {"end": 1})):
        case(f"atst8 {flds}", D4(**kw), "atst8", flds)
    for t in ("o", "pa"):
        case(f"atst8 dst {t}", D4(d=A(t, 1)), "atst8", ops=[k(t, 1), None, None, None, None])
    case("atst8 src0 sa", D4(s0=A("sa", 9)), "atst8", ops=[None, None, k("sa", 9), None, None])
    D5 = lambda e1=E(0x15), e2=E(0x35), **kw: U(0xD5, A("pa", 2), A("r", 3), A("c", 4), e1, e2, **kw)  # noqa: E731
    case("depthf", D5(), "depthf", {"pred": 0}, [k("pa", 2), r(3), k("c", 4)])
    for kw, flds in ((dict(e1=E(0x34)), {"sel34": 1}), (dict(e2=E(0x37)), {"sel37": 1}),
                     (dict(f1=4), {"syncs": 1}), (dict(f1=8), {"nosched": 1}), (dict(f1=1 << 17), {"end": 1}),
                     (dict(f1=1 << 13), {"pred": 1}), (dict(f1=2 << 13), {"pred": 2}),
                     (dict(f1=3 << 13), {"pred": 3})):
        case(f"depthf {flds}", D5(**kw), "depthf", flds)
    # phase
    case("phas regs", U(0xD8, A("sa", 1), A("#", 2), f1=0x20008), "phas", {"end": 1, "nosched": 1},
         [k("sa", 1), imm(2)], src="0xd8")
    D9 = lambda a=0x123, b=0x3FC, e1=E(0x3C), e2=E(0x3F), e3=E(0x42), **kw: U(  # noqa: E731
        0xD9, I(a), I(b), e1, e2, e3, **kw)
    case("phas imm", D9(), "phas", {"rate": 0, "wait": 1, "sel43": 0}, [imm(0x123), imm(0xFF)], src="0xd9")
    for kw, flds in ((dict(e1=E(0x3E)), {"rate": 1}), (dict(e1=E(0x3D)), {"rate": 2}),
                     (dict(e2=E(0x40)), {"wait": 2}), (dict(e2=E(0x41)), {"wait": 7}),
                     (dict(e2=A(0xE, 0)), {"wait": 0}), (dict(e3=E(0x43)), {"sel43": 1}),
                     (dict(f1=1 << 17), {"end": 1}), (dict(a=0x3FFFF), {})):
        case(f"phas imm {flds}", D9(**kw), "phas", flds, src="0xd9")
    for op in (0xFF, 0xFE, 0x100):
        case(f"op{op:#x}", U(op, A("o", 1), A("i", 2), I(3), f1=8, f3=0x1800), f"op{op:#x}",
             {"nosched": 1, "f3_11": 1, "f3_12": 1}, [k("o", 1), k("i", 2), imm(3)])


def corpus_checks():
    """every hw 1c-1f word the encoder logs and the corpora hold"""
    import glob
    import usp
    here = os.path.dirname(os.path.abspath(__file__))
    words = set()
    for p in glob.glob(os.path.join(here, "corpus", "c*", "*.usp")):
        for lo, hi in usp.load(p)["insts"]:
            if hi >> 27 >= 0x1C:
                words.add((lo, hi))
    for lo, hi in sorted(words):
        N[0] += 1
        roundtrip("corpus", lo, hi)
    return len(words)


# --------------------------------------------------------------------------
# mutation sweep: a valid instruction per USEASM opcode, then every flags /
# test bit on its own, every argument with every register type and a spread
# of numbers, every argument flag bit, indexed arguments and one extra
# argument. Whatever libusc accepts must decode with no unexplained bit and
# round-trip through the text.

B31 = 0x80000000
SMP_ARGS = [A("r", 1), A("pa", 2), A("sa", 3), DRC(), E(0x15)]
SMP_LOD_ARGS = [A("r", 1), A("pa", 2), A("sa", 3), A("r", 4), DRC(), E(0x15)]
TEMPLATES = {
    0x6F: (SMP_ARGS, 0x200), 0x70: (SMP_ARGS, 0x200), 0x73: (SMP_LOD_ARGS, 0x200),
    0x76: (SMP_LOD_ARGS, 0x200), 0x79: (SMP_LOD_ARGS, 0x200),
    0x7D: ([A("r", 1), A("r", 2), A("#", 3), A("#", 0), DRC()], B31),
    0x85: ([A("r", 1), A("r", 2), A("#", 3), A("r", 9), DRC()], B31 | 1 << 29),
    0x8D: ([A("r", 1), A("r", 2), A("r", 3), A("#", 0), DRC()], B31),
    0x81: ([A("r", 1), A("#", 2), A("r", 3)], B31),
    0x91: ([A("r", 1), A("r", 2), A("r", 3)], B31),
    0x93: ([A("r", 1), A("r", 2), A("r", 3), I(5), A("r", 4), DRC()], B31),
    0x94: ([A("r", 1), A("r", 2), A("r", 3), I(5), A("r", 4), DRC()], B31),
    0x95: ([A(0x15, 7)], 0), 0x96: ([A(0x15, 7)], 0), 0x97: ([], 0), 0x98: ([], 0),
    0x99: ([DRC(), I(0)], 0), 0x9A: ([DRC()], 0), 0x9B: ([A("r", 1), I(7)], 0),
    0x9C: ([I(1), A("r", 2), A("r", 3)], 0), 0x9D: ([I(1), A("r", 2), A("r", 3), A("r", 4), I(9)], 0),
    0xA2: ([I(1), A("r", 2), A("r", 3), A("r", 4), I(9)], 0),
    0x101: ([I(1), A("r", 2), A("r", 3)], 0), 0x102: ([I(1), A("r", 2), A("r", 3)], 0),
    0x103: ([I(1), A("r", 2)], 0), 0x104: ([I(1), A("r", 2)], 0),
    0xBC: ([I(1), I(2), I(3), I(4), A(0xE, 1), A(0xE, 2), A(0xE, 3), A(0xE, 0)], 0),
    0xBE: ([I(1), I(2), I(3), I(4), I(0), I(0), I(0), I(0), I(4), I(8), I(12)], 0),
    0xBD: ([I(1), I(2), I(3), I(4)], 0), 0xBF: ([I(1), I(2), I(3), I(4)], 0), 0xC0: ([I(1), I(2), I(3), I(4)], 0),
    0xC1: ([I(1), I(0)], 0), 0xCD: ([I(1)], 0), 0xCE: ([I(1)], 0),
    0xD0: ([A("r", 1), I(5), DRC()], 0), 0xD1: ([I(5), A("r", 2)], 0),
    0xD2: ([A("r", 1), A("r", 2), A("r", 3)], 0), 0xD3: ([], 0),
    0xD4: ([A("r", 1), A(0xC, 1), A("r", 2), A("r", 3), A("r", 4), E(0x15), E(0x35)], 0),
    0xD5: ([A("r", 2), A("r", 3), A("r", 4), E(0x15), E(0x35)], 0),
    0xD6: ([I(1)], 0), 0xD7: ([I(1)], 0),
    0xD8: ([A("r", 1), A("r", 2)], 0), 0xD9: ([I(5), I(8), E(0x3D), E(0x40), E(0x43)], 0),
    0xF1: ([], 0), 0xFD: ([A("r", 3), I(1)], 0),
    0xFE: ([A("r", 1), A("r", 2), I(1)], 0), 0xFF: ([A("r", 1), A("r", 2), I(1)], 0),
    0x100: ([A("r", 1), A("r", 2), I(1)], 0),
}
MUT_TYPES = [0, 1, 2, 3, 4, 5, 6, 7, 8, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x10, 0x11, 0x12, 0x13, 0x15]
MUT_NUMS = [0, 1, 2, 3, 5, 0x15, 0x34, 0x37, 0x3E, 0x41, 0x43, 0x45, 0x7F, 0x80, 0x3FC, 0xFFF]


def mutation_sweep():
    o = oracle()
    stat = {"ok": 0, "rejected": 0}
    bad = {}

    def run(op, args, f1, f2=0, f3=0, test=0):
        try:
            ret, lo, hi, errs, calls = o.encode(U(op, *args, f1=f1, f2=f2, f3=f3, test=test))
        except Exception:  # noqa: BLE001
            stat["rejected"] += 1
            return
        if errs or ret != 2:
            stat["rejected"] += 1
            return
        stat["ok"] += 1
        x = hw.decode(lo, hi)
        if x is None or x.unknown:
            bad.setdefault((op, "decode"), f"{hi:08x} {lo:08x}")
            return
        t = hw.fmt(x)
        try:
            if hw.encode(hw.parse(t)) != (lo, hi):
                bad.setdefault((op, "round trip"), t)
        except Exception as e:  # noqa: BLE001
            bad.setdefault((op, "parse"), f"{t}: {e}")

    for op, (args, f1) in TEMPLATES.items():
        run(op, args, f1)
        for b in range(32):
            run(op, args, f1 | 1 << b)
            run(op, args, f1, f2=1 << b)
            run(op, args, f1, f3=1 << b)
            run(op, args, f1, test=1 << b)
        for i in range(len(args)):
            for t in MUT_TYPES:
                for n in MUT_NUMS:
                    a = list(args)
                    a[i] = A(t, n, args[i][2], args[i][3])
                    run(op, a, f1)
            for b in range(32):
                a = list(args)
                a[i] = A(args[i][1], args[i][0], 1 << b)
                run(op, a, f1)
            for ix in (1, 2):
                a = list(args)
                a[i] = A(args[i][1], args[i][0], args[i][2], ix)
                run(op, a, f1)
        for t in MUT_TYPES:
            run(op, args + [A(t, 0)], f1)
    N[0] += stat["ok"]
    for (op, what), ex in bad.items():
        FAIL.append(f"mutation sweep, USEASM {op:#x}: {what}: {ex}")
    return stat


def main():
    smp_checks()
    ld_checks()
    special_checks()
    nc = corpus_checks()
    ms = mutation_sweep()
    want = {"smp", "ld", "st", "eld", "ba", "br", "lapc", "op0x98", "nop", "smoa", "smr", "smlsi", "smbo", "imo",
            "setfc", "op0xfd", "idf", "wdf", "op0xd7", "emit", "limm", "lock", "release", "ldr", "str", "op0xd6",
            "pcoeff", "ptoff", "atst8", "depthf", "phas", "op0xff", "op0xfe", "op0x100"}
    seen = {s.split()[0] for s in SEEN}
    missing = want - seen
    print(f"{N[0]} checks ({nc} corpus words; mutation sweep {ms['ok']} accepted, {ms['rejected']} rejected), "
          f"forms exercised: {len(seen)}")
    if missing:
        FAIL.append(f"forms never exercised: {sorted(missing)}")
    for f in FAIL:
        print("FAIL", f)
    print("ok" if not FAIL else f"{len(FAIL)} failures")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
