"""hwt suite for hwt_open.py: smp.2d.pcf with the reference outside [0, 1],
then one T3 bit, then one zero T0 bit at a time (T0 b0-2 = 3, ref 0.25 below
the texel 0.5): which state turns the compare on."""
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


for lo in (0, 3, 7):
    for ref in (-1.0, 2.0):
        add(f"a_lo{lo}_ref{'m1' if ref < 0 else 'p2'}", T[0] | lo, T[3], ref)
for bit in range(32):
    if not T[3] >> bit & 1:
        add(f"b_t3b{bit:02d}", T[0] | 3, T[3] | 1 << bit, 0.25)
for bit in range(3, 32):
    if not T[0] >> bit & 1:
        add(f"c_t0b{bit:02d}", T[0] | 3 | 1 << bit, T[3], 0.25)
