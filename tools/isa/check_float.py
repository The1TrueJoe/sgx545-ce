"""Oracle sweep for hw_float.py and hw_move.py: encode USEASM instructions
with libusc's own assembler (oracle.py), decode them with hw.py, and check
that (1) every bit is explained, (2) text -> parse -> encode gives the same
64 bits, (3) the decoded operands and flags are the ones put in.

  cd <scratchpad> && LIBUSC=<libusc.so> <venv>/bin/python sgx/tools/isa/check_float.py
"""
import itertools
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw  # noqa: E402
from uinst import A, U, try_enc  # noqa: E402

fails = []
n_ok = 0
PRED_IN = {0: "", 1: "p0", 2: "p1", 3: "!p0", 4: "!p1", 5: "p2", 6: "p3", 7: "pn"}


def check(tag, inst, want_ops=None, want=None, pair=None):
    """want_ops: [(kind, n)] for the decoded operands; want: {field: value}"""
    global n_ok
    r = try_enc(inst, pair=pair)
    if r is None:
        return None
    lo, hi = r
    x = hw.decode(lo, hi)
    if x is None:
        fails.append(f"{tag}: {hi:08x} {lo:08x} no form")
        return r
    if x.unknown:
        fails.append(f"{tag}: {hi:08x} {lo:08x} {hw.fmt(x)} unknown {x.unknown}")
    t = hw.fmt(x)
    try:
        lo2, hi2 = hw.encode(hw.parse(t))
        if (lo2, hi2) != (lo, hi):
            fails.append(f"{tag}: {hi:08x} {lo:08x} {t} re-encodes as {hi2:08x} {lo2:08x}")
    except Exception as e:
        fails.append(f"{tag}: {hi:08x} {lo:08x} {t}: {e}")
    if want_ops:
        got = [(o["kind"], o.get("n", o.get("off"))) for o in x.ops]
        for i, w in enumerate(want_ops):
            if w is not None and (i >= len(got) or got[i] != w):
                fails.append(f"{tag}: operand {i} is {got[i] if i < len(got) else None}, want {w} ({t})")
    for k, v in (want or {}).items():
        if x.d.get(k) != v:
            fails.append(f"{tag}: {k}={x.d.get(k)} want {v} ({t})")
    n_ok += 1
    return r


DT = ["r", "o", "pa", "sa", "g", "i"]
ST = ["r", "o", "pa", "sa", "c", "#", "i"]


def kind(t, n):
    return ("c", n) if t == "c" else (t, n)


