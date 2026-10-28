"""Oracle sweep for hw_int.py: hardware opcodes 07, 10-19, 1a.

  cd $S && LIBUSC=$S/bin/libusc.so $S/venv/bin/python sgx/tools/isa/check_int.py

Every case encodes a USEASM instruction with libusc's encoder (oracle.py),
decodes it, checks the form, that no bit is unexplained, that the decoded
fields and operands are the ones put in, and that fmt -> parse -> encode gives
the same 64 bits. USEASM 0x66/0x6a/0x6d/0x6e are rejected for SGX545; they run
with the target patched (word0 | 2, target flags & ~0x14000), see patched().
"""
import os
import struct
import sys

S = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(S, "sgx", "re", "isa"))
os.environ.setdefault("LIBUSC", os.path.join(S, "bin", "libusc.so"))
from uinst import U, A, try_enc, oracle  # noqa: E402
import hw  # noqa: E402
# raw register numbers: the encoder knows nothing of format control (hw.CTX),
# so check with it off; with it on, number bit 6 reads as .c10/.f16 instead
hw.CTX["fc"] = 0

FEAT = 0x10000000 + 0x14DE40        # SGX545 feature word0 (oracle mapping)
TFL = 0x10000000 + 0x1498A0 + 8     # core-table flags word (FUN_0011dd80..)
I = 0x10                            # USEASM_REGTYPE_INTSRCSEL


def patched(inst):
    mu = oracle().mu
    w0 = struct.unpack("<I", mu.mem_read(FEAT, 4))[0]
    tf = struct.unpack("<I", mu.mem_read(TFL, 4))[0]
    mu.mem_write(FEAT, struct.pack("<I", w0 | 2))
    mu.mem_write(TFL, struct.pack("<I", tf & ~0x14000))
    try:
        return try_enc(inst)
    finally:
        mu.mem_write(FEAT, struct.pack("<I", w0))
        mu.mem_write(TFL, struct.pack("<I", tf))


def isel(n, fl=0):
    return A(I, n, flags=fl)


def R(t, n, fl=0, ix=0):
    return A(t, n, flags=fl, index=ix)


FAIL = []
N = [0]


def linked(inst, link):
    """encode inst with a coissued USE_INST at dw61 (flags1 bit 25)"""
    from oracle import HEAP
    o = oracle()
    w = list(inst) + [0] * (64 - len(inst))
    w[1] |= 1 << 25
    lk = list(link) + [0] * (64 - len(link))
    o.mu.mem_write(HEAP + 0x2400, struct.pack("<64I", *[x & 0xFFFFFFFF for x in lk]))
    w[61] = HEAP + 0x2400
    try:
        ret, lo, hi, errs, calls = o.encode(w)
    except Exception:  # noqa: BLE001
        return None
    return None if errs else (lo, hi)


def case(tag, inst, form, fields=None, ops=None, patch=False, link=None, reject=False):
    N[0] += 1
    if link is not None:
        r = linked(inst, link)
    else:
        r = patched(inst) if patch else try_enc(inst)
    if reject:
        if r is not None:
            FAIL.append(f"{tag}: encoder accepted, expected a reject")
        return
    if r is None:
        FAIL.append(f"{tag}: encoder rejected")
        return
    lo, hi = r
    x = hw.decode(lo, hi)
    if x is None or x.form.name != form:
        FAIL.append(f"{tag}: {hi:08x} {lo:08x} decodes as {x and x.form.name}")
        return
    if x.unknown:
        FAIL.append(f"{tag}: unexplained bits {x.unknown}")
    for k, v in (fields or {}).items():
        if x.d.get(k) != v:
            FAIL.append(f"{tag}: field {k}={x.d.get(k)} want {v}")
    for i, want in (ops or {}).items():
        got = x.ops[i]
        for k, v in want.items():
            if got.get(k) != v:
                FAIL.append(f"{tag}: op{i} {k}={got.get(k)} want {v} ({got})")
    t = hw.fmt(x)
    try:
        back = hw.encode(hw.parse(t))
    except Exception as e:  # noqa: BLE001
        FAIL.append(f"{tag}: '{t}' does not parse: {e}")
        return
    if back != (lo, hi):
        FAIL.append(f"{tag}: '{t}' -> {back[1]:08x} {back[0]:08x}, want {hi:08x} {lo:08x}")


def flags_cases(tag, build, form, patch=False, rmax=8, end=True, link=None):
    """common fields: pred (2-bit), skipinv, nosched, end, repeat"""
    for p, want in ((0, 0), (1, 1), (2, 2), (3, 3)):
        case(f"{tag} pred{p}", build(f1=0x200 | p << 13), form, {"pred": want}, patch=patch, link=link)
    case(f"{tag} skipinv", build(f1=0x201), form, {"skipinv": 1}, patch=patch, link=link)
    case(f"{tag} nosched", build(f1=0x208), form, {"nosched": 1}, patch=patch, link=link)
    if end:
        case(f"{tag} end", build(f1=0x200 | 1 << 17), form, {"end": 1}, patch=patch, link=link)
    if rmax:
        for n in range(1, rmax + 1):
            case(f"{tag} rpt{n}", build(f1=n << 4), form, {"rpt": n}, patch=patch, link=link)


KINDS = [("r", 5, {"kind": "r", "n": 5}), ("o", 6, {"kind": "o", "n": 6}), ("pa", 7, {"kind": "pa", "n": 7}),
         ("sa", 8, {"kind": "sa", "n": 8}), ("c", 9, {"kind": "c", "n": 9}), ("g", 10, {"kind": "g", "n": 10}),
         ("i", 1, {"kind": "i", "n": 1}), ("#", 100, {"kind": "#", "n": 100}), ("r", 123, {"kind": "r", "n": 123})]
