"""hwt.py suite: the items p1-p6 left open. F16 operand numbering in test and efo, setfc's first
argument, fmad/fmad16 rounding and fusing, C10 alpha, test chan, testmask byte/word, calls (lapc),
fdp, bitwise ops."""
from fractions import Fraction as Fr

from hwt_lib import F, H, load, out, vec

TESTS = []
M1 = "[skipinv mask=1]"
FH = {"r0": H(3, 2), "r1": H(5, 4)}

# test.fsub with F16 sources: half-indexed numbers? r1.f16 = r0.hi (2) if so, else r1.lo (5)
for a, b, half in (("r1", "r0", True), ("r0", "r1", False)):
    # drawn without fmt: the assembler of the time dropped .f16, so this compared f32 words (see hwt_p10)
    TESTS.append((f"test_f16_{a}_lt_{b}", load({**FH, "r3": 0xaaaaaaaa})
                  + [f"test.fsub.lt p0, {a}, {b}  [skipinv]", f"(p0) mov r3, c52  {M1}", out("r3")],
                  F(1) if half else 0xaaaaaaaa))
# efo d = a0 = s0 + s2 (corpus control word 0x230600 without the I writes)
EFO = "[skipinv d=a0 i=i0=a0,i1=a1 a=a0=s0+s2,a1=s1+s2 m=m0=s0*s2,m1=s1*s2]"
TESTS.append(("efo_f32", load({"r0": F(3), "r1": F(5)}) + [f"efo r3, r0, r1, c52  {EFO}", out("r3")], F(4)))
for fc in ("setfc #1, #1", "setfc #1, #0"):
    for s0 in ("r0", "r1"):
        # encoded number 0 / 1 = r0.f16.lo / r0.f16.hi (efo's F16 numbers count halves)
        TESTS.append((f"efo_f16_{s0}_{fc[7]}{fc[11]}", load(FH)
                      + [fc, f"efo r3, r0.f16.{('lo', 'hi')[s0 == 'r1']}, r1, c52  {EFO}", out("r3")], None))
# fmad: fused? (1 + 2^-12)^2 - (1 + 2^-11) = 2^-24 exactly; rounding the product first gives 0
a = 1 + 2 ** -12
TESTS.append(("fmad_fused", load({"r0": F(a), "r1": F(a), "r2": F(-(1 + 2 ** -11))}) + ["fmad r3, r0, r1, r2  [skipinv]", out("r3")], None))
# fmad rounding: a = 1 + 2069 * 2^-23, a*a: nearest 0x3f8008 2b? (see below), truncation one ulp lower
a = 1 + 2069 * 2 ** -23
p = Fr(a) * Fr(a)
ulp = Fr(1, 2 ** 23)
TESTS.append(("fmad_round", load({"r0": F(a)}) + ["fmad r3, r0, r0, c48  [skipinv]", out("r3")],
              F(float(p - p % ulp + ulp))))
TESTS.append(("fmad16_round", load({"r0": H(1 + 23 * 2 ** -10, 1 + 23 * 2 ** -10)}) + ["fmad16 r3, r0, r0, c48  [skipinv]", out("r3")], None))
# f32 -> f16 conversion inside fmad16 (swizzle 3) on 1 + 3 * 2^-12: nearest 0x3c01, truncation 0x3c00
TESTS.append(("fmad16_cvt_round", load({"r0": F(1 + 3 * 2 ** -12)}) + ["fmad16 r3, r0, c45, c48  [skipinv swz0=3]", out("r3")], None))
# C10 alpha: sop2 select (alpha from src2); r1 bits 30-31 = 0..3
C10 = 0x100 | 0x080 << 10 | 0x040 << 20
SEL = "c1=one c2=zero cop=add a1=zero a2=one aop=add"
for hi in range(4):
    TESTS.append((f"sop2_c10_alpha{hi}", load({"r0": C10, "r1": hi << 30})
                  + [f"sop2 r3, r0.c10, r1.c10  [skipinv {SEL}]", out("r3")], None))
    TESTS.append((f"sop2_c10dst_alpha{hi}", load({"r0": C10, "r1": hi << 30, "r3": 0})
                  + [f"sop2 r3.c10, r0.c10, r1.c10  [skipinv {SEL}]", out("r3")], None))
# test chan on a C10 compare: r0 = (0x010, 0x050, 0x010), r1 = (0x020, 0x020, 0x020): lt per channel T F T
# drawn without fmt (the .c10 suffix was dropped): really fpsub8 on plain registers; superseded by hwt_p10
for ch in range(4):
    TESTS.append((f"test_c10_chan{ch}", load({"r0": 0x010 | 0x050 << 10 | 0x010 << 20, "r1": 0x020 | 0x020 << 10 | 0x020 << 20,
                                              "r3": 0xaaaaaaaa})
                  + [f"test.fpsub8.lt p0, r0, r1  [skipinv chan={ch}]", f"(p0) mov r3, c52  {M1}", out("r3")], None))
# testmask widths
for w in ("byte", "word", "dword"):
    for x, y in ((1, 2), (2, 1)):
        TESTS.append((f"testmask_{w}_{x}{y}", load({"r0": F(x), "r1": F(y), "r3": 0x5a5a5a5a})
                      + [f"testmask.fsub.lt.{w} r3, r0, r1  [skipinv]", out("r3")], None))
# call / return: br [link] to a body that sets r5, lapc back to the instruction after the call
PADS = load({"r5": 0x55, "r6": 0x66})
TESTS.append(("call_lapc", PADS + ["br #3  [link]", out("r5"), out("r6"), "limm r5, #0x99  [skipinv]", "lapc"], 0x99))
# fdp: r0-3 = 1, 2, 3, 4; r4-7 = 10, 100, 1000, 10000; dst r12
V = {**vec(0, [1, 2, 3, 4]), **vec(4, [10, 100, 1000, 10000]), **{f"r{12 + i}": 0x5a5a5a50 + i for i in range(4)}}
for n in (3, 4):
    for i in range(4):
        TESTS.append((f"fdp_rpt{n}_{i}", load(V) + [f"fdp r12, r0, r4  [skipinv rpt={n} moe]", out(f"r{12 + i}")], None))
# bitwise
A, B = 0xf0f0ff00, 0x3c3c0ff0
for name, op, regs, want in (
        ("and", "and r3, r0, r1", {"r0": A, "r1": B}, A & B),
        ("or", "or r3, r0, r1", {"r0": A, "r1": B}, A | B),
        ("xor", "xor r3, r0, r1", {"r0": A, "r1": B}, A ^ B),
        ("shl4", "shl r3, r0, #4", {"r0": A}, A << 4 & 0xffffffff),
        ("shr4", "shr r3, r0, #4", {"r0": A}, A >> 4),
        ("asr4", "asr r3, r0, #4", {"r0": A}, (A >> 4) | 0xf0000000),
        ("rol4", "rol r3, r0, #4", {"r0": A}, (A << 4 | A >> 28) & 0xffffffff),
        ("shr_reg", "shr r3, r0, r1", {"r0": A, "r1": 8}, A >> 8),
        ("shl_33", "shl r3, r0, r1", {"r0": A, "r1": 33}, None),
        ("and_rot", "and r3, r0, #0xff00  [skipinv rot=16]", {"r0": 0xffffffff}, 0xff000000),
        ("and_inv", "and.inv r3, r0, #1", {"r0": 0xffffffff}, 0xfffffffe)):
    TESTS.append((f"bit_{name}", load(regs) + [op if "[" in op else op + "  [skipinv]", out("r3")], want))