# --- three-source float ops, every operand type in every slot
for op, name in ((0, "fmad"), (1, "fadm"), (2, "fmsa")):
    for dt in DT:
        check(f"{name} dst {dt}", U(op, A(dt, 5), A("r", 1), A("r", 2), A("r", 3), f1=0x200),
              [kind(dt, 5)], {"rpt": 1, "moe": 1})
    for t0 in ("r", "pa", "i"):
        want = ("r", 124 + 2) if t0 == "i" else (t0, 2)
        check(f"{name} s0 {t0}", U(op, A("r", 1), A(t0, 2), A("r", 3), A("r", 4), f1=0x200), [None, want])
    for t1, t2 in itertools.product(ST, ST):
        check(f"{name} s1 {t1} s2 {t2}", U(op, A("r", 1), A("pa", 2), A(t1, 3), A(t2, 4), f1=0x200),
              [None, None, kind(t1, 3), kind(t2, 4)])
    for ix in (1, 2):
        for b in ("r", "o", "pa", "sa"):
            r = check(f"{name} s2 {b}[idx{ix}+5]", U(op, A("r", 1), A("pa", 2), A("r", 3), A(b, 5, index=ix), f1=0x200))
            if r:
                x = hw.decode(*r)
                o = x.ops[3]
                if (o["kind"], o.get("bank"), o.get("idx"), o.get("off")) != ("ix", b, ix, 5):
                    fails.append(f"{name} index: got {o}")
    check(f"{name} g9", U(op, A("r", 1), A("pa", 2), A("g", 9), A("r", 3), f1=0x200), [None, None, ("g", 9)])
    # modifiers: argument flags 0 (negate) and 1 (abs) on each source
    for f0, f1_, f2_ in itertools.product(range(4), repeat=3):
        check(f"{name} mods {f0}{f1_}{f2_}", U(op, A("r", 1), A("pa", 2, flags=f0), A("r", 3, flags=f1_),
                                               A("r", 4, flags=f2_), f1=0x200))
    # format select: argument flag 22 on any source
    for sel in range(1, 8):
        check(f"{name} f16 {sel}", U(op, A("r", 1), A("pa", 2, flags=(sel & 1) << 22), A("r", 3, flags=(sel >> 1 & 1) << 22),
                                    A("r", 4, flags=(sel >> 2 & 1) << 22), f1=0x200), want={"fmt": 1})
    # repeat counts, masks, increments, predicates and flags
    for rpt in range(1, 5):
        check(f"{name} rpt {rpt}", U(op, A("r", 1), A("pa", 2), A("r", 3), A("r", 4), f1=rpt << 4),
              want={"rpt": rpt, "moe": 1})
        for inc in range(8):
            check(f"{name} rpt {rpt} inc {inc}", U(op, A("r", 1), A("pa", 2), A("r", 3), A("r", 4), f1=rpt << 4,
                                                   f2=0x10000000 | inc << 29),
                  want={"rpt": rpt, "moe": 0, "inc0": inc & 1, "inc1": inc >> 1 & 1, "inc2": inc >> 2})
    for m, rpt in ((1, 1), (3, 2), (7, 3), (0xF, 4)):
        check(f"{name} mask {m}", U(op, A("r", 1), A("pa", 2), A("r", 3), A("r", 4), f1=m << 9), want={"rpt": rpt})
    for p in range(8):
        r = check(f"{name} pred {p}", U(op, A("r", 1), A("pa", 2), A("r", 3), A("r", 4), f1=0x200 | p << 13))
        if r and hw.PRED3[hw.decode(*r).d["pred"]] != PRED_IN[p]:
            fails.append(f"{name} pred {p}: decoded {hw.PRED3[hw.decode(*r).d['pred']]}")
    for bit, fld in ((0, "skipinv"), (2, "syncs"), (3, "nosched"), (17, "end")):
        check(f"{name} f1.{bit}", U(op, A("r", 1), A("pa", 2), A("r", 3), A("r", 4), f1=0x200 | 1 << bit), want={fld: 1})

