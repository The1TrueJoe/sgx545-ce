"""hwt suite for hwt_open.py: every value of T3 b29-31 (the compare: b31
ref < texel, b30 ref > texel, b29 equal, if a mask) at references below, at
and above the texel (0.5); the result format bits b16-17 = 3."""
from hwt_lib import END
from hwt_pcf import T, TEXEL, coords, SENT

TESTS = []
TEXS = {}


def add(name, t0, t3, ref):
    TEXS[name] = TEXEL
    TESTS.append((name, [
        f"limm r4, #{t0:#010x}  [skipinv]",
        f"limm r5, #{T[1]:#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{t3:#010x}  [skipinv]",
    ] + coords(ref) + SENT +
        ["smp.2d.pcf r0, r8, r4  [skipinv]", "wdf", f"mov o0, r0  {END}"], None))


for v in range(8):
    for ref in (0.25, 0.5, 0.75):
        add(f"cmp{v}_ref{int(ref * 100):03d}", T[0], T[3] | v << 29, ref)
for ref in (0.25, 0.75):
    add(f"fmt3_ref{int(ref * 100):03d}", T[0], T[3] | 4 << 29 | 3 << 16, ref)
