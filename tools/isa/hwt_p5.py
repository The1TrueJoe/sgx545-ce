"""hwt.py suite: follow-ups to p1-p4 (fadm/fmsa formulas, the scalar ops' h2 F16 destination,
C10 source channel 3 and range, ba)."""
from hwt_lib import F, H, load, out

TESTS = []
M1 = "[skipinv mask=1]"
# fadm = (s0 + s1) * s2 and fmsa = s0 * s0 + s1 * s2 fit (-3, 5, 7.5); check on (2, 3, 4) and (0.5, -6, 3)
for a, b, c in ((2, 3, 4), (0.5, -6, 3)):
    regs = {"r0": F(a), "r1": F(b), "r2": F(c)}
    TESTS.append((f"fadm_{a:g}_{b:g}_{c:g}", load(regs) + ["fadm r3, r0, r1, r2  [skipinv]", out("r3")], F((a + b) * c)))
    TESTS.append((f"fmsa_{a:g}_{b:g}_{c:g}", load(regs) + ["fmsa r3, r0, r1, r2  [skipinv]", out("r3")], F(a * a + b * c)))
# scalar h2 (f16d): F16 result into the half the source half selects; the other half is zeroed (hardware)
for ch in (0, 1):
    TESTS.append((f"frcp_f16dst_ch{ch}", load({"r0": H(4, 2), "r3": 0x5a5a5a5a})
                  + [f"frcp r3, r0  [skipinv mask=1 f16 chan={ch} f16d]", out("r3")],
                  [0x00003400, 0x38000000][ch]))
TESTS.append(("frcp_f32src_f16dst", load({"r0": F(4), "r3": 0x5a5a5a5a}) + ["frcp r3, r0  [skipinv mask=1 f16d]", out("r3")], None))
TESTS.append(("frcp_f16dst_rpt2", load({"r0": H(4, 2), "r1": H(8, 16), "r3": 0x5a5a5a5a, "r4": 0x5a5a5a5a})
              + ["frcp r3, r0  [skipinv rcnt rpt=2 f16 f16d]", out("r4")], None))
# C10 channel 3 of a 32-bit register: bits 30-31?
for v in (0x40000000, 0x80000000, 0xc0000000):
    TESTS.append((f"frcp_c10_ch3_{v:08x}", load({"r0": v}) + ["frcp r3, r0  [skipinv mask=1 c10 chan=3]", out("r3")], None))
# C10 range: v / 255 up to where?
for v in (0x1f0, 0x1fe, 0x1ff, 0x201, 0x202):
    TESTS.append((f"frcp_c10_{v:03x}", load({"r0": v}) + ["frcp r3, r0  [skipinv mask=1 c10 chan=0]", out("r3")], None))
