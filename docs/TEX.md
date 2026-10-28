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
n5x7 (pitch 8) and n33x17 (pitch 40) match too.

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

## 4. Render to texture [C]

`fbrgba8888`: a 128×64 RGBA8888 texture as an FBO's colour attachment, the
probe triangle drawn into it, then a window quad sampling it. The DDK makes
it five TA kicks on the **one** HW render context, with **two** render
targets, each with its own RT data, render details and region-header pair
(the FBO's 0x300 bytes for 4 × 4 tiles):

| kick | RT | flags | what |
|---|---|---|---|
| 1 | FBO (RT data 0x3200abc0) | first | the FBO's clear + triangle, TA only |
| 2 | window (0x3200aac0) | first | the window's clear + quad, TA only |
| 3 | FBO | last, 3D 0x400 | renders the FBO (the quad's draw needs the texture) |
| 4 | window | — | more window TA (glFinish) |
| 5 | window | last, 3D 0x400 | renders the window |

The microkernel runs the 3D renders in kick order on the context, so the
FBO is rendered before the window samples it; no source sync is involved.
The FBO's end-of-tile program writes the texture directly:

- PBE words: clip max (127, 63), pitch 128 (word 2 `003f8000`), address =
  the texture (0xf0001000);
- sideband **0x2601**: PBE format **1 (RGBA8888, R,G,B,A bytes)**, b9, dither,
  **b13 = twiddled**. A power-of-two FBO texture is rendered straight into
  the twiddled layout of section 1, so sampling it needs nothing else;
- the window samples it as an ordinary texture: T0 `07fe0090` (clamp,
  nearest), T1 `0d07f03f` (RGBA8888, layout 0, 128×64), T2 0xf0001000.

The other colour formats (`fbrgb565`, `fbrgba4444`, `fbrgba5551`, same
scene) differ only in the PBE format code and the sampled T1 format; all are
written twiddled with dither:

| FBO texture | sideband | PBE format | sampled as T1 |
|---|---|---|---|
| RGBA8888 | 0x2601 | 1 | 0x0d |
| RGB565 | 0x2603 | 3 | 0x05 |
| RGBA5551 | 0x2604 | 4 | 0x04 |
| RGBA4444 | 0x2605 | 5 | 0x02 (texels come out ARGB4444, as uploads are stored) |

**Replayed natively on the EA3:** the replay's own FBO render (the texture
kept out of every delta, `--no-delta`) leaves the texture byte-identical to
the DDK's, and the window frame and its swap to the fb0 wrap match the DDK's
readback, 0 of 345600 pixels. **Authored:** with the FBO's clear colour
changed (the three background-vertex colour words at 0x3e51d00c/1c/2c) to
green, the frame is 0 of 345600 pixels off the prediction (the DDK's readback
with its 170094 FBO-clear pixels turned green).

## 5. Several renders per frame: the HW sync lists [C]

Each render target has a **HW sync list** in kernel data (the kick's
`hKernelHWSyncListMemInfo`; `SGXMKIF_HWDEVICE_SYNC_LIST`: access address,
count, then per sync object its write/read-ops-complete addresses and the
**pending values**). The client writes the pending values: absolute op
counts of the surfaces the render writes, which the 3D waits for.

Found the hard way: a first replay of `fbrgba8888` skipped the one transfer
the DDK had submitted for the FBO texture (its initial fill) before kick 1.
All five TA kicks completed (TA status counter 1 → 5, CCB read offset in
step), but neither 3D render ever started, because the FBO's sync list
asked for the texture's write op 1, which only that transfer completes. The
process then hit cleanup timeouts at exit, and the next client hung. With
`--xfers all` the same replay is pixel-exact. A driver must keep these
counts equal to what it actually submitted for each surface.

(`mkreplay.py --peek LABEL:VA:words` prints words after every event; that is
how the stall was located: `0x2d00303c` counts TA kicks, `0x2d003030` and
`0x2d003088` are the window's and the FBO's 3D completion status words.)

## 6. Presentation: eglSwapBuffers [C]

- **fb0 is wrapped, not mapped through the display class.** The DDK
  (LinuxFB WSEGL) mmaps `/dev/fb0` (720×480, 32 bpp BGRA, pitch 2944 = 736
  pixels) and passes that CPU address to `WRAP_EXT_MEMORY` (lands at
  0x3e10c000; `OPEN_DISPCLASS_DEVICE` is called but not used for the
  memory). A native x86_64 client does the same (`replay -F`: `mmap` with
  `MAP_SHARED | MAP_32BIT`, then `pvr_wrap`).
- **The swap is one `SGX_SUBMITTRANSFER`** on EGL's own transfer context:
  the blit of section 3 from the back buffer (source sync) to fb0 (destination
  sync), 720×480 BGRA8888, pitch 736 both sides, sideband 0x200.
- **Setup:** `SGX_REGISTER_HW_TRANSFER_CONTEXT` with a 0xb4-byte buffer (PD
  offset 8; returned 0x3000e060 for GLES's context and 0x3000e120 for EGL's),
  a CCB in kernel data (0x104c8 bytes + an 8-byte control), and the five small
  buffers of section 3. `libpvr` now has `pvr_register_transfer_ctx` and
  `pvr_submit_transfer`; `replay` has `regxfer` / `xfer`.

Proven on the EA3, each read back from the real `/dev/fb0` with `head -c`
after the replay:

| run | what | result |
|---|---|---|
| p64x `-F` | the p64 frame, its GPU texture upload and its swap, all native | fb0 = the DDK's readback, 0 of 345600 px (43200 px differed before) |
| blit `-F` | the swap's words all rebuilt by `present.py` as a different blit: a 512×384 crop of the back buffer from (96, 40) to fb0 (0, 0), fb0 pre-filled | 0 of 345600 px off the prediction |
| direct `-F` | no blit: the frame's own PBE address (end-of-tile LIMM r3) pointed at the fb0 wrap | fb0 = the DDK's readback, 0 px; back buffer untouched |
| direct15 `-F` | the same with PBE word 2 b15 cleared | **nothing written** to fb0 (no recovery) |

So a client can present either way: render into its own back buffer and blit
(what the DDK does, tear-free with two buffers), or render straight into the
fb0 wrap (zero copy, single buffered). PBE b15 must stay set for both; it is
not the "scanout" bit STATE.md had guessed.

## 7. Depth and stencil across renders (ZLS)

`fbd`: the FBO of section 4 with a 16-bit depth renderbuffer, depth test
LESS. The first FBO scene draws a green triangle at clip z 0 (depth 0.5);
the window samples the FBO; then a red triangle at clip z 0.5 (depth 0.75)
that covers the green one is drawn into the FBO **without a clear**, and the
window samples again. GL requires the green triangle to stay on top.

**The DDK loses the FBO's depth** [D]. In its own frame not one green pixel
is left: the red triangle covers all 24306 pixels of the green one, and the
FBO's depth buffer (0x3e581000) is still all zero when the re-entry scene is
kicked. The capture is ten kicks: FBO TA (1), window (2), FBO render (3),
window (4, 5), FBO re-entry TA (6), window (7), FBO re-entry render (8),
window (9, 10). The FBO's render details (one block, 0x3200ece0, for both
FBO renders) carry ZLS control `0x00140018`, and the re-entry `0x00144018`
(b14 added). Neither has the store bits.

