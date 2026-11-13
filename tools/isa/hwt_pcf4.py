"""hwt suite for hwt_open.py: smp.2d.pcf with T3 b31 set (the bit that made
the compare return 1.0), T0 b0-2 = 0-7 against references around the texel
(0.5); a plain smp with b31 set; r1 after a pcf."""
from hwt_lib import END
from hwt_pcf import T, TEXEL, coords, SENT

TESTS = []
TEXS = {}
B31 = T[3] | 1 << 31
REFS = (0.0, 0.25, 0.49, 0.5, 0.51, 0.75, 1.0)


def add(name, t0, t3, ref, op="smp.2d.pcf", r="r0"):
    TEXS[name] = TEXEL
    TESTS.append((name, [
        f"limm r4, #{t0:#010x}  [skipinv]",
        f"limm r5, #{T[1]:#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{t3:#010x}  [skipinv]",
    ] + coords(ref) + SENT +
        [f"{op} r0, r8, r4  [skipinv]", "wdf", f"mov o0, {r}  {END}"], None))


for lo in range(8):
    for ref in REFS:
        add(f"f{lo}_ref{int(round(ref * 100)):03d}", T[0] | lo, B31, ref)
add("plain_b31_r0", T[0], B31, None, "smp.2d")
add("plain_b31_r1", T[0], B31, None, "smp.2d", "r1")
add("f3_ref025_r1", T[0] | 3, B31, 0.25, r="r1")
