"""hwt suite for hwt_open.py: with the compare on (T3 b31: 1.0 where ref <
texel), one more T3 bit or one zero T0 bit at a time, at references below,
at and above the texel (0.5): which field holds the compare function. T3
b13, b14 and b19 fault, so they're left out."""
from hwt_lib import END
from hwt_pcf import T, TEXEL, coords, SENT

TESTS = []
TEXS = {}
B31 = T[3] | 1 << 31


def add(name, t0, t3, ref):
    TEXS[name] = TEXEL
    TESTS.append((name, [
        f"limm r4, #{t0:#010x}  [skipinv]",
        f"limm r5, #{T[1]:#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{t3:#010x}  [skipinv]",
    ] + coords(ref) + SENT +
        ["smp.2d.pcf r0, r8, r4  [skipinv]", "wdf", f"mov o0, r0  {END}"], None))


for bit in range(31):
    if B31 >> bit & 1 or bit in (13, 14, 19):
        continue
    for ref in (0.25, 0.5, 0.75):
        add(f"t3b{bit:02d}_ref{int(ref * 100):03d}", T[0], B31 | 1 << bit, ref)
for bit in range(3, 32):
    if T[0] >> bit & 1:
        continue
    for ref in (0.25, 0.5, 0.75):
        add(f"t0b{bit:02d}_ref{int(ref * 100):03d}", T[0] | 1 << bit, B31, ref)
