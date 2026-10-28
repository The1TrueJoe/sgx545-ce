"""hwt.py suite, priority 3: scalar ops (values, source modifiers, f16/c10 sources and channel
select, repeat vs write mask) and the hardware constant table c0-c63."""
from hwt_lib import F, H, load, out, vec

TESTS = []
M1 = "[skipinv mask=1]"
X = {"frcp": [4, 3, 0, -0.0, -2, 1e30], "frsq": [4, 2, 0, -4], "flog2": [8, 3, 1, 0, -1, 0.5],
     "fexp2": [3, 0.5, -1, 128, -126.5, 0.1], "fsqrt": [4, 2, 0, -1],
     "fsin": [0, 0.5, 1, 1.5, 2, -1, -0.5, 3, 4.5, 0.25],
     "fcos": [0, 0.5, 1, 2, -1, 3, 0.25]}
for op, xs in X.items():
    for x in xs:
        TESTS.append((f"{op}_{x:g}", load({"r0": F(x)}) + [f"{op} r3, r0  {M1}", out("r3")], None))
# source modifiers on a scalar op
for m, src in (("neg", "-r0"), ("abs", "|r0|"), ("negabs", "-|r0|")):
    TESTS.append((f"frcp_{m}", load({"r0": F(-4)}) + [f"frcp r3, {src}  {M1}", out("r3")],
                  {"neg": F(0.25), "abs": F(0.25), "negabs": F(-0.25)}[m]))
# f16 source: r0 = (lo 4.0, hi 2.0), r1 = (lo 8.0, hi 16.0)
for ch in range(4):
    for f2 in ("", " f2_22"):
        TESTS.append((f"frcp_f16_ch{ch}{f2.replace(' ', '_')}", load({"r0": H(4, 2), "r1": H(8, 16)})
                      + [f"frcp r3, r0  [skipinv mask=1 f16 chan={ch}{f2}]", out("r3")], None))
# c10 source: r0 = ch0 0x100 | ch1 0x080 << 10 | ch2 0x040 << 20, r1 = 0x020 | 0x3ff << 10
C10 = {"r0": 0x100 | 0x080 << 10 | 0x040 << 20, "r1": 0x020 | 0x3ff << 10}
for ch in range(4):
    TESTS.append((f"frcp_c10_ch{ch}", load(C10) + [f"frcp r3, r0  [skipinv mask=1 c10 chan={ch}]", out("r3")], None))
# c10 negative / max channel values through fexp2 (2^x is exact for small integers)
for v in (0x3ff, 0x200, 0x1ff, 0x300, 0x0ff):
    TESTS.append((f"fexp2_c10_{v:03x}", load({"r0": v}) + [f"fexp2 r3, r0  [skipinv mask=1 c10 chan=0]", out("r3")], None))
# repeat vs write mask: r0-r3 = 1, 2, 4, 8; r4-r7 sentinels
R = {**vec(0, [1, 2, 4, 8]), **{f"r{4 + i}": 0x5a5a5a50 + i for i in range(4)}}
for i in range(4):
    TESTS.append((f"frcp_rpt4_{i}", load(R) + ["frcp r4, r0  [skipinv rcnt rpt=4]", out(f"r{4 + i}")], F(1 / [1, 2, 4, 8][i])))
    TESTS.append((f"frcp_mask5_{i}", load(R) + ["frcp r4, r0  [skipinv mask=5]", out(f"r{4 + i}")], None))
    TESTS.append((f"frcp_mask0_{i}", load(R) + ["frcp r4, r0  [skipinv mask=0]", out(f"r{4 + i}")], None))
# the hardware constant table
for n in range(64):
    TESTS.append((f"c{n}", [f"mov o0, c{n}  {M1[:-1]} end]".replace("[skipinv mask=1 end]", "[skipinv end mask=1]")], None))
