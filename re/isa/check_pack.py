"""Oracle sweep for hw_pack.py: pack/unpack (hw 08), test (09), testmask (0f),
bitwise (0a-0e). Encodes USEASM instructions with libusc's own assembler
(oracle.py), decodes them with hw.py, checks the decoded fields against the
inputs, that no bit is unexplained, and that decode -> text -> parse -> encode
gives the same 64 bits.

  cd $S && LIBUSC=$S/bin/libusc.so $S/venv/bin/python sgx/re/isa/check_pack.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uinst import U, A, try_enc, hexpair  # noqa: E402
import hw  # noqa: E402
from hw_pack import PCKFMT, ALU, COND  # noqa: E402

N = [0]
FAIL = []
T = 1 << 30
# USEASM predicate (f1 b13-16) -> hardware PRED3 code
UPRED = {0: 0, 1: 1, 2: 2, 3: 5, 4: 6, 5: 3, 6: 4, 7: 7}


def chk(what, u, name=None, mn=None, f=None, ops=None):
    N[0] += 1
    r = try_enc(u)
    if r is None:
        FAIL.append(f"{what}: encoder rejected")
        return None
    x = hw.decode(*r)
    if x is None:
        FAIL.append(f"{what}: {hexpair(*r)} no form")
        return None
    err = []
    if x.unknown:
        err.append(f"unknown bits {x.unknown}")
    if name and x.form.name != name:
        err.append(f"form {x.form.name} != {name}")
    if mn is not None and (x.form.mn(x.d) if x.form.mn else []) != mn:
        err.append(f"mn {x.form.mn(x.d)} != {mn}")
    for k, v in (f or {}).items():
        if x.d.get(k) != v:
            err.append(f"{k}={x.d.get(k)} want {v}")
    for i, o in (ops or {}).items():
        for k, v in o.items():
            if x.ops[i].get(k) != v:
                err.append(f"op{i}.{k}={x.ops[i].get(k)} want {v}")
    t = hw.fmt(x)
    try:
        back = hw.encode(hw.parse(t))
        if back != r:
            err.append(f"round trip {t!r} -> {hexpair(*back)}")
    except Exception as e:
        err.append(f"parse {t!r}: {e}")
    if err:
        FAIL.append(f"{what}: {hexpair(*r)} {t}: " + "; ".join(err))
    return x


# --------------------------------------------------------------------------
# pack: USEASM op -> (dst fmt, src fmt), from FUN_001165c0's local_34 table
F = {n: i for i, n in enumerate(PCKFMT)}
PCK = {}
for i, d in enumerate(["f16", "u16", "s16", "u8", "s8", "o8", "c10", "f32"]):
    for j, s in enumerate(["f32", "f16", "u16", "s16"]):
        PCK[0x14 + 4 * i + j] = (d, s)
for i, s in enumerate(["u8", "s8", "o8", "c10"]):
    PCK[0x34 + i] = ("f32", s)
for base, d in ((0x38, "f16"), (0x3f, "u16"), (0x46, "s16")):
    for i, s in enumerate(["f16", "u16", "s16", "u8", "s8", "o8", "c10"]):
        PCK[base + i] = (d, s)
PCK.update({0x4d: ("u8", "u8"), 0x4e: ("c10", "c10"), 0x16f: ("u8", "s8"), 0x170: ("s8", "u8")})
FLOAT = ("f16", "f32")
RNDU = {0x3a: 0, 0x39: 1, 0x3b: 2, 0: 3}


def pck(op, a0=None, a1=None, a2=None, rnd=0x3a, f1=0x200, f2=0, dmask=None):
    d, s = PCK[op]
    ff = d in FLOAT and s in FLOAT
    if dmask is None:
        dmask = 0xF
    args = [a0 or A("r", 1, dmask << 7), a1 or A("r", 2), a2 or A("r", 3)]
    if ff or f2 & 0x100:
        args.append(A(0x10, rnd))
    return U(op, *args, f1=f1, f2=f2)


def check_pack():
    for op, (d, s) in sorted(PCK.items()):
        chk(f"pck {op:#x}", pck(op), "pck", [d, s])
        ff = d in FLOAT and s in FLOAT
        conv = (d in FLOAT) != (s in FLOAT)
        if ff:
            for ur, hr in RNDU.items():
                chk(f"pck {op:#x} rnd {ur:#x}", pck(op, rnd=ur), "pck", [d, s], {"rnd": hr, "rint": 0})
            chk(f"pck {op:#x} rint", pck(op, f1=0x200 | 1 << 24, rnd=0), "pck", [d, s], {"rint": 1, "rnd": 3})
        if conv:
            for ur, hr in RNDU.items():
                chk(f"pck {op:#x} scale rnd {ur:#x}", pck(op, f2=0x100, rnd=ur), "pck", [d, s], {"scale": 1, "rnd": hr})
        # lanes
        for ln in range(4):
            chk(f"pck {op:#x} lane1 {ln}", pck(op, a1=A("r", 2, ln << 5)), ops={1: {"sfx": [f"l{ln}"]} if ln else {"n": 2}})
            chk(f"pck {op:#x} lane2 {ln}", pck(op, a2=A("pa", 3, ln << 5)), ops={2: {"sfx": [f"l{ln}"]} if ln else {"n": 3}})
        # destination byte masks the encoder accepts for this destination format
        fi = F[d]
        masks = range(16) if fi < 3 or fi == 7 else ((3, 0xC, 0xF) if fi < 6 else (0xF,))
        for m in masks:
            chk(f"pck {op:#x} dmask {m}", pck(op, dmask=m), f={"dmask": m})
    op = 0x20
    for p in range(8):
        chk(f"pck pred {p}", pck(op, f1=0x200 | p << 13), f={"pred": UPRED[p]})
    for b, k in ((0, "skipinv"), (2, "syncs"), (3, "nosched"), (17, "end")):
        chk(f"pck {k}", pck(op, f1=0x200 | 1 << b), f={k: 1})
    for m in range(16):
        chk(f"pck mask {m}", pck(op, f1=m << 9), f={"rcnt": 0, "mask": m})
    for n in range(1, 17):
        chk(f"pck rpt {n}", pck(op, f1=n << 4), f={"rcnt": 1, "rpt": n})
    for t, n in (("r", 5), ("o", 6), ("pa", 7), ("sa", 8), ("g", 9)):
        chk(f"pck dst {t}", pck(op, a0=A(t, n, 0x780)), ops={0: {"kind": t, "n": n}})
    chk("pck dst i (c10)", pck(0x4e, a0=A("i", 1, 0x180)), ops={0: {"kind": "i", "n": 1}})
    chk("pck dst ix", pck(op, a0=A("r", 3, 0x780, 2)), ops={0: {"kind": "ix", "idx": 2, "off": 3}})
    for t, n in (("r", 5), ("o", 6), ("pa", 7), ("sa", 8), ("c", 9), ("g", 10), ("#", 11), ("i", 2)):
        chk(f"pck src1 {t}", pck(op, a1=A(t, n)), ops={1: {"kind": t, "n": n}})
        chk(f"pck src2 {t}", pck(op, a2=A(t, n)), ops={2: {"kind": t, "n": n}})
    chk("pck src1 ix", pck(op, a1=A("pa", 3, 0, 1)), ops={1: {"kind": "ix", "idx": 1, "off": 3}})
    chk("pck src2 ix", pck(op, a2=A("sa", 4, 0, 2)), ops={2: {"kind": "ix", "idx": 2, "off": 4}})


# --------------------------------------------------------------------------
# test / testmask: USEASM op -> ALU op (l14-19), from FUN_00112770
TALU = {0xa4: 0x00, 0xa6: 0x00, 0x3: 0x03, 0xcf: 0x03, 0x4: 0x04, 0x5: 0x05, 0x6: 0x06, 0x7: 0x07,
        0xb: 0x08, 0xc2: 0x08, 0xc3: 0x08, 0xc: 0x09, 0xd: 0x0a, 0xe: 0x0b, 0xf: 0x0c, 0xa5: 0x0d,
        0xa9: 0x0e,
        0xe4: 0x16, 0xe5: 0x17, 0xe6: 0x18, 0xe7: 0x19, 0xe8: 0x1a, 0xe9: 0x1b, 0xea: 0x1c, 0xf0: 0x1d,
        0xeb: 0x20, 0xec: 0x21, 0xf5: 0x22, 0xf6: 0x23, 0xed: 0x24, 0xf7: 0x25, 0xee: 0x26, 0xf8: 0x27,
        0xf9: 0x28,
        0x4f: 0x30, 0x50: 0x31, 0x51: 0x32, 0x52: 0x33, 0x53: 0x34, 0x55: 0x35, 0x54: 0x37}
NODST = A("c", 0, 0x10000)


def tst(op=0xa9, test=0x19, f1=T | 0x200, f2=0, a0=None, p=0, s1=None, s2=None):
    return U(op, a0 or NODST, A(0xc, p), s1 or A("r", 8), s2 or A("r", 9), f1=f1, f2=f2, test=test)


def tsm(op=0xa9, test=0x619, f1=T | 0x200, f2=0, a0=None, s1=None, s2=None):
    return U(op, a0 or A("r", 5), s1 or A("r", 8), s2 or A("r", 9), f1=f1, f2=f2, test=test)


def check_test():
    for op, alu in TALU.items():
        mask = {0xc2: 7, 0xc3: 15}.get(op, 1)
        chk(f"test op {op:#x}", tst(op), "test", f={"alu": alu, "mask": mask})
        chk(f"testmask op {op:#x}", tsm(op), "testmask", f={"alu": alu, "mask": mask, "type": 2})
    for c in range(32):
        chk(f"test cond {c:#x}", tst(test=c), f={"cond": c})
        chk(f"testmask cond {c:#x}", tsm(test=0x600 | c), f={"cond": c})
    for ch in range(8):
        chk(f"test chan {ch}", tst(test=0x19 | ch << 5), f={"chan": ch})
    for p in range(4):
        chk(f"test pdst p{p}", tst(p=p), ops={0: {"kind": "p", "n": p}})
    for m in range(1, 16):
        chk(f"test mask {m}", tst(f1=T | m << 9), f={"mask": m})
        chk(f"testmask mask {m}", tsm(f1=T | m << 9), f={"mask": m})
    for n in range(1, 5):
        chk(f"testmask rpt {n}", tsm(f1=T | n << 4), f={"mask": (1 << n) - 1})
    for ty, hv in ((1, 0), (2, 1), (3, 2)):
        chk(f"testmask type {ty}", tsm(test=0x19 | ty << 9), f={"type": hv})
    for p in range(8):
        chk(f"test pred {p}", tst(f1=T | 0x200 | p << 13), f={"pred": UPRED[p]})
        chk(f"testmask pred {p}", tsm(f1=T | 0x200 | p << 13), f={"pred": UPRED[p]})
    for b, k in ((0, "skipinv"), (2, "syncs"), (3, "nosched")):
        chk(f"test {k}", tst(f1=T | 0x200 | 1 << b), f={k: 1})
        chk(f"testmask {k}", tsm(f1=T | 0x200 | 1 << b), f={k: 1})
    chk("test f2_0", tst(f2=1), f={"f2_0": 1})
    chk("testmask f2_0", tsm(f2=1), f={"f2_0": 1})
    # destinations
    chk("test nodst", tst(), "test", f={"nodst": 0x280})
    chk("testmask nodst", tsm(a0=A("r", 5, 0x10000)), "testmask", f={"nodst": 0x280})
    for t, n in (("r", 5), ("o", 6), ("pa", 7), ("sa", 8), ("g", 9), ("i", 1)):
        chk(f"test dst {t}", tst(a0=A(t, n)), "test", ops={1: {"kind": t, "n": n}})
        chk(f"testmask dst {t}", tsm(a0=A(t, n)), "testmask", ops={0: {"kind": t, "n": n}})
    chk("testmask dst ix", tsm(a0=A("r", 1, 0, 1)), ops={0: {"kind": "ix", "idx": 1, "off": 1}})
    # sources
    for t, n in (("r", 5), ("o", 6), ("pa", 7), ("sa", 8), ("c", 9), ("g", 10), ("#", 11), ("i", 2)):
        chk(f"test src1 {t}", tst(s1=A(t, n)), ops={1: {"kind": t, "n": n}})
        chk(f"test src2 {t}", tst(s2=A(t, n)), ops={2: {"kind": t, "n": n}})
    chk("test src2 ix", tst(s2=A("pa", 2, 0, 2)), ops={2: {"kind": "ix", "idx": 2, "off": 2}})
    # format: F16 sources/dest on the float group, C10 on group 2 (flag b24).
    # An F16 operand's number counts halves (hardware, hwt_p1): r8 = r4.lo, r9 = r4.hi
    chk("test f16 src1", tst(s1=A("r", 8, 0x400000)), f={"fmt": 1}, ops={1: {"n": 4, "half": 0}})
    chk("test f16 src2", tst(s2=A("r", 9, 0x400000)), f={"fmt": 1}, ops={2: {"n": 4, "half": 1}})
    # the dest flag alone does not select the format; a source flag does
    chk("test f16 dst", tst(a0=A("r", 5, 0x400000), s1=A("r", 8, 0x400000)), f={"fmt": 1}, ops={1: {"n": 2, "half": 1}})
    chk("test f16 dst only", tst(a0=A("r", 5, 0x400000)), f={"fmt": 0}, ops={1: {"n": 5}})
    chk("testmask f16", tsm(a0=A("r", 5, 0x400000), s1=A("r", 8, 0x400000)), f={"fmt": 1},
        ops={0: {"n": 2, "half": 1}, 1: {"n": 4, "half": 0}})
    for op in (0xf7, 0xf8, 0xf9):
        chk(f"test c10 {op:#x}", tst(op, s1=A("pa", 4, 0x1000000), s2=A("pa", 5, 0x1000000)), f={"fmt": 1},
            ops={1: {"sfx": ["c10"]}, 2: {"sfx": ["c10"]}})
    # USEASM compare pseudo-ops: the dispatcher rewrites them to a TEST fsub/fadd
    for op, c in ((0xaa, "gt"), (0xab, "ge"), (0xac, "eq"), (0xad, "lt"), (0xae, "le"), (0xaf, "ne")):
        chk(f"cmp {op:#x}", U(op, A(0xc, 1), A("r", 8), A("r", 9), f1=0x200), "test",
            f={"alu": 0x0e, "cond": [k for k, v in COND.items() if v == c][0]}, ops={0: {"n": 1}})
    for op, c in ((0xb8, 0x14), (0xb9, 0x18)):
        chk(f"cmp0 {op:#x}", U(op, A(0xc, 2), A("r", 8), f1=0x200), "test", f={"alu": 0, "cond": c},
            ops={2: {"kind": "#", "n": 0}})
    # bitwise ALU with an immediate (7-bit here, no rotate)
    chk("test and #", tst(0x4f, s2=A("#", 0x7f)), f={"alu": 0x30}, ops={2: {"kind": "#", "n": 0x7f}})


# --------------------------------------------------------------------------
# bitwise
BIT = {0x4f: ("and", 0xa), 0x50: ("or", 0xa), 0x51: ("xor", 0xb), 0x52: ("shl", 0xc), 0x55: ("rol", 0xc),
       0x53: ("shr", 0xd), 0x54: ("asr", 0xd)}


def bit(op=0x4f, f1=0x200, a0=None, s1=None, s2=None):
    return U(op, a0 or A("r", 5), s1 or A("r", 8), s2 or A("r", 9), f1=f1)


def rol(v, n):
    return (v << n | v >> (32 - n)) & 0xFFFFFFFF if n else v


def check_bit():
    for op, (nm, hwop) in BIT.items():
        x = chk(f"{nm}", bit(op), nm)
        if x and x.form.fval >> 59 != hwop:
            FAIL.append(f"{nm}: hw opcode {x.form.fval >> 59:#x} != {hwop:#x}")
        chk(f"{nm} inv", bit(op, s2=A("r", 9, 0x800)), nm, f={"inv": 1})
        chk(f"{nm} #", bit(op, s2=A("#", 0x1234)), nm, ops={2: {"kind": "#", "n": 0x1234}})
    for v in (0, 1, 0x7f, 0x80, 0x3fff, 0x4000, 0xffff, 0x10000, 0xff000000, 0x80000001, 0xfffffffe,
              0xffff0000, 0x0ff00000, 0xffffffff, 0x0001fffe, 0xc0000003):
        for partial in (0, 1):
            x = chk(f"and #{v:#x} partial={partial}", bit(s2=A("#", v), f1=0x200 | partial << 20), "and",
                    f={"partial": partial})
            if x:
                got = rol(x.ops[2]["n"], x.d["rot"]) ^ (0xFFFFFFFF if x.d["inv"] else 0)
                want = v & 0xFFFF if partial else v
                if got != want:
                    FAIL.append(f"and #{v:#x} partial={partial}: imm decodes to {got:#x}")
    chk("and # inv", bit(s2=A("#", 0xff, 0x800)), f={"inv": 1}, ops={2: {"n": 0xff}})
    for p in range(8):
        chk(f"and pred {p}", bit(f1=0x200 | p << 13), f={"pred": UPRED[p]})
    for b, k in ((0, "skipinv"), (2, "syncs"), (3, "nosched"), (17, "end"), (20, "partial")):
        chk(f"and {k}", bit(f1=0x200 | 1 << b), f={k: 1})
    for m in range(16):
        chk(f"and mask {m}", bit(f1=m << 9), f={"rcnt": 0, "mask": m})
    for n in range(1, 17):
        chk(f"and rpt {n}", bit(f1=n << 4), f={"rcnt": 1, "rpt": n})
    for t, n in (("r", 5), ("o", 6), ("pa", 7), ("sa", 8), ("g", 9), ("i", 1)):
        chk(f"and dst {t}", bit(a0=A(t, n)), ops={0: {"kind": t, "n": n}})
    chk("and dst ix", bit(a0=A("o", 2, 0, 1)), ops={0: {"kind": "ix", "bank": "o", "idx": 1, "off": 2}})
    for t, n in (("r", 5), ("o", 6), ("pa", 7), ("sa", 8), ("c", 9), ("g", 10), ("#", 11), ("i", 2)):
        chk(f"and src1 {t}", bit(s1=A(t, n)), ops={1: {"kind": t, "n": n}})
        chk(f"and src2 {t}", bit(s2=A(t, n)), ops={2: {"kind": t, "n": n}})
    chk("and src2 ix", bit(s2=A("r", 3, 0, 2)), ops={2: {"kind": "ix", "idx": 2, "off": 3}})
    # rlp
    for p in range(4):
        chk(f"rlp p{p}", U(0x56, A("r", 5), A("r", 8), A(0xc, p), f1=0x200), "rlp", ops={2: {"kind": "p", "n": p}})
    chk("rlp rpt", U(0x56, A("r", 5), A("pa", 8), A(0xc, 1), f1=3 << 4), "rlp", f={"rpt": 3})
    chk("rlp #", U(0x56, A("o", 5), A("#", 8), A(0xc, 1), f1=0x200 | 1 << 20), "rlp", f={"partial": 1})


def main():
    check_pack()
    check_test()
    check_bit()
    print(f"{N[0]} encodings checked, {len(FAIL)} failures")
    for x in FAIL[:60]:
        print("  ", x)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
