"""hwt suite for hwt_open.py: what smp's PCF bit does on a depth texture
(ISA.md "hw 1c: smp" h5; TEX.md 15, test 3: the DDK's ES2 has no
EXT_shadow_samplers).

A 16x16 depth16 texture as the DDK stores one (the dt16 capture, STATE.md /
TEX.md 15: converted to F32 on upload, T0 00060e00, T1 d200f00f 16x16, format
0x12, layout 6 strided with a 32-texel pitch, T3 00800800; a sample returns
(d, d, d, 1)), every texel 0.5. First a plain dependent smp at (0.5, 0.5), to
see the result's registers (r0-r3); then
smp.2d.pcf with the reference as a third coordinate (r10, the guess: the
coordinate count follows the PCF mode), swept across 0.5. A compare shows as
the result switching between the references below and above the texel.
"""
import struct

from hwt_lib import END

TESTS = []
TEXS = {}

TEXEL = 0x3F000000  # 0.5
T = [0x00060E00, 0xD200F00F, None, 0x00800800]


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def words(t3=T[3]):
    return [
        f"limm r4, #{T[0]:#010x}  [skipinv]",
        f"limm r5, #{T[1]:#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{t3:#010x}  [skipinv]",
    ]


def coords(ref=None):
    c = [f"limm r8, #{f32(0.5):#010x}  [skipinv]",
         f"limm r9, #{f32(0.5):#010x}  [skipinv]"]
    if ref is not None:
        c.append(f"limm r10, #{f32(ref):#010x}  [skipinv]")
    return c


SENT = [f"limm r{k}, #0x5a5a5a{k:02x}  [skipinv]" for k in range(4)]

for r in ("r0", "r1", "r2", "r3"):
    name = f"dt16_plain_{r}"
    TEXS[name] = TEXEL
    TESTS.append((name, words() + coords() + SENT +
                  ["smp.2d r0, r8, r4  [skipinv]", "wdf", f"mov o0, {r}  {END}"], None))

for ref in (0.0, 0.25, 0.49, 0.51, 0.75, 1.0):
    for r in ("r0", "r1") if ref in (0.25, 0.75) else ("r0",):
        name = f"pcf_ref{int(round(ref * 100)):03d}_{r}"
        TEXS[name] = TEXEL
        TESTS.append((name, words() + coords(ref) + SENT +
                      ["smp.2d.pcf r0, r8, r4  [skipinv]", "wdf", f"mov o0, {r}  {END}"], None))
