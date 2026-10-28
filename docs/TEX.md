# SGX545 textures, render targets, uploads and presentation

What a GL client needs beyond one flat-shaded triangle, as the Cedarview DDK
1.7 does it on the EA3 (CE5300, SGX545 rev 1.0.14): where texels live in
memory, how they get there, how a render lands in a texture, how depth and
stencil survive between renders, and how a frame reaches the display.

Tags as in `docs/STATE.md`: **[C]** replay-confirmed on the EA3, **[D]**
diff of captures or read from the DDK code that writes it, **[G]** guess.

## Tools

| path | what |
|---|---|
| `tex.py` | the encoder: linear texels → GPU memory (twiddle, NPOT pitch, mip chain) |
| `rawmap.py` | reads a `PROBE_TEXPAT` texture back out of a capture: which texel sits at each offset |
| `check.py` | compares every texel of a capture with `tex.py`'s prediction |
| `tools/probe/probe.c` | new knobs: `PROBE_TEXPAT`, `PROBE_TEXFILE`, `PROBE_TEXLEVELS`, `PROBE_TEXCUBE`, `PROBE_FBO`, `PROBE_FBODS`, `PROBE_FBOREENTER`, `PROBE_QUAD`, `PROBE_FBOCLEAR` |
| `tools/trace/pvrtrace.c` | now also snapshots all mapped memory before every `SGX_SUBMITTRANSFER` (reason `xfer`) |
| `tools/decode/mkreplay.py` | `--xfers` replays transfers and registers transfer contexts; `--write VA:file`; `--no-delta VA:size` |
| `tools/replay/replay.c` | `regxfer` and `xfer` ops; `-F` wraps the real `/dev/fb0` instead of private memory |

`PROBE_TEXPAT=1` makes every texel name itself: RGBA8888 texels are
`(x, y, level | face<<4, 0x80 | x>>8 | (y>>8)<<2)`, 16-bit texels are
`y*w + x`, 8-bit ones `(y*w + x) & 0xff`. A capture's texture buffer then
says where each texel went with no guessing.

## 1. Texel memory layout

All from `PROBE_TEXPAT` captures on 2026-10-28; `check.py` places every
texel of every capture where `tex.py` predicts.

### Texel formats [D]

Every format at 16×16 (`f*` captures), all 256 texels of each in place:

| GL upload | T1 format | in memory |
|---|---|---|
| RGBA / UNSIGNED_BYTE | 0x0d | R, G, B, A bytes, as uploaded |
| RGB / UNSIGNED_BYTE | 0x0d | expanded to R, G, B, 0xff |
| RGB / UNSIGNED_SHORT_5_6_5 | 0x05 | the GL ushort |
| RGBA / UNSIGNED_SHORT_4_4_4_4 | 0x02 | **ARGB4444**: the ushort rotated right by 4 (A on top) |
| RGBA / UNSIGNED_SHORT_5_5_5_1 | 0x04 | **ARGB1555**: the ushort rotated right by 1 (A on top) |
| LUMINANCE_ALPHA | 0x1b | L, A bytes, as uploaded |
| LUMINANCE / ALPHA | 0x1a / 0x19 | the byte |

The 4444/5551 rotations are the driver's conversion on upload; a client
writing memory itself must apply them (`tex.convert`, `sgxtex_convert16`).
The render target's BGRA8888 is another format code (T1 0x0c), so a texture
and a BGRA surface are not interchangeable without a swizzle.

### Twiddled: power-of-two sizes, T1 layout 0 [D]

Square tiles of `t = min(w, h, 16)` texels. Inside a tile the texel index is
the Morton interleave of x and y with **y in the lowest bit**
(`y0 x0 y1 x1 y2 x2 y3 x3`). The tiles follow each other in row-major order.

    i = morton_y_first(x mod t, y mod t) + t*t * ((y / t) * (w / t) + x / t)

