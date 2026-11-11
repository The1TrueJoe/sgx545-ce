"""hwt suite for hwt_open.py: smp.2d.pcf with T0 b0-2 (0 in every 2D
capture) set to 1-7, the reference below (0.25) and above (0.75) the texel
(0.5): a compare function in that field shows as results that differ between
the two references and across the values."""
from hwt_lib import END
from hwt_pcf import T, TEXEL, coords, SENT

TESTS = []
TEXS = {}

for v in range(1, 8):
    for ref in (0.25, 0.75):
        name = f"pcf_t0lo{v}_ref{int(ref * 100):03d}"
        TEXS[name] = TEXEL
        TESTS.append((name, [
            f"limm r4, #{T[0] | v:#010x}  [skipinv]",
            f"limm r5, #{T[1]:#010x}  [skipinv]",
            "limm r6, TEX  [skipinv]",
            f"limm r7, #{T[3]:#010x}  [skipinv]",
        ] + coords(ref) + SENT +
            ["smp.2d.pcf r0, r8, r4  [skipinv]", "wdf", f"mov o0, r0  {END}"], None))
