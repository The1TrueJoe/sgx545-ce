"""hwt suite for hwt_open.py: what decides a dependent sample's result format
(ISA.md "hw 1c: smp": nothing in the word; the texture state, unconfirmed).

One smp word, `smp.2d r0, r8, r4` at (0.5, 0.5), on an 8x8 RGBA8888 texture whose
every texel is bytes 0x10, 0x20, 0x40, 0x80 in memory (R, G, B, A). Its state words
are in r4-r7 (STATE.md section 5: T0 nearest/repeat, T1 RGBA8888 8x8 twiddled,
T2 the texture, T3 the shader's half). T3 = 0x00800800 is the U8 return the driver
uses; 0x01028800 the F16 return of its f16_samples. r0 and r1 are read back, r1
preset to a sentinel, so a second result register shows.
"""
from hwt_lib import END

TESTS = []
TEXS = {}
TEXEL = 0x80402010
T3 = {"u8": 0x00800800, "f16": 0x01028800}
for kind, t3 in T3.items():
    for r in ("r0", "r1"):
        name = f"smp_{kind}_{r}"
        TEXS[name] = TEXEL
        TESTS.append((name, [
            "limm r4, #0x07fe0000  [skipinv]",
            "limm r5, #0x0d007007  [skipinv]",
            "limm r6, TEX  [skipinv]",
            f"limm r7, #{t3:#x}  [skipinv]",
            "limm r8, #0x3f000000  [skipinv]",
            "limm r9, #0x3f000000  [skipinv]",
            "limm r0, #0x5a5a5a5a  [skipinv]",
            "limm r1, #0xa5a5a5a5  [skipinv]",
            "smp.2d r0, r8, r4  [skipinv]",
            "wdf",
            f"mov o0, {r}  {END}"], None))
