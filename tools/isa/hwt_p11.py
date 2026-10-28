"""hwt.py suite: how a repeat steps an F16 operand (by a half or by a register), as the corpus's
mediump `fdp pa0, pa2.f16.lo, pa0.f16.lo [rpt=3 moe fmt]` relies on."""
from hwt_lib import H, load, out

TESTS = []
R = {"r0": H(3, 2), "r1": H(5, 4), "r12": 0x5a5a5a5a, "r13": 0x5a5a5a5a}
for how in ("moe", "inc0"):
    for i in range(2):
        TESTS.append((f"f16_step_{how}_{i}", load(R) + [f"fmad r12, r0.f16.lo, c52, c48  [skipinv rpt=2 {how} fmt]", out(f"r{12 + i}")],
                      None))
