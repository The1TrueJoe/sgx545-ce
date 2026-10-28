"""hwt.py suite, priority 2: movc conditions and types, test + predicated mov, testmask, br."""
from hwt_lib import F, load, out

TESTS = []
S1, S2 = 0x11111111, 0x22222222
MASK1 = "[skipinv mask=1]"

# movc.<cond>.<type> r3, r0, r1, r2: r3 = test(r0) ? r1 : r2
VALS = {"f32": [0, 0x80000000, F(1), F(-1), 1, 0x7fc00000],
        "i32": [0, 0x80000000, 1, 0xffffffff],
        "u8": [0x00ff8001, 0x7f000180],
        "i16": [0x80010000, 0x00007fff],
        "c10": [0x00ff8001, 0x3ff00200]}
for ty, vals in VALS.items():
    for cond in ("z", "nz", "ltz", "gez"):
        for v in vals:
            TESTS.append((f"movc_{cond}_{ty}_{v:08x}", load({"r0": v, "r1": S1, "r2": S2})
                          + [f"movc.{cond}.{ty} r3, r0, r1, r2  {MASK1}", out("r3")], None))

# test.fsub.<cond> p0, a, b; (p0) mov r3, c52 (1.0); r3 otherwise keeps 0xaaaaaaaa
COND = {"lt": lambda a, b: a < b, "le": lambda a, b: a <= b, "eq": lambda a, b: a == b,
        "ne": lambda a, b: a != b, "gt": lambda a, b: a > b, "ge": lambda a, b: a >= b}
for c, fn in COND.items():
    for a, b in ((1, 2), (2, 2), (2, 1)):
        TESTS.append((f"test_{c}_{a}_{b}", load({"r0": F(a), "r1": F(b), "r3": 0xaaaaaaaa})
                      + [f"test.fsub.{c} p0, r0, r1  [skipinv]", f"(p0) mov r3, c52  {MASK1}", out("r3")],
                      F(1) if fn(a, b) else 0xaaaaaaaa))
# sign tests on fadd x + 0
for c, x, want in (("neg", -1.0, 1), ("neg", 1.0, 0), ("pos", 1.0, 1), ("pos", -1.0, 0), ("pos", 0.0, 1)):
    TESTS.append((f"test_{c}_{x:g}", load({"r0": F(x), "r3": 0xaaaaaaaa})
                  + [f"test.fadd.{c} p0, r0, c48  [skipinv]", f"(p0) mov r3, c52  {MASK1}", out("r3")],
                  F(1) if want else 0xaaaaaaaa))
# predicate registers and negation: lt(1, 2) is true
for p, pr, want in (("p1", "p1", 1), ("p2", "p2", 1), ("p3", "p3", 1), ("p0", "!p0", 0), ("p1", "!p1", 0)):
    TESTS.append((f"pred_{p}_{pr.replace('!', 'n')}", load({"r0": F(1), "r1": F(2), "r3": 0xaaaaaaaa})
                  + [f"test.fsub.lt {p}, r0, r1  [skipinv]", f"({pr}) mov r3, c52  {MASK1}", out("r3")],
                  F(1) if want else 0xaaaaaaaa))
# testmask: the mask word itself, then movc.nz.i32 as the compiler uses it
for c, a, b in (("lt", 1, 2), ("lt", 2, 1)):
    TESTS.append((f"testmask_{c}_{a}_{b}", load({"r0": F(a), "r1": F(b), "r3": 0x5a5a5a5a})
                  + [f"testmask.fsub.{c}.dword r3, r0, r1  [skipinv]", out("r3")], None))

# br: which instruction does `br #n` land on? landing pads read r4..r7 = 0x44.. 0x77
PADS = load({"r4": 0x44, "r5": 0x55, "r6": 0x66, "r7": 0x77})
for n in (1, 2, 3):
    TESTS.append((f"br_{n}", PADS + [f"br #{n}  [nosched]"] + [out(f"r{4 + i}") for i in range(4)],
                  [0x44, 0x55, 0x66][n - 1]))
# backward: r3 = 0; do r3 += 1 while (r3 < 8)
TESTS.append(("loop_br", load({"r3": 0}) + ["fmad r3, r3, c52, c52  [skipinv]", "test.fsub.lt p0, r3, c6  [skipinv]",
                                            "(p0) br #-2", out("r3")], F(8)))
# predicated, not taken / taken
for c, want in (("lt", 0x55), ("ge", 0x66)):   # (!p0) br #2: lt(1, 2) true -> not taken
    TESTS.append((f"br_pred_{c}", PADS + load({"r0": F(1), "r1": F(2)})
                  + [f"test.fsub.{c} p0, r0, r1  [skipinv]", "(!p0) br #2"] + [out(f"r{5 + i}") for i in range(2)],
                  want))