Why, from libGLESv2 (`FUN_0002e2d0`): ZLS is only set up for **EGL window
surfaces**, and only when a frame is flushed mid-scene (kick flag 0x80). Then
it allocates a ZLS buffer of 5 bytes per padded pixel and sends, as
deferred render-details updates (section 1 of STATE.md):

| render details | value | from |
|---|---|---|
| +0x2c ZLS control | `surface bits & 0xf80ee007 \| 0x111000 \| (xtiles-1)<<3` | window d0: `001710b4` |
| +0x04 ZLS request base | buffer & 0xfff00000 | `3e500000` |
| +0x30, +0x34 | **depth** address - base (both words) | `00081000` |
| +0x38, +0x3c | b31 \| **stencil** address - base (both words), stencil after 4 bytes/pixel of depth | `801da000` |

STATE.md had +0x30/+0x34 as depth/stencil store and +0x38/+0x3c as
depth/stencil load; the code writes the depth address into both of the first
pair and the stencil address into both of the second.

**Store, proven on the EA3** [C]. Replaying `fbd` natively (control: 0 of
345600 pixels off the DDK's frame) with the FBO's ZLS control set to
`0x0017101c` (the DDK's FBO word plus b2, b12, b16, b17: the window path's
bits) makes the 3D store the FBO's depth: after the frame the depth buffer
holds float32 depths (`3f800000` = 1.0 where only the clear was, `3f400000` =
0.75 under the red triangle) even though GL asked for 16 bits. Single-bit
runs on the re-entry render, with the store on:

| ZLS control | result |
|---|---|
| `0x00140018` (DDK) | no store, no load |
| `0x00141018` (+ b12) or `0x00151018` (+ b12, b16) | no store |
| `0x0017101c` (+ b2, b12, b16, b17) | **depth stored**, float32 |
| + b14 (`0x0017501c`, the DDK's "re-entry" bit) | stored, **not loaded**: red still covers green |
| + b13 or + b15 | no visible change |
| + b19 | the stored depths are negated (`bf400000`): a format bit |
| + b0 or + b1, stencil words left at the DDK's `c1b00000` | **3D lockup**; once the recovery failed (`SGXInitialise failed (24)`) and the EA3 had to be power-cycled |
| + b0 (`0x0017501d`), stencil words +0x38/+0x3c = `00081000` (valid memory) | no lockup; a **depth-only render**: the re-entry triangle's depth is stored (0.75 in 3306 texels) but no colour is written. Still no load |

The packed depth-stencil FBO (`fbds`, DEPTH24_STENCIL8) gets ZLS control
`0x162018` (b13 and b17 on top of the depth-only word) and its stencil
address 0x8000 after the depth (+0x38 = `80089000`); its depth is lost the
same way (the re-entry triangle covers the first one).

So the lockups were most likely the DDK's placeholder stencil words: `c1b00000`
is b31 | b30 | 0x01b00000, an address past the end of the ZLS request base's
range. Bits that make the 3D touch the stencil address then fault. **Point
+0x38/+0x3c at valid memory before trying ZLS bits.**

**Load: open.** No combination made the re-entry render start from the
stored depth. TA-command 3D flag b8 (0x580, as the window depth capture has)
on the re-entry render locked up once with the placeholder stencil words; with
valid ones it runs normally and still doesn't load. What is known
for a driver: the store works (`0x0017101c`, depth at ZLS base + +0x30);
loading it back needs the remaining bit(s) of the ZLS control or a
render-details/3D-flag combination not found yet. Until then a driver can
avoid relying on ZLS loads: keep an FBO's scene in one render when it can,
or redraw the depth-only geometry.

## 8. Cube maps [D]

Six faces in GL order (+X, −X, +Y, −Y, +Z, −Z), one after the other, each a
complete mip chain in the layout of section 1. Face spacing:

| capture | chain | face stride |
|---|---|---|
| c8 (8×8, no mips) | 0x100 | 0x100 |
| c8m (8×8, 4 levels) | 0x154 | **0x154**: packed |
| c16m (16×16, 5 levels) | 0x554 | 0x800 |
| c32m (32×32, 6 levels) | 0x1554 | 0x1800 |

So faces are packed when level 0 is smaller than one 16×16 tile and aligned
otherwise; 0x400 and 0x800 alignment both fit these two points [G for the
exact rule]. Every texel of all four captures sits where `check.py --cube
--face-stride` predicts (384, 510, 2046 and 8190 texels).

## 9. What is proven on the EA3, pixel for pixel

| claim | run | result |
|---|---|---|
| encoder: RGBA8888 256×256 twiddled | tp256 replay with `tex.py`'s layout of a test image vs the DDK drawing that image (ti256) | 0 of 345600 px differ (vs the pattern frame: all differ) |
| encoder: RGB565 64×64 | tp565 / ti565 | 0 of 345600 |
| encoder: RGBA8888 100×30 linear (NPOT) | tpn100 / tin100 | 0 of 345600 |
| GPU texture upload (transfer queue) | p64 `--xfers all` | texture byte-identical to the DDK's |
| render to texture, RGBA8888 | fbrgba8888 `--xfers all` | frame and swap 0 px off; FBO texture byte-identical |
| render to texture, RGB565 (dithered) | fbrgb565 `--xfers all` | frame and swap 0 px off; FBO texture byte-identical |
| render to texture, authored | fbrgba8888 with the FBO clear changed | 0 px off the prediction (170094 px changed) |
| present via the swap blit | p64 `-F` into `/dev/fb0` | fb0 read back = the DDK's readback, 0 px |
| present via an authored blit | `present.py` words, crop to fb0 | 0 px off the prediction |
| present by rendering into fb0 | PBE address = the fb0 wrap | fb0 = the DDK's readback, 0 px |
| depth store across renders | fbd with ZLS control `0x0017101c` | depth buffer written (float32) |

## 10. GPU blits of our own: copies, mipmaps, sub-rectangles [C]

`tools/tex/blitest.c` runs the transfer queue on a context it builds itself:
no DDK library and no capture. Every word comes from `sgxtex`:
- the 0xb4-byte HW transfer context (`sgxtex_xctx_words`);
- the secondary PDS program, a DOUTU to a `nop [end]` shader;
- the pixel-event program (the same code `tools/gen` assembles);
- the emit, nop and copy shaders;
- the per-blit command, background block, pixel program and end-of-tile
  program.

Before any GPU run, those builders were checked against the 364 words the
DDK writes for its two transfer contexts in the p64 capture; all match.

**Setting a context up** (libsrv_um `SGXCreateTransferContext`, decompiled):

| buffer | heap, attribs, size |
|---|---|
| CCB | kernel data (3), 0x4f, 0x10000 + 0x4c8 |
| CCB control | kernel data (3), 0x4f, 8 bytes: write offset, read offset |
| USSE code | pixel shader (4), 0xd, 0x800 (end-of-tile, emit, nop, copy programs) |
| "ISP stream" | general (0), 0xd, 0x4800 (the background block) |
| PDS code | PDS pixel (6), 0xd, 0x1800 (secondary program at +0, pixel event at +0x20) |
| pixel PDS | PDS pixel (6), 0xd, 0x400 (the background pixel program) |

The context buffer is `{1, 0, PD (kernel), CCB, CCB control, 0 ...}` with the
PID at word 0x2a; the DDK leaves the rest uninitialised. Register it with PD
offset 8. Per blit, the client:
1. writes the 0x1c0-byte command at the CCB write offset:
   - words 0-15;
   - +0xa4 = 0x1c0;
   - +0xa8 = 8 for a twiddled destination, else 0;
   - +0x194 = 0x07c00000;
2. advances the control's write offset by 0x1c0;
3. submits `SGX_SUBMITTRANSFER` with `ui32SharedCmdCCBOffset` = command +
   0xac and the source and destination syncs;
4. waits on the destination sync.

Two traps:
- surfaces need sync objects, so allocate them without attribs b3
  (`PVRSRV_MEM_NO_SYNCOBJ`);
- a sync object can only be mmapped once per process.

**Sources and filters.**
- Twiddled (T1 layout 0) and linear (layout 3) sources take either filter.
  T0 is `001e0090` (clamp, no mips) plus `0x1400` for LINEAR.
- Strided sources (layout 6) keep their pitch where the minification filter
  bit would be, so they sample nearest.

**The texture coordinates.** The background object is the triangle (0,0),
(W,0), (0,H) over the whole destination. Block words 3-8 are the texture
coordinates (u, v) at those three vertices, and the 3D interpolates them; a
sweep of each word on the EA3 fits u = w3·(1-s-t) + w5·s + w7·t exactly (and
the same for v). The DDK always writes (0,0), (1,0), (0,1); words 18, 20 and
21 hold the vertices themselves, in a fixed-point form not decoded further.
A sub-rectangle copy (`sgxtex_blit_build_rect`) does three things:
- sets the coordinates so destination pixel (x, y) samples source texel
  (sx − dx + x, sy − dy + y), scaled;
- clips the PBE (words 0 and 1) to the destination rectangle;
- clips the tiles (command words 12-13) to the ones it touches.

T0 clamps, so no read leaves the source.

**Twiddled destinations clip only in y.** For a twiddled surface the PBE
takes the tile row length from the clip width. A 16-wide clip at x 8..23 of a
64-wide texture wrote all 256 pixels, each to where a 16-wide texture would
have it (256 of 256 fit that model). Full-width bands are exact. Any other
rectangle of a twiddled texture takes three blits:
1. read the texture back into a linear scratch surface;
2. copy the rectangle into the scratch;
3. upload the scratch.

`sgxtex_blit_build_rect` refuses the single-blit case (returns −1).

**Proven on the EA3** (`blitest`, `check_blit.py`): every destination byte is
predicted, with no SGX recovery in any run.

| case | what | result |
|---|---|---|
| up | 64×64 strided → twiddled (texture upload) | 4096 / 4096 |
| down | twiddled → linear (readback, glCopyTexImage from a texture) | 4096 / 4096 |
| mipn | 64×64 → 32×32, NEAREST | 1024 / 1024: texel (2x+1, 2y+1), as the DDK's glGenerateMipmap |
| mipl | 64×64 → 32×32, LINEAR | 1024 / 1024: **a real box filter**, `(((a+b+1)>>1) + ((c+d+1)>>1) + 1) >> 1` |
| up2x | 64×64 → 128×128, NEAREST | 16384 / 16384, pixel replication |
| npot | 100×30 linear (layout 3, pitch 104) → strided | 3000 / 3000 |
| to565 | RGBA8888 → RGB565 | 4096 / 4096: each channel truncated, no dither |
| rsub | (8,4) 24×20 → (30,40) of a linear surface | 4096 / 4096 |
| rscale | (0,0) 16×8 → (10,10) 32×16, 2× | 4096 / 4096 |
| rband, rband2 | full-width bands into a twiddled texture (tile-aligned or not) | 4096 / 4096 each |
| rcomp | (40,10) 16×16 → (8,24) of a twiddled texture in three blits | 4096 / 4096 |

So glCopyTexImage2D, glCopyTexSubImage2D, texture-to-texture copies,
glBlitFramebuffer-style scaled copies and box-filtered mipmap generation can
all run on the GPU.

## 11. Render targets the sampler reads directly [C]

The DDK never converts a render-to-texture target: it renders it in the
layout the sampler reads (section 4).
- **Power-of-two** sizes are rendered twiddled: PBE sideband b13, PBE pitch
  = the width, sampled as T1 layout 0.
- **Other sizes** are rendered linear, with the pitch rounded up to 8
  texels. A 100×60 RGBA8888 FBO gets PBE pitch 104 (`00338000`), sideband
  0x601 and a 0x6180-byte buffer, and is sampled as T1 `6d06303b` (layout 3,
  100×60) from the same address (`fbnpot`, retaken today).

`sgxtex_rt_surface` and `sgxtex_rt_pbe` give exactly the DDK's words for
both: 128×64 gives `003f8000`, sideband 0x2601 and T1 `0d07f03f`; 100×60
gives `00338000`, 0x601 and `6d06303b`. The same PBE words work for a TA/3D
render's end-of-tile program and for a transfer-queue blit, and render to
texture is replay-proven for RGBA8888 and RGB565 (section 4).

## 12. Tear-free presentation

**Back buffer plus swap blit** [C]. `blitest present`:
- wraps the real `/dev/fb0` the way the DDK's EGL does;
- fills a 720×480 BGRA8888 back buffer (pitch 736);
- blits it to fb0 on its own transfer context.

fb0 read back afterwards equals the back buffer, 0 of 345600 pixels. Submit
to completion took **1.3 ms**, so a frame rendered off-screen reaches the
display in about a tenth of a 60 Hz frame.

**Flipping is not available.** fb0 is `simplefb` over the fixed buffer the
CEFDK left the display controller scanning out (0x7fc00000, 720×480, pitch
2944). It has no virtual height and no pan, so FBIOPAN flips can't work.

**Vsync: not established.** The display controller (VDC, 8086:2e61,
BAR 0xdfb00000) has the i9xx layout:
- pipe A's timing reads 857×525 total and 720×480 active (`035902cf`,
  `020c01df`);
- pipe A's config (+0x70008) read `80000000` (enabled);
- its status (+0x70024) changed between reads (`342` → `302`).

That status is where an i9xx pipe keeps its vblank bit (b1, write 1 to
clear). Two cautions for anyone continuing:
- A read-only scan of the whole 1 MB BAR hung the EA3 outright, with no ping
  and a power cycle needed: holes in this window are not safe to read. Read
  only known registers.
- A few minutes after the cycle the whole VDC block started reading 0,
  timings included, while its PCI command and power state were unchanged.
  The cause is unknown; the reads before were read-only, and the test's one
  write (clearing the vblank bit) came after it had already read 0.

Until a vblank source is proven, swapping with the 1.3 ms blit is the
tear-light path, and rendering straight into the fb0 wrap (section 6) is the
zero-copy one.

## Open

- The ZLS **load** enable (section 7).
- The cut-over between CPU and transfer-queue uploads (between 256 and
  4096 texels).
- A vblank source for vsync; why the VDC registers went to 0 (section 12).
- The fixed-point format of the background block's vertex words 18, 20 and 21
  (sub-rectangles don't need them; section 10).
- PBE word 2 b15: it must be set for the PBE to write; what clearing it
  selects is unknown.
- Formats beyond the ones probed: compressed textures (PVRTC/ETC), float
  textures, the PBE codes 2 and 6+ in libGLESv2's table.