| capture | bit map read off the capture (index bit = coordinate bit) |
|---|---|
| p8x32 | y0 x0 y1 x1 y2 x2 \| y3 y4 |
| p64 | y0 x0 y1 x1 y2 x2 y3 x3 \| x4 x5 \| y4 y5 |
| p128x32 | y0 x0 y1 x1 y2 x2 y3 x3 \| x4 x5 x6 \| y4 |
| p256x256 | y0 x0 y1 x1 y2 x2 y3 x3 \| x4 x5 x6 x7 \| y4 y5 y6 y7 |

Up to 32×32 this is indistinguishable from a plain Morton order; 64×64 is
the first size where the 16×16 tiles show (bit 10 is y4, not x5). Checked
texel by texel: p1x1, p2x1, p8x8, p16x8, p8x16, p32x8, p8x32, p64,
p128x32 (4096 texels) and p256x256 (65536): all match, none differ.

### Linear: sizes that are not powers of two, T1 layout 3 [D]

Rows top to bottom, each padded to a multiple of **8 texels**: 12×6 has a
16-texel pitch (0x40 bytes), 100×30 a 104-texel pitch (0x1a0 bytes, the
buffer is exactly 104 × 30 × 4). Every texel of both matches. T1 layout 3
has no pitch field: the sampler derives it from the width the same way.
n5x7 (pitch 8) matches too.

### Mipmaps [D]

Level n+1 follows level n with no padding or alignment, down to 1×1 on both
axes; each level is laid out by its own size (twiddled when both sides are
powers of two). m16 (16×16, 5 levels, 0x554 bytes), m32x8 (32×8 → 16×4 →
8×2 → 4×1 → 2×1 → 1×1, 0x55c bytes) and m565 (RGB565, 0x2aa bytes) match
texel for texel. T2 is the base; nothing else in the texture words moves
with the level count (STATE.md).

**glGenerateMipmap runs on the GPU**: one transfer per level (mg64: six,
before the frame's kicks), each sampling the previous level as a twiddled
texture (T0 `001e0090`: clamp, nearest, no mips) and writing the next one
twiddled. With nearest sampling the DDK's "filter" is decimation: texel
(x, y) of level n is texel (2x+1, 2y+1) of level n−1 (all 1365 generated
texels of mg64). A driver that wants box-filtered mips should make its own.

## 2. Uploads: the CPU for small textures, the transfer queue otherwise [D]

`glTexImage2D` does nothing visible until the first draw that uses the
texture. Then libGLESv2 allocates the texture in the texture heap
(0xf0000000, attribs 0x3, CPU-mapped) and fills it one of two ways:

- **CPU twiddle:** every power-of-two texture of up to 256 texels in the
  captures (p1x1 … p32x8, p8x32). No transfer is submitted; the texels are
  in place at the first kick.
- **Transfer queue (the GPU):** every non-power-of-two texture and every
  power-of-two one from 64×64 up (p64, p128x32, p256x256, n12x6, n100x30).
  The client copies the GL image, linear and unpadded, into a staging buffer
  in the general heap (0x3e000000, 0x80004 bytes), then submits one
  `SGX_SUBMITTRANSFER` whose only sync is the texture's (dst) and whose 3D
  sync is the GLES context's. The snapshot before the transfer shows the
  texture all zero; the one before kick 1 shows it filled.

(The Mac-side DDK copy's `powervr.ini` says `DisableHWTextureUpload=1`; the
box evidently does not read that file. The cut-over between 256 and 4096
texels was not narrowed down.) Either way the bytes are the same, so a
client that twiddles on the CPU with `tex.py` never needs the transfer.

## 3. The transfer queue: a 3D render with only a background object [D]

`SGX_SUBMITTRANSFER` (texture uploads, and the swap blit in section 6) runs
a render on the 3D core with no TA and no geometry. The only object is the
background object, which covers the destination; its pixel program samples
the source and its end-of-tile program writes the destination. Each
transfer context (one for GLES uploads, one for EGL's swap) owns:

| buffer (GLES / EGL context) | holds |
|---|---|
| 0x37000000 / 0x37002000 (pixel-shader heap, 0x800) | the end-of-tile program: `phas`, 4 × `limm` r0-r3 = PBE words, 2 × `emitpix` |
| 0x3e081000 / 0x3e107000 (general heap, 0x4800) | the background-object block |
| 0x2e000000 / 0x2e003000 (PDS pixel heap, 0x1800) | a secondary PDS program, then the pixel-event program (5 data rows) whose DOUTU starts the end-of-tile program |
| 0x2e002000 / 0x2e005000 (PDS pixel heap, 0x400) | the background object's pixel PDS program: DOUTU (copy shader), DOUTI, DOUTT (the source's 4 texture words) |
| 0x37001018 / 0x370018e0 (pixel-shader heap, 0x10) | the copy shader: `phas.i #0, #0; mov o0, pa0 [end]` |

The command itself sits in the context's own CCB (kernel data, 0x104c8
bytes) at offset 0; `ui32SharedCmdCCBOffset` = 0xac points at the
`SGXMKIF_TRANSFERCMD_SHARED` the kernel fills (syncs), and the whole command
is 0x1c0 bytes (word +0xa4). libsrv_um `FUN_0002ca20` writes the client
words:

