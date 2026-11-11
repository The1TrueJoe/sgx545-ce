"""hwt suite for hwt_open.py: whether a LINEAR compare filters (percentage
closer). The texpat texture: texel (x, y) of the 32-texel pitch is the F32
0x5a5a0000 | (32y + x), so texels increase with their index. Row 7 (t =
7.5 / 16, one row's weight), s between texels 7 and 8; the reference is
texel 231's own value with LESS (T3 b29-31 = 4): texel 231 fails, 232
passes. Nearest gives 0 or 1, a filtered compare the weight of 232."""
from hwt_lib import END
from hwt_pcf import T, SENT, f32

TESTS = []
TEXS = {}
REF = 0x5A5A0000 | (7 * 32 + 7)


def add(name, t0, t3, s, t, ref=REF):
    TEXS[name] = "pat"
    TESTS.append((name, [
        f"limm r4, #{t0:#010x}  [skipinv]",
        f"limm r5, #{T[1]:#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{t3:#010x}  [skipinv]",
        f"limm r8, #{f32(s):#010x}  [skipinv]",
        f"limm r9, #{f32(t):#010x}  [skipinv]",
        f"limm r10, #{ref:#010x}  [skipinv]",
    ] + SENT + ["smp.2d.pcf r0, r8, r4  [skipinv]", "wdf", f"mov o0, r0  {END}"], None))


CMP = T[3] | 4 << 29
TT = 7.5 / 16
for mag, t0 in (("near", T[0]), ("lin", T[0] | 1 << 12)):
    for k, s in enumerate((7.5, 7.75, 8.0, 8.25, 8.5)):
        add(f"{mag}_s{k}", t0, CMP, s / 16, TT)
# the plain value there (no compare), to see the texel order
TEXS["plain_lin_s2"] = "pat"
TESTS.append(("plain_lin_s2", [
    f"limm r4, #{T[0] | 1 << 12:#010x}  [skipinv]",
    f"limm r5, #{T[1]:#010x}  [skipinv]",
    "limm r6, TEX  [skipinv]",
    f"limm r7, #{T[3]:#010x}  [skipinv]",
    f"limm r8, #{f32(7.5 / 16):#010x}  [skipinv]",
    f"limm r9, #{f32(TT):#010x}  [skipinv]",
] + SENT + ["smp.2d r0, r8, r4  [skipinv]", "wdf", f"mov o0, r0  {END}"], None))

# across rows 7 and 8 (texels 231 and 263, 32 apart: their LINEAR average,
# 247, is an F32 value): whether this format filters at all
for mag, t0 in (("near", T[0]), ("lin", T[0] | 1 << 12)):
    for k, t in enumerate((7.75, 8.0, 8.25)):
        name = f"row_plain_{mag}_t{k}"
        TEXS[name] = "pat"
        TESTS.append((name, [
            f"limm r4, #{t0:#010x}  [skipinv]",
            f"limm r5, #{T[1]:#010x}  [skipinv]",
            "limm r6, TEX  [skipinv]",
            f"limm r7, #{T[3]:#010x}  [skipinv]",
            f"limm r8, #{f32(7.5 / 16):#010x}  [skipinv]",
            f"limm r9, #{f32(t / 16):#010x}  [skipinv]",
        ] + SENT + ["smp.2d r0, r8, r4  [skipinv]", "wdf", f"mov o0, r0  {END}"], None))
        add(f"row_pcf_{mag}_t{k}", t0, CMP, 7.5 / 16, t / 16)
