"""hwt suite for hwt_open.py: where a volume's depth lives. hwt_vol's 4x4x4
RGBA8888 texpat texture on layouts 0 (twiddled) and 1 (linear) returned
slice 0 for every r. Here T0 b0-2 = 0-7, then one T3 bit at a time, at
(0, 0, 1), (0, 0, 2) and (1, 0, 0): a field that gives the texture depth
shows as r reading another dword."""
from hwt_lib import END
from hwt_vol import T0, T3, f32

TESTS = []
TEXS = {}
PTS = [(0, 0, 1), (0, 0, 2), (1, 0, 0)]


def add(name, t0, t1, t3, p):
    s, t, r = ((v + 0.5) / 4 for v in p)
    TEXS[name] = "pat"
    TESTS.append((name, [
        f"limm r4, #{t0:#010x}  [skipinv]",
        f"limm r5, #{t1:#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{t3:#010x}  [skipinv]",
        f"limm r8, #{f32(s):#010x}  [skipinv]",
        f"limm r9, #{f32(t):#010x}  [skipinv]",
        f"limm r10, #{f32(r):#010x}  [skipinv]",
        "limm r0, #0x5a5a5a5a  [skipinv]",
        "smp.3d r0, r8, r4  [skipinv]",
        "wdf",
        f"mov o0, r0  {END}"], None))


for layout in (0, 1):
    t1 = layout << 29 | 0x0D << 24 | 3 << 12 | 3
    for v in range(8):
        for p in PTS:
            add(f"l{layout}_t0lo{v}_{''.join(map(str, p))}", T0 | v, t1, T3, p)
    for bit in range(32):
        if T3 >> bit & 1 or bit in (13, 14, 19):
            continue
        for p in PTS:
            add(f"l{layout}_t3b{bit:02d}_{''.join(map(str, p))}", T0, t1, T3 | 1 << bit, p)

# layout 1 with one of T3 b0-6, r = 0-3 at s = t = 0, and s = 1
t1 = 1 << 29 | 0x0D << 24 | 3 << 12 | 3
for bit in range(7):
    for p in ((0, 0, 0), (0, 0, 1), (0, 0, 2), (0, 0, 3), (1, 0, 0), (0, 1, 0)):
        add(f"z_b{bit}_{''.join(map(str, p))}", T0, t1, T3 | 1 << bit, p)