# --- scalar functions
for op in range(4, 11):
    name = hw_name = ["frcp", "frsq", "flog2", "fexp2", "fsqrt", "fsin", "fcos"][op - 4]
    for dt in DT:
        for st in ST:
            check(f"{name} {dt} <- {st}", U(op, A(dt, 5), A(st, 3), f1=0x200), [kind(dt, 5), kind(st, 3)])
    for fl in range(4):
        check(f"{name} mods {fl}", U(op, A("r", 1), A("r", 3, flags=fl), f1=0x200))
    for ch in range(4):
        check(f"{name} c10 chan {ch}", U(op, A("r", 1), A("r", 3, flags=1 << 24 | ch << 5), f1=0x200),
              want={"c10": 1, "chan": ch})
    for ch in (0, 2):
        check(f"{name} f16 chan {ch}", U(op, A("r", 1), A("r", 3, flags=1 << 22 | ch << 5), f1=0x200),
              want={"f16": 1, "chan": ch // 2})
    for m in range(16):
        check(f"{name} mask {m}", U(op, A("r", 1), A("r", 3), f1=m << 9), want={"rcnt": 0, "mask": m})
    for rpt in range(1, 17):
        check(f"{name} rpt {rpt}", U(op, A("r", 1), A("r", 3), f1=rpt << 4), want={"rcnt": 1 if rpt > 0 else 0})
    check(f"{name} f2.22", U(op, A("r", 1), A("r", 3), f1=0x200, f2=1 << 22), want={"f16d": 1})
    for bit, fld in ((0, "skipinv"), (2, "syncs"), (3, "nosched"), (17, "end")):
        check(f"{name} f1.{bit}", U(op, A("r", 1), A("r", 3), f1=0x200 | 1 << bit), want={fld: 1})

# --- two-source and min/max
for op in (0xB, 0xE, 0xF, 0xCF, 0xC, 0xD):
    for t1, t2 in itertools.product(ST, ST):
        check(f"op{op:#x} {t1} {t2}", U(op, A("r", 1), A(t1, 2), A(t2, 3), f1=0x200))
    for f1_, f2_ in itertools.product(range(4), repeat=2):
        check(f"op{op:#x} mods", U(op, A("r", 1), A("r", 2, flags=f1_), A("r", 3, flags=f2_), f1=0x200))
    for rpt in range(1, 5):
        check(f"op{op:#x} rpt {rpt}", U(op, A("r", 1), A("r", 2), A("r", 3), f1=rpt << 4), want={"rpt": rpt})
for op in (0x11, 0x12):
    for f in itertools.product(range(4), repeat=3):
        check(f"op{op:#x} mods {f}", U(op, A("r", 1), A("pa", 2, flags=f[0]), A("r", 3, flags=f[1]),
                                      A("r", 4, flags=f[2]), f1=0x200))

# --- fmad16: swizzles, increments, repeat
for s0, s1, s2 in itertools.product(range(4), repeat=3):
    check(f"fmad16 swz {s0}{s1}{s2}", U(0x10, A("r", 1), A("pa", 2, flags=s0 << 25), A("r", 3, flags=s1 << 25),
                                        A("r", 4, flags=s2 << 25), f1=0x200),
          want={"swz0": s0, "swz1": s1, "swz2": s2})
for f in itertools.product(range(4), repeat=3):
    check(f"fmad16 mods {f}", U(0x10, A("r", 1), A("pa", 2, flags=f[0]), A("r", 3, flags=f[1]), A("r", 4, flags=f[2]),
                                f1=0x200))
for inc in range(8):
    check(f"fmad16 rpt2 inc {inc}", U(0x10, A("r", 1), A("pa", 2), A("r", 3), A("r", 4), f1=2 << 4,
                                      f2=0x10000000 | inc << 29), want={"rpt": 2})
for t1, t2 in itertools.product(ST, ST):
    check(f"fmad16 {t1} {t2}", U(0x10, A("r", 1), A("pa", 2), A(t1, 3), A(t2, 4), f1=0x200))

# --- mov / movc
for dt in DT + ["idx"]:
    for st in ST:
        check(f"mov {dt} <- {st}", U(0xA3, A(dt if dt != "idx" else 4, 1 if dt == "idx" else 5), A(st, 3), f1=0x200))
for m in range(16):
    check(f"mov mask {m}", U(0xA3, A("r", 1), A("pa", 3), f1=m << 9), want={"rcnt": 0, "mask": m})
for rpt in range(1, 17):
    check(f"mov rpt {rpt}", U(0xA3, A("r", 1), A("pa", 3), f1=rpt << 4), want={"rcnt": 1, "rpt": rpt})
for p in range(8):
    check(f"mov pred {p}", U(0xA3, A("r", 1), A("pa", 3), f1=0x200 | p << 13))
TYPES = ((2, 0, 1), (0, 1 << 19, 2), (0, 1 << 20, 3), (0, 1 << 21, 4), (0, 1 << 23, 5))
for f1x, f2, ty in TYPES:
    for test, cond in ((0x14, 0), (0x18, 1), (0x19, 2), (0x6, 3)):
        check(f"movc type {ty} test {test:#x}", U(0xF2, A("r", 1), A("pa", 2), A("r", 3), A("sa", 4),
                                                  f1=0x40000200 | f1x, f2=f2, test=test), want={"cond": cond})
    for st in ST:
        check(f"movc {ty} srcs {st}", U(0xF2, A("o", 1), A("r", 2), A(st, 3), A(st, 4), f1=0x40000200 | f1x, f2=f2,
                                        test=0x14))
# USEASM 0xf3
for sub_flag in (0, 1 << 19):
    a1 = A("r", 2) if sub_flag else A(0x10, 0x15)
    for f1x, f2 in ((2, 0), (1 << 26, 0), (0, 1 << 21), (1 << 21, 0)):
        for a3f in range(4):
            check(f"op0xf3 {sub_flag:#x} {f1x:#x} {f2:#x} {a3f}",
                  U(0xF3, A("o", 1), a1, A("r", 3), A("pa", 4, flags=a3f << 5), f1=0x200 | f1x, f2=f2, f3=sub_flag))

# --- fdpc / fddp / fddpc
for cp in range(8):
    check(f"fdpc cp{cp}", U(0xC4, A("r", 1), A(0xD, cp), A("pa", 2), A("r", 3), f1=0x40))
for i in (0, 1):
    check(f"fddp i{i}", U(0xC7, A("o", 1), A("i", i), A("r", 2), A("pa", 3), A("r", 4), f1=0x200))
    for c0, c1 in ((0, 7), (5, 2)):
        check(f"fddpc i{i} {c0} {c1}", U(0xCA, A("r", 1), A("i", i), A(0xD, c0), A(0xD, c1), A("r", 2), A("r", 3),
                                         A("r", 4), f1=0x30))
for f in itertools.product(range(4), repeat=3):
    check(f"fddp mods {f}", U(0xC7, A("r", 1), A("i", 0), A("r", 2, flags=f[0]), A("r", 3, flags=f[1]),
                              A("r", 4, flags=f[2]), f1=0x200))

# --- fdual: every primary with every secondary, secondary operands routed
# through i0-i2 or shared with the primary's sources
D = 1 << 25
PRI_ARGS = {0x0: [A("r", 1), A("pa", 2), A("r", 3), A("r", 4)], 0x1: [A("r", 1), A("pa", 2), A("r", 3), A("r", 4)],
            0x2: [A("r", 1), A("pa", 2), A("r", 3), A("r", 4)], 0x11: [A("r", 1), A("pa", 2), A("r", 3), A("r", 4)],
            0xA3: [A("r", 1), A("pa", 2)], 0xA7: [A("o", 1), A("pa", 2), A("r", 3)],
            0xC7: [A("r", 1), A("i", 0), A("pa", 2), A("r", 3), A("r", 4)]}
PRI_CODE = {0x0: 0, 0xA3: 1, 0x1: 2, 0x2: 3, 0xC7: 4, 0x11: 5, 0xA7: 7}
SEC_CODE = {0x0: 0, 0x10: 1, 0x2: 2, 0x1: 3, 0x3: 3, 0xCF: 3, 0x12: 4, 0x4: 5, 0x5: 6, 0x7: 7, 0x6: 8, 0x9: 9,
            0xA: 10, 0xA6: 11, 0xA9: 12, 0xA5: 13, 0xA3: 15}
NSRC = {0x0: 3, 0x10: 3, 0x2: 3, 0x12: 3, 0x1: 2, 0x3: 1, 0xCF: 2, 0xA6: 2, 0xA9: 2, 0xA5: 2}
for po, pargs in PRI_ARGS.items():
    for so, code in SEC_CODE.items():
        n = NSRC.get(so, 1)
        for d in range(3):
            for srcs in itertools.product(range(3), repeat=n):
                sec = U(so, A("i", d), *[A("i", k) for k in srcs])
                check(f"fdual {po:#x}+{so:#x} i{d} {srcs}", U(po, *pargs, f1=0x200 | D), pair=sec,
                      want={"pri": PRI_CODE[po], "sec": code})

# mov secondary: one source, i0-i2 or a primary slot (int-source-select 4/5/6
# = src0/src1/src2, or a register put into a slot the primary leaves free)
MSRC_IN = [(A("i", 0), 0), (A("i", 1), 1), (A("i", 2), 2), (A(0x10, 4), 4), (A(0x10, 5), 5), (A(0x10, 6), 6),
           (A("pa", 9), None)]
for po, pargs in PRI_ARGS.items():
    for d in range(3):
        for a, code in MSRC_IN:
            r = check(f"fdual {po:#x}+mov i{d} <- {a[:2]}", U(po, *pargs, f1=0x200 | D), pair=U(0xA3, A("i", d), a),
                      want={"pri": PRI_CODE[po], "sec": 15})
            if r and code is not None and hw.decode(*r).d.get("msrc") != code:
                fails.append(f"fdual {po:#x}+mov <- {a[:2]}: msrc {hw.decode(*r).d.get('msrc')} want {code}")
            if r and code is None and hw.decode(*r).d.get("msrc") not in (4, 6):
                fails.append(f"fdual {po:#x}+mov <- pa9: msrc {hw.decode(*r).d.get('msrc')} want a free slot")

print(f"{n_ok} encodings checked, {len(fails)} failures")
for f in fails[:40]:
    print("  " + f)
sys.exit(1 if fails or n_ok < 1000 else 0)
