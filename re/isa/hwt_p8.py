"""hwt.py suite (its test lines ran without [fmt]: the assembler of the time dropped .f16/.c10 there,
so they are f32 / plain-register compares, written that way below; see hwt_p10): follow-ups to p7 (which half test's .f16 reads, setfc's first argument, the
C10 alpha source, test chan on a C10 compare)."""
from hwt_lib import F, H, load, out

TESTS = []
M1 = "[skipinv mask=1]"
# test.fsub .f16: r0 = (3, 10), r1 = (5, 1): lo halves 3 < 5, hi halves 10 > 1
for a, b in (("r0", "r1"), ("r1", "r0")):
    TESTS.append((f"test_f16_half_{a}_lt_{b}", load({"r0": H(3, 10), "r1": H(5, 1), "r3": 0xaaaaaaaa})
                  + [f"test.fsub.lt p0, {a}, {b}  [skipinv]", f"(p0) mov r3, c52  {M1}", out("r3")], None))
# efo with number bit 6 set (r0 as F16 when setfc's first argument is on) with no setfc and with setfc #0, #1:
# raw word = `efo r3, r0.f16.lo, r1, c52 [d=a0 ...]` as assembled under setfc #1, #1 (hwt_p7)
EFO_F16_R0 = "raw 38a30080 107000b4"
FH = {"r0": H(3, 2), "r1": H(5, 4)}
TESTS.append(("efo_bit6_nofc", load(FH) + [EFO_F16_R0, out("r3")], None))
TESTS.append(("efo_bit6_fc01", load(FH) + ["setfc #0, #1", EFO_F16_R0, out("r3")], None))
TESTS.append(("efo_bit6_fc11", load(FH) + ["setfc #1, #1", EFO_F16_R0, out("r3")], F(4)))
# C10 alpha of src2 in a sop2 select: r1 channels 0x010, 0x020, 0x040 (U8 destination)
SEL = "c1=one c2=zero cop=add a1=zero a2=one aop=add"
TESTS.append(("sop2_c10_alpha_src", load({"r0": 0x100, "r1": 0x010 | 0x020 << 10 | 0x040 << 20})
              + [f"sop2 r3, r0.c10, r1.c10  [skipinv {SEL}]", out("r3")], None))
# U8 destination from C10: which channel goes to which byte (r0 = 0x011, 0x022, 0x044)
TESTS.append(("sop2_c10_bytes", load({"r0": 0x011 | 0x022 << 10 | 0x044 << 20, "r1": 0})
              + [f"sop2 r3, r0.c10, r1.c10  [skipinv {SEL}]", out("r3")], None))
# test chan: only channel k of r0 is below r1 (0x010 vs 0x020; the others 0x050)
for k in range(3):
    r0 = sum((0x010 if c == k else 0x050) << 10 * c for c in range(3))
    for ch in range(4):
        TESTS.append((f"test_c10_onehot{k}_chan{ch}", load({"r0": r0, "r1": 0x020 | 0x020 << 10 | 0x020 << 20, "r3": 0xaaaaaaaa})
                      + [f"test.fpsub8.lt p0, r0, r1  [skipinv chan={ch}]", f"(p0) mov r3, c52  {M1}", out("r3")], None))