DKINDS = [("r", 5, {"kind": "r", "n": 5}), ("o", 6, {"kind": "o", "n": 6}), ("pa", 7, {"kind": "pa", "n": 7}),
          ("sa", 8, {"kind": "sa", "n": 8}), ("g", 10, {"kind": "g", "n": 10}), ("i", 2, {"kind": "i", "n": 2})]


def operand_cases(tag, build, form, slot_args, patch=False, link=None, ix=True, noix=(1,)):
    """slot_args: {opd index in text: (arg index, kinds)}; index = pa[idx1+3]/[idx2+5]"""
    for oi, (ai, kinds) in slot_args.items():
        for t, n, want in kinds:
            case(f"{tag} op{oi} {t}{n}", build(sub={ai: R(t, n)}), form, ops={oi: want}, patch=patch, link=link)
        if not ix or (oi in noix and kinds is S0K):
            continue
        for x, off in ((1, 3), (2, 5)):
            case(f"{tag} op{oi} ix{x}", build(sub={ai: R("pa", off, ix=x)}), form,
                 ops={oi: {"kind": "ix", "bank": "pa", "idx": x, "off": off}}, patch=patch, link=link)


def builder(op, args):
    def build(f1=0x200, f2=0, f3=0, sub=None):
        a = list(args)
        for k, v in (sub or {}).items():
            a[k] = v
        return U(op, *a, f1=f1, f2=f2, f3=f3)
    return build


def r(n):
    return R("r", n)


# INTSRCSEL -> hardware select codes, from the encoders' switch tables
SOP2_C1 = {0: 0, 5: 1, 6: 2, 8: 3, 9: 4, 2: 5, 0x18: 6}
SOP2_C2 = {0: 0, 5: 1, 6: 2, 8: 3, 9: 4, 2: 5, 0x19: 6}
SOP2_A1 = {0: 0, 8: 1, 9: 2, 0x18: 3}
SOP2_A2 = {0: 0, 8: 1, 9: 2, 0x19: 3}
SEL3 = {0: 0, 2: 1, 5: 2, 8: 3, 4: 4, 7: 5, 6: 6, 9: 7}
WMSEL = {0: 0, 2: 1, 5: 2, 8: 3, 6: 6, 9: 7}
OPN = {0x12: 0, 0x13: 1, 0x16: 2, 0x17: 3}
EKINDS = [k for k in KINDS if k[0] in ("r", "o", "pa", "sa")]
DK4 = [("r", 5, {"kind": "r", "n": 5}), ("o", 6, {"kind": "o", "n": 6}), ("pa", 7, {"kind": "pa", "n": 7})]
S0K = [("r", 5, {"kind": "r", "n": 5}), ("pa", 7, {"kind": "pa", "n": 7}), ("i", 2, {"kind": "r", "n": 126})]


def sels(table, comp_bits, cw):
    """(INTSRCSEL number, complement flag, expected field value) for a select
    whose field = code | complement << cw (ONE = complement of ZERO)"""
    out = []
    for n, code in table.items():
        for c in (0, 1):
            out.append((n, c, code | c << cw))
    if 0 in table:
        out += [(1, 0, 1 << cw), (1, 1, 0)]
    if 2 in table:
        out += [(3, 0, table[2] | 1 << cw), (3, 1, table[2])]
    return out


