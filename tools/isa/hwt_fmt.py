"""hwt suite for hwt_open.py: what each T1 texture format code returns
(docs/GL3.md, test H2; TEX.md §1 and §5: only 0x02, 0x04, 0x05, 0x0c, 0x0d,
0x12, 0x19, 0x1a and 0x1b are known).

A 2x2 texture, T1 layout 6 (strided) with an 8-dword pitch (T0 00000e00,
nearest, repeat), every dword of the heap one value, sampled by a dependent
`smp.2d r0, r8, r4` at (0.5, 0.5) with the plain sample's T3 (00800800: one
register for RGBA8888, hwt_smp). r0-r3 start as sentinels, so a test shows
which registers a format writes and what lands in each.

- `c<code>_r<n>`: format code 0x00-0x1f, result register n, with the texel
  0xc0804020 (bytes 0x20, 0x40, 0x80, 0xc0: an 8-bit format shows its
  channel order, a 16-bit one two halves, a float one a recognizable F32).
- `f12_<name>`: format 0x12 (F32, one channel: the depth format), with
  texels a float path could change (a NaN payload, a denormal, -0, the
  largest finite float, a bit pattern that is no particular float). Each
  should read back unchanged if 0x12 can carry raw 32-bit integers
  (EXT_texture_integer's R32UI through the sampler).

Codes no capture shows may page-fault or return nothing; run the suite in a
slot where a fault is acceptable, and run the codes in small groups with -k
(e.g. -k c0 for 0x00-0x0f) so one fault doesn't hide the rest.
"""
from hwt_lib import END

TESTS = []
TEXS = {}

TEXEL = 0xC0804020
T0 = 0x00000E00  # strided pitch 8 dwords (pitch - 1 = 7 in b9-11), nearest, repeat
T3 = 0x00800800  # the plain sample's T3


def t1(code):
    """layout 6 strided, the format, 2x2"""
    return 6 << 29 | code << 24 | 1 << 12 | 1


def sample(code, reg):
    return [
        f"limm r4, #{T0:#010x}  [skipinv]",
        f"limm r5, #{t1(code):#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{T3:#010x}  [skipinv]",
        "limm r8, #0x3f000000  [skipinv]",
        "limm r9, #0x3f000000  [skipinv]",
    ] + [f"limm r{k}, #0x5a5a5a{k:02x}  [skipinv]" for k in range(4)] + [
        "smp.2d r0, r8, r4  [skipinv]",
        "wdf",
        f"mov o0, {reg}  {END}",
    ]


for code in range(0x20):
    for r in range(4):
        name = f"c{code:02x}_r{r}"
        TEXS[name] = TEXEL
        TESTS.append((name, sample(code, f"r{r}"), None))

F12 = {
    "nan_payload": 0x7FC12345,
    "snan": 0x7F800001,
    "denormal": 0x00000001,
    "negzero": 0x80000000,
    "maxfloat": 0x7F7FFFFF,
    "pattern": 0x12345678,
    "negint": 0xFFFFFFFE,
}
for n, v in F12.items():
    name = f"f12_{n}"
    TEXS[name] = v
    TESTS.append((name, sample(0x12, "r0"), v))
