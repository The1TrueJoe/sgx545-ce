"""hwt suite for hwt_open.py: T3 b0-2 = 1-7 on layout 1 (hwt_vol2: a T3 low
bit makes r read), r swept in sixteenths at s = t = 0: the dword each r
reads gives the depth and slice order the field selects."""
from hwt_lib import END
from hwt_vol import T0, T3, f32

TESTS = []
TEXS = {}
T1 = 1 << 29 | 0x0D << 24 | 3 << 12 | 3

for v in range(1, 8):
    for k in range(16):
        name = f"v{v}_r{k:02d}"
        TEXS[name] = "pat"
        TESTS.append((name, [
            f"limm r4, #{T0:#010x}  [skipinv]",
            f"limm r5, #{T1:#010x}  [skipinv]",
            "limm r6, TEX  [skipinv]",
            f"limm r7, #{T3 | v:#010x}  [skipinv]",
            f"limm r8, #{f32(0.125):#010x}  [skipinv]",
            f"limm r9, #{f32(0.125):#010x}  [skipinv]",
            f"limm r10, #{f32((k + 0.5) / 16):#010x}  [skipinv]",
            "limm r0, #0x5a5a5a5a  [skipinv]",
            "smp.3d r0, r8, r4  [skipinv]",
            "wdf",
            f"mov o0, r0  {END}"], None))

# depth 4 (T3 b0-2 = 3): the x / y bits against the slice bits
for p in ((2, 0, 0), (0, 2, 0), (1, 1, 1), (3, 3, 3), (3, 0, 0), (0, 3, 0), (0, 0, 3)):
    s, t, r = ((v + 0.5) / 4 for v in p)
    name = f"m_{''.join(map(str, p))}"
    TEXS[name] = "pat"
    TESTS.append((name, [
        f"limm r4, #{T0:#010x}  [skipinv]",
        f"limm r5, #{T1:#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{T3 | 3:#010x}  [skipinv]",
        f"limm r8, #{f32(s):#010x}  [skipinv]",
        f"limm r9, #{f32(t):#010x}  [skipinv]",
        f"limm r10, #{f32(r):#010x}  [skipinv]",
        "limm r0, #0x5a5a5a5a  [skipinv]",
        "smp.3d r0, r8, r4  [skipinv]",
        "wdf",
        f"mov o0, r0  {END}"], None))