def run_a():
    # ---- efo (0x13): control word = USEASM arg4 immediate
    def efo(ctl=0x2B0600, **k):
        return builder(0x13, [r(1), r(2), r(3), r(4), R("#", ctl)])(**k)
    flags_cases("efo", efo, "efo", rmax=4, end=False)
    case("efo end rejected", efo(f1=0x200 | 1 << 17), "efo", reject=True)
    for name, sh, n in (("d", 20, 4), ("i", 18, 4), ("a", 16, 4), ("m", 14, 4)):
        for v in range(n):
            case(f"efo {name}{v}", efo(ctl=v << sh), "efo", {name: v})
    for name, bit in (("wi0", 10), ("wi1", 9), ("a0rneg", 26), ("a0lneg", 22)):
        case(f"efo {name}", efo(ctl=1 << bit), "efo", {name: 1})
    case("efo f3_10", efo(f3=0x400), "efo", {"f3_10": 1})
    b = builder(0x13, [r(1), r(2), r(3), r(4), R("#", 0x2B0600)])
    operand_cases("efo", b, "efo", {0: (0, DK4), 1: (1, EKINDS), 2: (2, EKINDS), 3: (3, KINDS)}, ix=False)
    for x, off in ((1, 3), (2, 5)):
        for oi in (0, 3):
            case(f"efo op{oi} ix{x}", b(sub={oi: R("pa", off, ix=x)}), "efo",
                 ops={oi: {"kind": "ix", "bank": "pa", "idx": x, "off": off}})
    case("efo dst i1", b(sub={0: R("i", 1)}), "efo", ops={0: {"kind": "r", "n": 125}})
    case("efo src0 i0", b(sub={1: R("i", 0)}), "efo", ops={1: {"kind": "r", "n": 124}})
    for i in (1, 2, 3):
        case(f"efo neg{i}", b(sub={i: R("r", i + 1, 1)}), "efo", ops={i: {"neg": 1}})
        case(f"efo f16 {i}", b(sub={i: R("r", 2, 1 << 22)}), "efo", ops={i: {"n": 66}})
    case("efo 0xb0 rejected", U(0xB0, r(1), r(2), r(3), r(4), R("#", 0), f1=0x200), "efo", reject=True)

    # ---- sop2 (0x57 + 0xe3)
    def sop2(c1=(5, 0), c2=(6, 0), cop=0x12, cmod=0x15, a1=(8, 0), a2=(9, 0), aop=0x12, amod=0x15, aneg=0x15,
             f1=0x200, f2=0, sub=None):
        a = [r(1), r(2), r(3), isel(cmod), isel(c1[0], c1[1] << 15), isel(c2[0], c2[1] << 15), isel(cop)]
        for k, v in (sub or {}).items():
            a[k] = v
        return (U(0x57, *a, f1=f1, f2=f2),
                U(0xE3, isel(amod), isel(a1[0], a1[1] << 15), isel(a2[0], a2[1] << 15), isel(aop), isel(aneg)))

    def lcase(tag, pair, form, fields=None, ops=None):
        case(tag, pair[0], form, fields, ops, link=pair[1])
    lb = sop2()
    flags_cases("sop2", lambda **k: sop2(**k)[0], "sop2", link=lb[1])
    operand_cases("sop2", lambda **k: sop2(**k)[0], "sop2", {0: (0, DKINDS), 1: (1, KINDS), 2: (2, KINDS)},
                  link=lb[1])
    case("sop2 no coissue", sop2()[0], "sop2", reject=True)
    for n, c, v in sels(SOP2_C1, 0, 3):
        lcase(f"sop2 c1 {n:#x}/{c}", sop2(c1=(n, c)), "sop2", {"c1": v})
    for n, c, v in sels(SOP2_C2, 0, 3):
        lcase(f"sop2 c2 {n:#x}/{c}", sop2(c2=(n, c)), "sop2", {"c2": v})
    for n, c, v in sels(SOP2_A1, 0, 2):
        lcase(f"sop2 a1 {n:#x}/{c}", sop2(a1=(n, c)), "sop2", {"a1": v})
    for n, c, v in sels(SOP2_A2, 0, 2):
        lcase(f"sop2 a2 {n:#x}/{c}", sop2(a2=(n, c)), "sop2", {"a2": v})
    for n, v in OPN.items():
        lcase(f"sop2 cop {n:#x}", sop2(cop=n), "sop2", {"cop": v})
        lcase(f"sop2 aop {n:#x}", sop2(aop=n), "sop2", {"aop": v})
    lcase("sop2 cs1c", sop2(cmod=0x1A), "sop2", {"cs1c": 1})
    lcase("sop2 as1c", sop2(amod=0x1A), "sop2", {"as1c": 1})
    lcase("sop2 aneg", sop2(aneg=0x14), "sop2", {"aneg": 1})
    lcase("sop2 c10 src", sop2(f2=1 << 24, sub={1: R("r", 2, 1 << 24)}), "sop2", ops={1: {"n": 66}})

    # ---- sopwm (0x58; 0xfa)
    def wm(c1=(5, 0), c2=(6, 0), cop=0x12, aop=0x12, d=None, f1=0x200, f2=0, sub=None):
        a = [d or r(1), r(2), r(3), isel(c1[0], c1[1] << 15), isel(c2[0], c2[1] << 15), isel(cop), isel(aop)]
        for k, v in (sub or {}).items():
            a[k] = v
        return U(0x58, *a, f1=f1, f2=f2)
    flags_cases("sopwm", wm, "sopwm", rmax=0)
    case("sopwm repeat rejected", wm(f1=0x220), "sopwm", reject=True)
    operand_cases("sopwm", wm, "sopwm", {0: (0, DKINDS), 1: (1, KINDS), 2: (2, KINDS)})
    for m in range(16):
        case(f"sopwm mask {m}", wm(d=R("r", 1, 0x100000 | m << 7)), "sopwm", {"mask": m})
    for n, c, v in sels(WMSEL, 0, 3):
        case(f"sopwm c1 {n:#x}/{c}", wm(c1=(n, c)), "sopwm", {"c1": v})
        case(f"sopwm c2 {n:#x}/{c}", wm(c2=(n, c)), "sopwm", {"c2": v})
    for n, v in OPN.items():
        case(f"sopwm cop {n:#x}", wm(cop=n), "sopwm", {"cop": v})
        case(f"sopwm aop {n:#x}", wm(aop=n), "sopwm", {"aop": v})
    case("sopwm 0xfa", U(0xFA, r(1), r(2), r(3), f1=0x200), "sopwm",
         {"c1": 3, "c2": 3 | 8, "cop": 0, "aop": 0})

    # ---- sop3 family (hw 11)
    def s3(c1=(5, 0), c2=(6, 0), cop=0x12, cneg=0x15, op=0x59, f1=0x200, sub=None):
        a = [r(1), r(2), r(3), r(4), isel(c1[0], c1[1] << 15), isel(c2[0], c2[1] << 15), isel(cop), isel(cneg)]
        for k, v in (sub or {}).items():
            a[k] = v
        return U(op, *a, f1=f1)
    asop = U(0xDE, isel(7), isel(0x12))
    flags_cases("sop3", s3, "sop3", rmax=0, link=asop)
    operand_cases("sop3", s3, "sop3", {0: (0, DKINDS), 1: (1, S0K), 2: (2, KINDS), 3: (3, KINDS)}, link=asop)
    case("sop3 no coissue", s3(), "sop3", reject=True)
    for n, c, v in sels(SEL3, 0, 3):
        case(f"sop3 c1 {n:#x}/{c}", s3(c1=(n, c)), "sop3", {"c1": v}, link=asop)
        case(f"sop3 c2 {n:#x}/{c}", s3(c2=(n, c)), "sop3", {"c2": v}, link=asop)
    case("sop3 cop sub", s3(cop=0x13), "sop3", {"cop": 1}, link=asop)
    for lop, rev in ((0xDE, 0), (0xDF, 1)):
        for n, code in ((0, 0), (7, 1), (8, 2), (9, 3)):
            for c in (0, 1):
                case(f"sop3 {lop:#x} a {n}/{c}", s3(), "sop3", {"a": code | c << 2, "arev": rev},
                     link=U(lop, isel(n, c << 15), isel(0x12)))
        case(f"sop3 {lop:#x} aop sub", s3(), "sop3", {"aop": 1, "arev": rev}, link=U(lop, isel(7), isel(0x13)))
    case("sop3 asop cneg rejected", s3(cneg=0x14), "sop3", reject=True, link=asop)
    for n1, c1, v1 in ((0, 0, 0), (8, 0, 1), (0, 1, 2), (8, 1, 3), (1, 0, 2), (1, 1, 0)):
        for n2, c2, v2 in ((0, 0, 0), (9, 0, 1), (0, 1, 2), (9, 1, 3), (1, 0, 2)):
            case(f"sop3l a {n1}/{c1} {n2}/{c2}", s3(), "sop3l", {"a1": v1, "a2": v2},
                 link=U(0xE0, isel(n1, c1 << 15), isel(n2, c2 << 15)))
    alrp = U(0xE0, isel(8), isel(9))
    case("sop3l cneg", s3(cneg=0x14), "sop3l", {"cneg": 1}, link=alrp)
    case("sop3l cop sub", s3(cop=0x13), "sop3l", {"cop": 1}, link=alrp)

    def lrp1(l1=(5, 0), l2=(6, 0), cs=4, f1=0x200, sub=None):
        a = [r(1), r(2), r(3), r(4), isel(l1[0], l1[1] << 15), isel(l2[0], l2[1] << 15), isel(cs)]
        for k, v in (sub or {}).items():
            a[k] = v
        return U(0xE1, *a, f1=f1)
    asop2 = U(0xDE, isel(5), isel(6), isel(0x12))
    flags_cases("lrp1", lrp1, "lrp1", rmax=0, link=asop2)
    operand_cases("lrp1", lrp1, "lrp1", {0: (0, DKINDS), 1: (1, S0K), 2: (2, KINDS), 3: (3, KINDS)}, link=asop2)
    for n, c, v in ((0, 0, 0), (5, 0, 1), (0, 1, 2), (5, 1, 3), (1, 0, 2), (1, 1, 0)):
        case(f"lrp1 l1 {n}/{c}", lrp1(l1=(n, c)), "lrp1", {"l1": v}, link=asop2)
    for n, c, v in ((0, 0, 0), (6, 0, 1), (0, 1, 2), (6, 1, 3), (1, 0, 2), (1, 1, 0)):
        case(f"lrp1 l2 {n}/{c}", lrp1(l2=(n, c)), "lrp1", {"l2": v}, link=asop2)
    case("lrp1 cs s0a", lrp1(cs=7), "lrp1", {"cs": 1}, link=asop2)
    for n, c, v in sels(SEL3, 0, 3):
        case(f"lrp1 c1 {n:#x}/{c}", lrp1(), "lrp1", {"c1": v}, link=U(0xDE, isel(n, c << 15), isel(6), isel(0x12)))
        case(f"lrp1 c2 {n:#x}/{c}", lrp1(), "lrp1", {"c2": v}, link=U(0xDE, isel(5), isel(n, c << 15), isel(0x12)))
    case("lrp1 aop sub", lrp1(), "lrp1", {"aop": 1}, link=U(0xDE, isel(5), isel(6), isel(0x13)))

    def e2(p6=(5, 0), p7=(6, 0), c1=(5, 0), c2=(6, 0), f1=0x200, sub=None):
        a = [r(1), r(2), r(3), r(4), isel(c1[0], c1[1] << 15), isel(c2[0], c2[1] << 15),
             isel(p6[0], p6[1] << 15), isel(p7[0], p7[1] << 15)]
        for k, v in (sub or {}).items():
            a[k] = v
        return U(0xE2, *a, f1=f1)
    flags_cases("sop3x", e2, "sop3x", rmax=0, link=alrp)
    operand_cases("sop3x", e2, "sop3x", {0: (0, DKINDS), 1: (1, S0K), 2: (2, KINDS), 3: (3, KINDS)}, link=alrp)
    for p6, p7, v in (((5, 0), (6, 0), 0), ((5, 0), (6, 1), 1), ((1, 0), (6, 0), 2), ((5, 1), (6, 0), 3)):
        case(f"sop3x p{v}", e2(p6=p6, p7=p7), "sop3x", {"p": v}, link=alrp)
    case("sop3x p 1-s1,1-s2 rejected", e2(p6=(5, 1), p7=(6, 1)), "sop3x", reject=True, link=alrp)
    for n, c, v in sels(SEL3, 0, 3):
        case(f"sop3x c1 {n:#x}/{c}", e2(c1=(n, c)), "sop3x", {"c1": v}, link=alrp)
        case(f"sop3x c2 {n:#x}/{c}", e2(c2=(n, c)), "sop3x", {"c2": v}, link=alrp)
    case("sop3x a", e2(), "sop3x", {"a1": 3, "a2": 2}, link=U(0xE0, isel(8, 0x8000), isel(1)))

    # ---- dot3/dot4 (0x5a-0x61)
    def dot(op=0x5A, cs=0xA, as_=0xE, f1=0x200, sub=None):
        a = [r(1), isel(cs), isel(as_), r(2), r(3)]
        for k, v in (sub or {}).items():
            a[k] = v
        return U(op, *a, f1=f1)
    for op in range(0x5A, 0x62):
        k = op - 0x5A
        want = {"f6": k >> 2 & 1}
        want.update({"a1": 3} if k & 2 else {"a2": 3})
        case(f"dot {op:#x}", dot(op), f"dot{3 + (k & 1)}", want)
    flags_cases("dot3", dot, "dot3")
    operand_cases("dot3", dot, "dot3", {0: (0, DKINDS), 1: (3, KINDS), 2: (4, KINDS)})
    for n in range(4):
        case(f"dot cs {n}", dot(cs=0xA + n), "dot3", {"cs": n})
        case(f"dot as {n}", dot(as_=0xE + n), "dot3", {"as": n})
    case("dot s1c", dot(sub={3: R("r", 2, 0x8000)}), "dot3", {"s1c": 1})
    case("dot s2c", dot(sub={4: R("r", 3, 0x8000)}), "dot3", {"s2c": 1})
    for lop, f4 in ((0xDA, 0), (0xDB, 1)):
        for n1, v1 in ((0, 0), (8, 1), (9, 2)):
            for n2, c2, v2 in ((0, 0, 0), (8, 0, 1), (9, 0, 2), (0, 1, 4), (9, 1, 6), (1, 0, 4)):
                case(f"dot3s {lop:#x} {n1} {n2}/{c2}", dot(), "dot3s", {"f4": f4, "a1": v1, "a2": v2},
                     link=U(lop, r(5), isel(n1), isel(n2, c2 << 15)))
        for t, n, want in S0K:
            case(f"dot3s src0 {t}", dot(), "dot3s", ops={1: want}, link=U(lop, R(t, n), isel(0), isel(0)))
    case("dot4s 0x5b rejected", dot(0x5B), "dot4s", reject=True, link=U(0xDA, r(5), isel(0), isel(0)))
    case("dot4s 0x5d", dot(0x5D), "dot4", {"a1": 3}, link=U(0xDA, r(5), isel(0), isel(0)))
    for lop, f4 in ((0xDC, 0), (0xDD, 1)):
        for asx in range(4):
            case(f"dot3 {lop:#x} as{asx}", dot(), "dot3", {"f4": f4, "as": asx},
                 link=U(lop, isel(0xE + asx), isel(0), isel(8), isel(0), isel(9)))
        for n1, c1, v1 in ((0, 0, 0), (8, 0, 1), (9, 0, 2), (8, 1, 5), (1, 0, 4)):
            case(f"dot3 {lop:#x} a1 {n1}/{c1}", dot(), "dot3", {"a1": v1},
                 link=U(lop, isel(0xE), isel(n1, c1 << 15), isel(8), isel(0), isel(9)))
        for n2, c2, v2 in ((0, 0, 0), (8, 0, 1), (9, 0, 2), (9, 1, 6), (1, 0, 4)):
            case(f"dot3 {lop:#x} a2 {n2}/{c2}", dot(), "dot3", {"a2": v2},
                 link=U(lop, isel(0xE), isel(0), isel(8), isel(n2, c2 << 15), isel(9)))
        case(f"dot3 {lop:#x} s1ac", dot(), "dot3", {"s1ac": 1},
             link=U(lop, isel(0xE), isel(0), isel(8, 0x8000), isel(0), isel(9)))

    # ---- ima8 / fpma (0x62 / 0xf4) and the 0xeb/0xec/0xf5/0xf6 rewrites
    for op, name in ((0x62, "ima8"), (0xF4, "fpma")):
        def im(c0=(4, 0), c1=(5, 0), c2=(6, 0), a0=(7, 0), a1c=0, a2c=0, f1=0x200, f2=0, sub=None, op=op):
            a = [r(1), r(2), r(3), r(4)] + [isel(n, c << 15) for n, c in (c0, c1, c2, a0, (8, a1c), (9, a2c))]
            for k, v in (sub or {}).items():
                a[k] = v
            return U(op, *a, f1=f1, f2=f2)
        flags_cases(name, im, name)
        operand_cases(name, im, name, {0: (0, DKINDS), 1: (1, S0K), 2: (2, KINDS), 3: (3, KINDS)})
        case(f"{name} sat", im(f2=0x400), name, {"sat": 1})
        case(f"{name} neg0", im(sub={1: R("r", 2, 1)}), name, ops={1: {"neg": 1}})
        for n, c, v in sels({4: 0, 5: 1, 7: 2, 8: 3}, 0, 2):
            case(f"{name} c0 {n}/{c}", im(c0=(n, c)), name, {"c0": v})
        for n, c, v in sels({5: 0, 8: 1}, 0, 1):
            case(f"{name} c1 {n}/{c}", im(c1=(n, c)), name, {"c1": v})
        for n, c, v in sels({6: 0, 9: 1}, 0, 1):
            case(f"{name} c2 {n}/{c}", im(c2=(n, c)), name, {"c2": v})
        for n, c, v in sels({7: 0, 8: 1}, 0, 1):
            case(f"{name} a0 {n}/{c}", im(a0=(n, c)), name, {"a0": v})
        case(f"{name} a1c", im(a1c=1), name, {"a1c": 1})
        case(f"{name} a2c", im(a2c=1), name, {"a2c": 1})
    for op, sat, neg in ((0xEB, 0, 0), (0xEC, 0, 1), (0xF5, 1, 0), (0xF6, 1, 1)):
        case(f"ima8 {op:#x}", U(op, r(1), r(2), r(3), f1=0x200), "ima8", {"sat": sat},
             {1: {"kind": "r", "n": 3 if neg else 2, **({"neg": 1} if neg else {})},
              2: {"kind": "#", "n": 1}, 3: {"kind": "r", "n": 2 if neg else 3}})

    # ---- ima16 (0x63) and the 0xe4-0xe9 rewrites
    def i16(f1s=0x2C, f2s=0x2C, fl=(0, 0, 0, 0), f1=0x200, f2=0x1000, sub=None):
        a = [R("r", 1, fl[0]), R("r", 2, fl[1]), R("r", 3, fl[2]), R("r", 4, fl[3]), isel(f1s), isel(f2s)]
        for k, v in (sub or {}).items():
            a[k] = v
        return U(0x63, *a, f1=f1, f2=f2)
    flags_cases("ima16", i16, "ima16")
    K16 = [k if k[0] != "#" else ("#", 50, {"kind": "#", "n": 50}) for k in KINDS]
    operand_cases("ima16", i16, "ima16", {0: (0, DKINDS), 1: (1, S0K), 2: (2, K16), 3: (3, K16)})
    case("ima16 unsigned", i16(f2=0), "ima16", {"sgn": 0})
    case("ima16 signed", i16(f2=0x1000), "ima16", {"sgn": 1})
    case("ima16 b11+b12 rejected", i16(f2=0x1800), "ima16", reject=True)
    case("ima16 sat", i16(f2=0x1400), "ima16", {"sat": 1, "sgn": 1})
    case("ima16 sat unsigned", i16(f2=0x400), "ima16", {"sat": 1, "sgn": 0})
    for k in range(4):
        case(f"ima16 f2_15 {k}", i16(f2=0x1000 | k << 15), "ima16", {"f2_15": k})
    case("ima16 f2_15 4 rejected", i16(f2=0x1000 | 4 << 15), "ima16", reject=True)
    case("ima16 dflag1", i16(fl=(2, 0, 0, 0)), "ima16", {"dflag1": 1})
    case("ima16 neg2", i16(fl=(0, 0, 0, 1)), "ima16", ops={3: {"neg": 1}})
    for n, v in ((0x2C, 0), (0x1E, 1), (0x1F, 2), (0x20, 3)):
        for hi in (0, 1):
            fl = 0 if v == 0 else (0x2000 if hi else 0x1000)
            if v == 0 and hi:
                continue
            case(f"ima16 f1 {n:#x} hi{hi}", i16(f1s=n, fl=(0, 0, fl, 0)), "ima16", {"f1": v, "s1hi": hi})
            case(f"ima16 f2 {n:#x} hi{hi}", i16(f2s=n, fl=(0, 0, 0, fl)), "ima16", {"f2": v, "s2hi": hi})
    for imm in (-64, -1, 63):
        case(f"ima16 simm {imm}", i16(sub={2: R("#", imm), 3: R("#", imm)}), "ima16",
             ops={2: {"kind": "#", "n": imm}, 3: {"kind": "#", "n": imm}})
    case("ima16 uimm", i16(f1s=0x1E, f2s=0x1E, sub={2: R("#", 100), 3: R("#", 127)}), "ima16",
         ops={2: {"kind": "#", "n": 100}, 3: {"kind": "#", "n": 127}})
    for op, sgn, neg in ((0xE4, 1, 0), (0xE5, 1, 1), (0xE7, 0, 0), (0xE8, 0, 1)):
        case(f"ima16 {op:#x}", U(op, r(1), r(2), r(3), f1=0x200), "ima16", {"sgn": sgn},
             {2: {"kind": "#", "n": 1}, 3: {"kind": "r", "n": 3, **({"neg": 1} if neg else {})}})
    for op, sgn in ((0xE6, 1), (0xE9, 0)):
        case(f"ima16 {op:#x}", U(op, r(1), r(2), r(3), f1=0x200), "ima16", {"sgn": sgn},
             {2: {"kind": "r", "n": 3}, 3: {"kind": "#", "n": 0}})

    # ---- imae (0x64) and the 0xea/0xf0 rewrites
    LO = 0x1000

    def ie(t2=0x27, co=0x15, ci=0x15, creg=None, fl=(0, LO, LO, LO), f1=0x200, f2=0x1000, sub=None):
        a = [R("r", 1, fl[0]), R("r", 2, fl[1]), R("r", 3, fl[2]), R("r", 4, fl[3]), isel(t2), isel(co), isel(ci)]
        if creg is not None:
            a.append(R("i", creg))
        for k, v in (sub or {}).items():
            a[k] = v
        return U(0x64, *a, f1=f1, f2=f2)
    flags_cases("imae", ie, "imae")
    operand_cases("imae", ie, "imae", {0: (0, DKINDS)})
    for oi in (1, 2, 3):
        for t, n in (("r", 5), ("pa", 7)):
            case(f"imae op{oi} {t}", ie(sub={oi: R(t, n, LO)}), "imae", ops={oi: {"kind": t, "n": n}})
    for oi in (2, 3):
        for t, n in (("o", 6), ("sa", 8), ("c", 9), ("g", 10), ("i", 1)):
            case(f"imae op{oi} {t}", ie(sub={oi: R(t, n, LO)}), "imae", ops={oi: {"kind": t, "n": n}})
    case("imae unsigned", ie(f2=0x800), "imae", {"sgn": 0})
    case("imae sat", ie(f2=0x1400), "imae", {"sat": 1})
    for k in range(8):
        case(f"imae f2_15 {k}", ie(f2=0x1000 | k << 15), "imae", {"f2_15": k})
    for n, v in ((0x27, 0), (0x28, 1), (0x29, 2)):
        case(f"imae t2 {n:#x}", ie(t2=n, fl=(0, LO, LO, LO if v < 2 else 0)), "imae", {"t2": v})
    for oi, f in ((1, "s0hi"), (2, "s1hi"), (3, "s2hi")):
        fl = [0, LO, LO, LO]
        fl[oi] = 0x2000
        case(f"imae {f}", ie(fl=tuple(fl)), "imae", {f: 1})
    case("imae cout", ie(co=0x2B, creg=0), "imae", {"cout": 1, "ci": 0})
    case("imae cin", ie(ci=0x2A, creg=1), "imae", {"cin": 1, "ci": 1})
    case("imae cin+cout", ie(co=0x2B, ci=0x2A, creg=1), "imae", {"cin": 1, "cout": 1, "ci": 1})
    for imm in (-64, -1, 63):
        case(f"imae simm {imm}", ie(t2=0x28, sub={2: R("#", imm, LO), 3: R("#", imm, LO)}), "imae",
             ops={2: {"kind": "#", "n": imm}, 3: {"kind": "#", "n": imm}})
    case("imae uimm", ie(f2=0x800, sub={2: R("#", 127, LO), 3: R("#", 127, LO)}), "imae",
         ops={2: {"kind": "#", "n": 127}, 3: {"kind": "#", "n": 127}})
    for op, sgn in ((0xEA, 1), (0xF0, 0)):
        case(f"imae {op:#x}", U(op, r(1), R("r", 2, LO), r(3), f1=0x200), "imae", {"sgn": sgn, "t2": 2},
             {2: {"kind": "#", "n": 1}, 3: {"kind": "r", "n": 3}})


