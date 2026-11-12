"""hwt suite for hwt_open.py: a volume texture through smp's 3D dimension
(TEX.md 14, test 3: the DDK's ES2 has no OES_texture_3D).

The texture heap holds dword i = 0x5a5a0000 | i (TEXS "pat"), so the texel a
sample returns names the dword it read. The texture is a 4x4x4 RGBA8888 one by
size: T1 = layout << 29 | 0x0d << 24 | 3 << 12 | 3, for the candidate volume
layouts 1, 4, 5 and 7 (TEX.md: T1's unused layout codes), and layout 0
(twiddled 2D) as the control. T0 is the 2D nearest/repeat word of hwt_smp, so
the depth, if it lives in T0 b0-2 or T3, is the default here: a layout that
reads r at all shows it as a different dword for each r. One dependent
smp.3d at (s, t, r) per test, s and t at texel centres.
"""
import struct

from hwt_lib import END

TESTS = []
TEXS = {}

T0 = 0x07FE0000
T3 = 0x00800800


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


POINTS = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 2), (0, 0, 3)]

for layout in (0, 1, 4, 5, 7):
    t1 = layout << 29 | 0x0D << 24 | 3 << 12 | 3
    for x, y, z in POINTS:
        s, t, r = ((v + 0.5) / 4 for v in (x, y, z))
        name = f"vol_l{layout}_{x}{y}{z}"
        TEXS[name] = "pat"
        TESTS.append((name, [
            f"limm r4, #{T0:#010x}  [skipinv]",
            f"limm r5, #{t1:#010x}  [skipinv]",
            "limm r6, TEX  [skipinv]",
            f"limm r7, #{T3:#010x}  [skipinv]",
            f"limm r8, #{f32(s):#010x}  [skipinv]",
            f"limm r9, #{f32(t):#010x}  [skipinv]",
            f"limm r10, #{f32(r):#010x}  [skipinv]",
            "limm r0, #0x5a5a5a5a  [skipinv]",
            "smp.3d r0, r8, r4  [skipinv]",
            "wdf",
            f"mov o0, r0  {END}"], None))
