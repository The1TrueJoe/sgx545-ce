"""hwt.py suite, priority 1: fmad source modifiers, repeat (moe vs inc bits), fmt + .f16 sources,
and the other hw0 sub-ops (fadm, fmsa, fsubflr)."""
from hwt_lib import F, H, load, out, vec

A = {"r0": F(-3), "r1": F(5), "r2": F(-7.5)}
C = {"r0": F(-3), "r1": F(5), "r2": F(7.5)}
TESTS = []


def t(name, regs, op, expect, res="r3"):
    TESTS.append((name, load(regs) + [op + "  [skipinv]" if "[" not in op else op, out(res)], expect))


# modifiers (ISA.md: h7 |src0|, h5/h6 -src1/|src1|, h3/h4 -src2/|src2|)
t("fmad_plain", A, "fmad r3, r0, r1, r2", F(-22.5))
t("fmad_abs0", A, "fmad r3, |r0|, r1, r2", F(7.5))
t("fmad_neg1", A, "fmad r3, r0, -r1, r2", F(7.5))
t("fmad_abs1", {"r0": F(-3), "r1": F(-5), "r2": F(-7.5)}, "fmad r3, r0, |r1|, r2", F(-22.5))
t("fmad_negabs1", A, "fmad r3, r0, -|r1|, r2", F(7.5))
t("fmad_neg2", C, "fmad r3, r0, r1, -r2", F(-22.5))
t("fmad_abs2", A, "fmad r3, r0, r1, |r2|", F(-7.5))
t("fmad_negabs2", C, "fmad r3, r0, r1, -|r2|", F(-22.5))
# other sub-ops of hw0, on (-3, 5, 7.5): fadm / fmsa names are guesses
t("fadm", C, "fadm r3, r0, r1, r2", None)
t("fmsa", C, "fmsa r3, r0, r1, r2", None)
t("fsubflr", {"r1": F(2.75), "r2": F(1.5)}, "fsubflr r3, r1, r2", F(1.75))
t("fsubflr_neg", {"r1": F(-2.75), "r2": F(-2.75)}, "fsubflr r3, r1, r2", F(0.25))

# repeats: s0 = 1,2,3,4 (r0-3), s1 = 10,100,1000,10000 (r4-7), s2 = 1/8,1/4,1/2,3/4 (r8-11), dst r12-15
S0, S1, S2 = [1, 2, 3, 4], [10, 100, 1000, 10000], [0.125, 0.25, 0.5, 0.75]
V = {**vec(0, S0), **vec(4, S1), **vec(8, S2)}
for tag, flags, steps in (("moe", "rpt=4 moe", (1, 1, 1)), ("inc0", "rpt=4 inc0", (1, 0, 0)),
                          ("inc1", "rpt=4 inc1", (0, 1, 0)), ("inc2", "rpt=4 inc2", (0, 0, 1)),
                          ("inc012", "rpt=4 inc0 inc1 inc2", (1, 1, 1)), ("noinc", "rpt=4", (0, 0, 0))):
    for i in range(4):
        a, b, c = (S0[i * steps[0]], S1[i * steps[1]], S2[i * steps[2]])
        TESTS.append((f"rpt_{tag}_{i}", load(V) + [f"fmad r12, r0, r4, r8  [skipinv {flags}]", out(f"r{12 + i}")],
                      F(a * b + c)))
# rpt=2: does it stop after 2 (r14 keeps its limm)?
TESTS.append(("rpt2_stop", load({**V, "r14": 0x5a5a5a5a}) + ["fmad r12, r0, r4, r8  [skipinv rpt=2 moe]", out("r14")],
              0x5a5a5a5a))

# fmt + .f16: r0 = (lo 3.0, hi 2.0), r1 = (lo 5.0, hi 4.0). Is the number a register (low half?) or a half index?
FH = {"r0": H(3, 2), "r1": H(5, 4), "r2": F(0.5)}
for n in range(4):
    TESTS.append((f"f16_src0_r{n}", load(FH) + [f"fmad r3, r{n}.f16, c52, c48  [skipinv fmt]", out("r3")], None))
for n in range(2):
    TESTS.append((f"f16_src1_r{n}", load(FH) + [f"fmad r3, r2, r{n}.f16, c48  [skipinv fmt]", out("r3")], None))
    TESTS.append((f"f16_src2_r{n}", load(FH) + [f"fmad r3, r2, c52, r{n}.f16  [skipinv fmt]", out("r3")], None))
# fmt set with no F16 source: plain f32 numbers keep working?
TESTS.append(("fmt_f32", load(FH) + ["fmad r3, r2, c4, c52  [skipinv fmt]", out("r3")], F(2.0)))