| word | upload / swap | field |
|---|---|---|
| 0 | 02008100 / 02000700 | `1<<25 \| (background block - base)>>4` |
| 1 | 0x200 | 0x300 with a depth/stencil surface (not seen) |
| 2 | 3f800000 | background depth 1.0 |
| 3 | 3 | |
| 4 | 0 | depth/stencil block - base (not seen) |
| 5 | 0x100 | |
| 6 | 3e000000 / 3e100000 | base: the background block & 0xfff00000 |
| 7, 8 | 80000000, 0x88 | as render details +0x10, +0x50 |
| 9, 10, 11 | 2e000020, 5, 1 | pixel-event PDS program, its data rows, its code size in 16-byte units |
| 12 | 0 | first tile: `x/32 << 16 \| y/16` |
| 13 | 00010003 / 0016001d | last tile: `(ceil(w/32)-1) << 16 \| (ceil(h/16)-1)` |
| 14, 15 | 0x80, 2e000000 | PDS heap base |

The background block is the render path's (STATE.md section 6) with the PDS
triple in words 0-2: secondary `rows<<26 | (addr-0x2e000000)>>4`,
`00030001`, primary `3<<26 | ...` → the pixel program above, and the
destination's w, h in words 18 and 20.

The source is a **strided** texture (T1 layout 6, format 0x0c BGRA8888,
which here only means "copy 4 bytes"). Its T0 carries the pitch, which
STATE.md had as a guess: **(pitch in texels − 1)** is split, the low 3 bits
in T0 b9-11 and the rest in b17-26:

| source | pitch | T0 |
|---|---|---|
| upload n12x6 | 12 | 00020600 |
| upload p64 | 64 | 000e0e00 |
| upload n100x30 | 100 | 00180600 |
| upload p256x256 | 256 | 003e0e00 |
| swap (back buffer) | 736 | 00b60e00 |

The destination is the PBE words in the end-of-tile program's LIMMs; the
sideband's **b13 (0x2000) writes the destination twiddled**. libGLESv2's
PBE builder (`FUN_0004e7a0`) ORs it in from a two-entry table indexed by
the surface's memory layout (`DAT_0006d5bc` = {0, 0x2000}); the twiddled
uploads have sideband 0x2200 and the linear (NPOT) ones and the swap 0x200.
The PBE writes the linear destination at its own pitch (104 for a 100-wide
NPOT texture), so the GPU does the padding.

`present.py --check CAPTURE` rebuilds every one of these words from the
surface sizes and addresses: command words 0-15, the background block, the
DOUTT words, the pixel PDS program and the end-of-tile program, 74 words per
transfer. All 74 match for every transfer in every capture (uploads 12×6,
100×30, 64×64, 128×32, 256×256; 12 swaps).