def run():
    del FAIL[:]
    run_a()
    # ---- adif / adifsum
    b = builder(0x67, [r(1), r(2), r(3)])
    flags_cases("adif", b, "adif")
    operand_cases("adif", b, "adif", {0: (0, DKINDS), 1: (1, KINDS), 2: (2, KINDS)})
    b = builder(0x68, [r(1), r(2), r(3), r(4)])
    flags_cases("adifsum", b, "adifsum")
    operand_cases("adifsum", b, "adifsum", {0: (0, DKINDS), 2: (2, KINDS), 3: (3, KINDS)})
    for t, n in (("r", 9), ("pa", 9), ("i", 2)):
        want = {"kind": "r", "n": 126} if t == "i" else {"kind": t, "n": n}
        case(f"adifsum src0 {t}", b(sub={1: R(t, n)}), "adifsum", ops={1: want})
    case("adifsum s0f13", b(sub={1: R("r", 2, 0x2000)}), "adifsum", {"s0f13": 1})

    # ---- op16s1 (0x6e, patched)
    base = [r(1), r(2), r(3), r(4), isel(0x15), isel(0x15), R("i", 0), isel(0x39), isel(0x1e)]
    b = builder(0x6e, base)
    flags_cases("op16s1", b, "op16s1", patch=True, rmax=4)
    operand_cases("op16s1", b, "op16s1", {0: (0, DKINDS), 2: (2, KINDS), 3: (3, KINDS)}, patch=True)
    case("op16s1 src0 pa", b(sub={1: R("pa", 3)}), "op16s1", ops={1: {"kind": "pa", "n": 3}}, patch=True)
    for k in range(16):
        case(f"op16s1 f2 {k}", b(f2=k << 15), "op16s1", {"f2_15": k}, patch=True)
    case("op16s1 s8", b(sub={8: isel(0x1f)}), "op16s1", {"s8": 1}, patch=True)
    case("op16s1 x38", b(sub={4: isel(0x38)}), "op16s1", {"x38": 1}, patch=True)
    case("op16s1 x1c", b(sub={5: isel(0x1c)}), "op16s1", {"x1c": 1}, patch=True)
    for n, v in ((0x39, 0), (0x3a, 1), (0x3b, 2)):
        case(f"op16s1 x39 {n:#x}", b(sub={7: isel(n)}), "op16s1", {"x39": v}, patch=True)
    case("op16s1 i1", b(sub={6: R("i", 1)}), "op16s1", {"i": 1}, patch=True)

    # ---- bilin
    base = [r(1), r(2), r(3), r(4), isel(0x1e), isel(0x2e), isel(0x2f), isel(0x31), isel(0x15)]
    b = builder(0x6b, base)
    flags_cases("bilin", b, "bilin")
    operand_cases("bilin", b, "bilin", {0: (0, DKINDS), 2: (2, KINDS), 3: (3, KINDS)})
    case("bilin src0 pa", b(sub={1: R("pa", 3)}), "bilin", ops={1: {"kind": "pa", "n": 3}})
    for n, v in ((0x1e, 0), (0x1f, 1), (0x20, 2)):
        case(f"bilin fmt {v}", b(sub={4: isel(n)}), "bilin", {"fmt": v})
    for i, n, f in ((5, 0x2d, "ileave"), (6, 0x30, "src23"), (7, 0x32, "dst23"), (8, 0x33, "rnd")):
        case(f"bilin {f}", b(sub={i: isel(n)}), "bilin", {f: 1})
    for imm in (-64, -1, 63):
        case(f"bilin simm {imm}", b(sub={3: R("#", imm), 4: isel(0x1f)}), "bilin", ops={3: {"kind": "#", "n": imm}})
    case("bilin uimm 100", b(sub={3: R("#", 100)}), "bilin", ops={3: {"kind": "#", "n": 100}})

    # ---- ssum16 / ssum16i
    base = [r(1), r(2), r(3), r(4), isel(0x1e), isel(0x15), isel(0x15), isel(0x15)]
    b = builder(0x69, base)
    flags_cases("ssum16", b, "ssum16")
    operand_cases("ssum16", b, "ssum16", {0: (0, DKINDS), 3: (3, KINDS)})
    for t in ("r", "o", "pa", "sa"):
        case(f"ssum16 bank {t}", b(sub={1: R(t, 9), 2: R(t, 10)}), "ssum16",
             ops={1: {"kind": t, "n": 9}, 2: {"kind": t, "n": 10}})
    for n, v in ((0x1e, 0), (0x1f, 1), (0x20, 2)):
        case(f"ssum16 fmt {v}", b(sub={4: isel(n)}), "ssum16", {"fmt": v})
    for i, n, f in ((5, 0x1b, "x1b"), (6, 0x1c, "x1c"), (7, 0x1d, "x1d")):
        case(f"ssum16 {f}", b(sub={i: isel(n)}), "ssum16", {f: 1})
    case("ssum16 nf16", b(sub={0: R("r", 1, 0x10000)}), "ssum16", {"nf16": 0})
    case("ssum16 simm", b(sub={3: R("#", -5), 4: isel(0x20)}), "ssum16", ops={3: {"kind": "#", "n": -5}})
    base = [r(1), R("i", 0), r(3), isel(0x15), isel(0x15), isel(0x15)]
    b = builder(0x6a, base)
    flags_cases("ssum16i", b, "ssum16i", patch=True)
    operand_cases("ssum16i", b, "ssum16i", {0: (0, DKINDS), 2: (2, KINDS)}, patch=True)
    for n in range(3):
        case(f"ssum16i i{n}", b(sub={1: R("i", n)}), "ssum16i", ops={1: {"kind": "i", "n": n}}, patch=True)
    for i, n, f in ((3, 0x1b, "x1b"), (4, 0x1c, "x1c"), (5, 0x1d, "x1d")):
        case(f"ssum16i {f}", b(sub={i: isel(n)}), "ssum16i", {f: 1}, patch=True)
    case("ssum16i nf16", b(sub={0: R("r", 1, 0x10000)}), "ssum16i", {"nf16": 0}, patch=True)

    # ---- firv / firh
    for op, name, patch, rmax, d0 in ((0x6c, "firv", False, 4, r(1)), (0x6d, "firh", True, 6, R("i", 1))):
        base = [d0, r(2), r(3), r(4), isel(0x1e), R("#", 0), R(0x11, 0), R("#", 0), R("#", 0)]
        b = builder(op, base)
        flags_cases(name, b, name, patch=patch, rmax=rmax)
        if name == "firv":
            operand_cases(name, b, name, {0: (0, DKINDS), 3: (3, KINDS)})
        else:
            operand_cases(name, b, name, {3: (3, KINDS)}, patch=True)
            for n in (0, 3, 100):
                case(f"firh dst i{n}", b(sub={0: R("i", n)}), name, ops={0: {"kind": "i", "n": n}}, patch=True)
            case("firh f2_27", b(f2=1 << 27), name, {"f2_27": 1}, patch=True)
        for t in ("r", "o", "pa", "sa"):
            case(f"{name} bank {t}", b(sub={1: R(t, 9), 2: R(t, 10)}), name,
                 ops={1: {"kind": t, "n": 9}, 2: {"kind": t, "n": 10}}, patch=patch)
        for n, v in ((0x1e, 0), (0x1f, 1), (0x20, 2)):
            case(f"{name} fmt {v}", b(sub={4: isel(n)}), name, {"fmt": v}, patch=patch)
        for n in range(3):
            case(f"{name} a5 {n}", b(sub={5: R("#", n)}), name, {"a5": n}, patch=patch)
        for n in range(8):
            case(f"{name} coef {n}", b(sub={6: R(0x11, n)}), name, {"coef": n}, patch=patch)
        for n in range(-16, 16):
            case(f"{name} shift {n}", b(sub={7: R("#", n)}), name, {"shift": n & 31}, patch=patch)
        for n in range(4):
            case(f"{name} a8 {n}", b(sub={8: R("#", n)}), name, {"a8": n}, patch=patch)
        case(f"{name} simm", b(sub={3: R("#", -7), 4: isel(0x1f)}), name, ops={3: {"kind": "#", "n": -7}}, patch=patch)

    # ---- ima32 (and the 0x167/0x168 rewrites)
    base = [r(1), R("i", 0), r(2), r(3), r(4), isel(0x15)]
    b = builder(0x65, base)
    flags_cases("ima32", lambda **k: b(f2=0x800, **k), "ima32")
    operand_cases("ima32", lambda **k: b(f2=0x800, **k), "ima32", {0: (0, DKINDS), 2: (3, KINDS), 3: (4, KINDS)})
    for t, n in (("r", 9), ("o", 9), ("pa", 9), ("sa", 9)):
        case(f"ima32 src0 {t}", b(f2=0x800, sub={2: R(t, n)}), "ima32", ops={1: {"kind": t, "n": n}})
    case("ima32 signed", b(f2=0x1000), "ima32", {"sgn": 1})
    case("ima32 unsigned", b(f2=0x800), "ima32", {"sgn": 0})
    for n, v in ((0, 4), (1, 5), (2, 6)):
        case(f"ima32 hi i{n}", b(f2=0x800, sub={1: R("i", n)}), "ima32", {"hi": v})
    case("ima32 hi none", b(f2=0x800, sub={1: isel(0x15)}), "ima32", {"hi": 0})
    for n in range(3):
        case(f"ima32 cin i{n}", U(0x65, *base[:5], isel(0x2a), R("i", n), f1=0x200, f2=0x800), "ima32",
             {"cin": n + 1})
    case("ima32 neg src0", b(f2=0x800, sub={2: R("r", 2, 1)}), "ima32", {"negp": 1})
    case("ima32 neg src1", b(f2=0x800, sub={3: R("r", 3, 1)}), "ima32", {"negp": 1})
    case("ima32 neg both", b(f2=0x800, sub={2: R("r", 2, 1), 3: R("r", 3, 1)}), "ima32", {"negp": 0})
    case("ima32 neg src2", b(f2=0x800, sub={4: R("r", 4, 1)}), "ima32", ops={3: {"neg": 1}})
    for imm in (-64, -1, 63):
        case(f"ima32 simm {imm}", b(f2=0x1000, sub={3: R("#", imm), 4: R("#", imm)}), "ima32",
             ops={2: {"kind": "#", "n": imm}, 3: {"kind": "#", "n": imm}})
    case("ima32 uimm 127", b(f2=0x800, sub={3: R("#", 127)}), "ima32", ops={2: {"kind": "#", "n": 127}})
    case("isub32 0x167", U(0x167, r(1), r(2), r(3), f1=0x200), "ima32",
         {"sgn": 1, "hi": 0}, {1: {"kind": "r", "n": 2}, 2: {"kind": "#", "n": 1}, 3: {"kind": "r", "n": 3, "neg": 1}})
    case("isub32 0x168", U(0x168, r(1), r(2), r(3), f1=0x200), "ima32",
         {"sgn": 0, "hi": 0}, {1: {"kind": "r", "n": 2}, 2: {"kind": "#", "n": 1}, 3: {"kind": "r", "n": 3, "neg": 1}})

    # ---- op1adrc (0x66, patched)
    b = builder(0x66, [r(1), r(2), r(3), R(10, 0)])
    flags_cases("op1adrc", b, "op1adrc", patch=True)
    operand_cases("op1adrc", b, "op1adrc", {1: (1, KINDS), 2: (2, KINDS)}, patch=True)
    for t, n in (("r", 0), ("r", 127), ("pa", 9)):
        case(f"op1adrc dst {t}{n}", b(sub={0: R(t, n)}), "op1adrc", ops={0: {"kind": t, "n": n}}, patch=True)
    case("op1adrc drc1", b(sub={3: R(10, 1)}), "op1adrc", {"drc": 1}, patch=True)
    return len(FAIL)


if __name__ == "__main__":
    n = run()
    for x in FAIL:
        print(x)
    print(f"check_int: {N[0]} cases, {n} failure(s)")
    sys.exit(1 if n else 0)
