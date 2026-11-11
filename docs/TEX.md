# SGX545 textures, render targets, uploads and presentation

What a GL client needs beyond one flat-shaded triangle, as the Cedarview DDK
1.7 does it on the CE5300 (SGX545 rev 1.0.14): where texels live in
memory, how they get there, how a render lands in a texture, how depth and
stencil survive between renders, and how a frame reaches the display.

## How each fact is established

Each fact below carries one of these tags, for what it rests on today:

- **hardware**: confirmed on a CE5300 board, by a replay or a native run
  whose output was checked (pixels, bytes or words read back; section 9
  lists the pixel-exact ones). **hardware, open driver** means the open
  driver in this repo (`mesa/src/gallium/drivers/sgx`,
  `mesa/src/gallium/winsys/sgx`) does it and runs on the board.
- **capture**: seen in the DDK's captured output: command streams, texture
  words, texel uploads, transfer commands, and the diff between two captures
  that differ in one probe setting.
- **unconfirmed**: stated but not backed by hardware or a capture today.
  Each one is listed in "Gaps" at the end with the test that would confirm
  it. Shipping code must not rely on an unconfirmed fact.
- **guess**: a name or meaning that fits but has no other backing.

A tag on a lead sentence covers what follows it, unless a fact carries its
own. Where a captured fact was later run on the board, both are given.

## Tools

| path | what |
|---|---|
| `tex.py` | the encoder: linear texels → GPU memory (twiddle, NPOT pitch, mip chain) |
| `rawmap.py` | reads a `PROBE_TEXPAT` texture back out of a capture: which texel sits at each offset |
| `check.py` | compares every texel of a capture with `tex.py`'s prediction |
| `tools/probe/probe.c` | new knobs: `PROBE_TEXPAT`, `PROBE_TEXFILE`, `PROBE_TEXLEVELS`, `PROBE_TEXCUBE`, `PROBE_FBO`, `PROBE_FBODS`, `PROBE_FBOREENTER`, `PROBE_QUAD`, `PROBE_FBOCLEAR`; for §13-15 `PROBE_TEX3D`, `PROBE_TEXBORDER`, `PROBE_TEXCOMPARE`, `PROBE_EXTS` |
| `tools/trace/pvrtrace.c` | now also snapshots all mapped memory before every `SGX_SUBMITTRANSFER` (reason `xfer`) |
| `tools/decode/mkreplay.py` | `--xfers` replays transfers and registers transfer contexts; `--write VA:file`; `--no-delta VA:size` |
| `tools/replay/replay.c` | `regxfer` and `xfer` ops; `-F` wraps the real `/dev/fb0` instead of private memory |

`PROBE_TEXPAT=1` makes every texel name itself: RGBA8888 texels are
`(x, y, level | face<<4, 0x80 | x>>8 | (y>>8)<<2)`, 16-bit texels are
`y*w + x`, 8-bit ones `(y*w + x) & 0xff`. A capture's texture buffer then
says where each texel went with no guessing.

## 1. Texel memory layout

All from `PROBE_TEXPAT` captures on 2026-10-28; `check.py` places every
texel of every capture where `tex.py` predicts (**capture**). The replays of
section 9 sample `tex.py`'s twiddled RGBA8888 and RGB565 layouts and its
linear RGBA8888 layout pixel for pixel (**hardware**).

### Texel formats

Every format at 16×16 (`f*` captures), all 256 texels of each in place
(**capture**):

| GL upload | T1 format | in memory |
|---|---|---|
| RGBA / UNSIGNED_BYTE | 0x0d | R, G, B, A bytes, as uploaded |
| RGB / UNSIGNED_BYTE | 0x0d | expanded to R, G, B, 0xff |
| RGB / UNSIGNED_SHORT_5_6_5 | 0x05 | the GL ushort |
| RGBA / UNSIGNED_SHORT_4_4_4_4 | 0x02 | **ARGB4444**: the ushort rotated right by 4 (A on top) |
| RGBA / UNSIGNED_SHORT_5_5_5_1 | 0x04 | **ARGB1555**: the ushort rotated right by 1 (A on top) |
| LUMINANCE_ALPHA | 0x1b | L, A bytes, as uploaded |
| LUMINANCE / ALPHA | 0x1a / 0x19 | the byte |

The open driver samples 0x0d, 0x05, 0x1b, 0x1a and 0x19 with these memory
layouts (**hardware, open driver**); RGB565 is also replayed (section 9).

The 4444/5551 rotations are the driver's conversion on upload (**capture**);
a client writing memory itself must apply them (`tex.convert`,
`sgxtex_convert16`). The render target's BGRA8888 is another format code
(T1 0x0c; **capture**, and **hardware, open driver**), so a texture and a
BGRA surface are not interchangeable without a swizzle.

### Twiddled: power-of-two sizes, T1 layout 0

**capture**; **hardware** for RGBA8888 256×256 and RGB565 64×64 (section
9); **hardware, open driver** for every power-of-two texture it samples.

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

### Linear: sizes that are not powers of two, T1 layout 3

**capture**; **hardware** for RGBA8888 100×30 (section 9); **hardware,
open driver** for every NPOT texture it samples.

Rows top to bottom, each padded to a multiple of **8 texels**: 12×6 has a
16-texel pitch (0x40 bytes), 100×30 a 104-texel pitch (0x1a0 bytes, the
buffer is exactly 104 × 30 × 4). Every texel of both matches. T1 layout 3
has no pitch field: the sampler derives it from the width the same way
(**hardware**: `tpn100` samples at pitch 104). n5x7 (pitch 8) and n33x17
(pitch 40) match too.

### Mipmaps

**capture**; the open driver lays its mip chains out this way (**hardware,
open driver**).

Level n+1 follows level n with no padding or alignment, down to 1×1 on both
axes; each level is laid out by its own size (twiddled when both sides are
powers of two). m16 (16×16, 5 levels, 0x554 bytes), m32x8 (32×8 → 16×4 →
8×2 → 4×1 → 2×1 → 1×1, 0x55c bytes) and m565 (RGB565, 0x2aa bytes) match
texel for texel. T2 is the base; nothing else in the texture words moves
with the level count (STATE.md).

**glGenerateMipmap runs on the GPU** (**capture**): one transfer per level
(mg64: six, before the frame's kicks), each sampling the previous level as a
twiddled texture (T0 `001e0090`: clamp, nearest, no mips) and writing the
next one twiddled. With nearest sampling the DDK's "filter" is decimation:
texel (x, y) of level n is texel (2x+1, 2y+1) of level n−1 (all 1365
generated texels of mg64; `blitest mipn` gives the same on the board,
**hardware**, section 10). A driver that wants box-filtered mips should make
its own.

## 2. Uploads: the CPU for small textures, the transfer queue otherwise

**capture**; the p64 GPU upload replays byte-identical to the DDK's
(**hardware**, section 9).

`glTexImage2D` does nothing visible until the first draw that uses the
texture. Then the DDK allocates the texture in the texture heap
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

(The DDK's `powervr.ini` says `DisableHWTextureUpload=1`; the board
evidently does not read that file. The cut-over between 256 and 4096
texels was not narrowed down.) Either way the bytes are the same, so a
client that twiddles on the CPU with `tex.py` never needs the transfer.

## 3. The transfer queue: a 3D render with only a background object

The buffers and words below are as captured (**capture**), and
`present.py --check` rebuilds all of them (end of this section). `blitest`
(section 10) builds the same words itself and runs them on the board, and
the swap replays of section 6 run the DDK's (**hardware**). The two
depth/stencil entries in the command table are the exception.

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
is 0x1c0 bytes (word +0xa4). The client words:

| word | upload / swap | field |
|---|---|---|
| 0 | 02008100 / 02000700 | `1<<25 \| (background block - base)>>4` |
| 1 | 0x200 | 0x300 with a depth/stencil surface (not seen; **unconfirmed**) |
| 2 | 3f800000 | background depth 1.0 |
| 3 | 3 | |
| 4 | 0 | depth/stencil block - base (not seen; **unconfirmed**) |
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
in T0 b9-11 and the rest in b17-26 (**capture**; **hardware**: the swap
replays and `blitest up`; **hardware, open driver**: it samples its render
targets this way):

| source | pitch | T0 |
|---|---|---|
| upload n12x6 | 12 | 00020600 |
| upload p64 | 64 | 000e0e00 |
| upload n100x30 | 100 | 00180600 |
| upload p256x256 | 256 | 003e0e00 |
| swap (back buffer) | 736 | 00b60e00 |

The destination is the PBE words in the end-of-tile program's LIMMs; the
sideband's **b13 (0x2000) writes the destination twiddled** (**hardware**:
`blitest up` and the render-to-texture replays of section 4; **hardware,
open driver**). It is set by the surface's memory layout: the twiddled
uploads have sideband 0x2200 and the linear (NPOT) ones and the swap 0x200
(**capture**). The PBE writes the linear destination at its own pitch (104
for a 100-wide NPOT texture), so the GPU does the padding (**capture**).

`present.py --check CAPTURE` rebuilds every one of these words from the
surface sizes and addresses: command words 0-15, the background block, the
DOUTT words, the pixel PDS program and the end-of-tile program, 74 words per
transfer. All 74 match for every transfer in every capture (uploads 12×6,
100×30, 64×64, 128×32, 256×256; 12 swaps).

## 4. Render to texture

The kicks and words below are as captured (**capture**). RGBA8888 and RGB565
are replayed on the board pixel for pixel (**hardware**, section 9).

`fbrgba8888`: a 128×64 RGBA8888 texture as an FBO's color attachment, the
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
FBO is rendered before the window samples it; no source sync is involved
(**hardware**: the replay renders this way, pixel-exact). The FBO's
end-of-tile program writes the texture directly:

- PBE words: clip max (127, 63), pitch 128 (word 2 `003f8000`), address =
  the texture (0xf0001000);
- sideband **0x2601**: PBE format **1 (RGBA8888, R,G,B,A bytes)**, b9, dither,
  **b13 = twiddled**. A power-of-two FBO texture is rendered straight into
  the twiddled layout of section 1, so sampling it needs nothing else;
- the window samples it as an ordinary texture: T0 `07fe0090` (clamp,
  nearest), T1 `0d07f03f` (RGBA8888, layout 0, 128×64), T2 0xf0001000.

The other color formats (`fbrgb565`, `fbrgba4444`, `fbrgba5551`, same
scene) differ only in the PBE format code and the sampled T1 format; all are
written twiddled with dither. RGBA8888 and RGB565 are replayed (**hardware**);
RGBA5551 and RGBA4444 are **capture** only. The open driver renders
RGBA8888 (and BGRA8888, PBE format 0) into textures this way (**hardware,
open driver**).

| FBO texture | sideband | PBE format | sampled as T1 |
|---|---|---|---|
| RGBA8888 | 0x2601 | 1 | 0x0d |
| RGB565 | 0x2603 | 3 | 0x05 |
| RGBA5551 | 0x2604 | 4 | 0x04 |
| RGBA4444 | 0x2605 | 5 | 0x02 (texels come out ARGB4444, as uploads are stored) |

**Replayed natively on a CE5300 board** (**hardware**): the replay's own FBO
render (the texture kept out of every delta, `--no-delta`) leaves the
texture byte-identical to the DDK's, and the window frame and its swap to
the fb0 wrap match the DDK's readback, 0 of 345600 pixels. **Authored:**
with the FBO's clear color changed (the three background-vertex color words
at 0x3e51d00c/1c/2c) to green, the frame is 0 of 345600 pixels off the
prediction (the DDK's readback with its 170094 FBO-clear pixels turned
green).

## 5. Several renders per frame: the HW sync lists

**hardware** (the `fbrgba8888` replay below).

Each render target has a **HW sync list** in kernel data (the kick's
`hKernelHWSyncListMemInfo`; `SGXMKIF_HWDEVICE_SYNC_LIST`: access address,
count, then per sync object its write/read-ops-complete addresses and the
**pending values**). The client writes the pending values: absolute op
counts of the surfaces the render writes, which the 3D waits for.

On the board: a first replay of `fbrgba8888` skipped the one transfer
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

## 6. Presentation: eglSwapBuffers

What the DDK does is from its trace (**capture**); each part is run
natively on the board (**hardware**, table below).

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

Proven on a CE5300 board, each read back from the real `/dev/fb0` with `head -c`
after the replay:

| run | what | result |
|---|---|---|
| p64x `-F` | the p64 frame, its GPU texture upload and its swap, all native | fb0 = the DDK's readback, 0 of 345600 px (43200 px differed before) |
| blit `-F` | the swap's words all rebuilt by `present.py` as a different blit: a 512×384 crop of the back buffer from (96, 40) to fb0 (0, 0), fb0 pre-filled | 0 of 345600 px off the prediction |
| direct `-F` | no blit: the frame's own PBE address (end-of-tile LIMM r3) pointed at the fb0 wrap | fb0 = the DDK's readback, 0 px; back buffer untouched |
| direct15 `-F` | the same with PBE word 2 b15 cleared | **nothing written** to fb0 (no recovery) |

So a client can present either way: render into its own back buffer and blit
(what the DDK does, tear-free with two buffers: **unconfirmed**, see section
12), or render straight into the fb0 wrap (zero copy, single buffered). PBE
b15 must stay set for both; it is not the "scanout" bit STATE.md had guessed
(**hardware**).

## 7. Depth and stencil across renders (ZLS)

`fbd`: the FBO of section 4 with a 16-bit depth renderbuffer, depth test
LESS. The first FBO scene draws a green triangle at clip z 0 (depth 0.5);
the window samples the FBO; then a red triangle at clip z 0.5 (depth 0.75)
that covers the green one is drawn into the FBO **without a clear**, and the
window samples again. GL requires the green triangle to stay on top.

**The DDK loses the FBO's depth** (**capture**). In its own frame not one
green pixel is left: the red triangle covers all 24306 pixels of the green
one, and the FBO's depth buffer (0x3e581000) is still all zero when the
re-entry scene is kicked. The capture is ten kicks: FBO TA (1), window (2),
FBO render (3), window (4, 5), FBO re-entry TA (6), window (7), FBO re-entry
render (8), window (9, 10). The FBO's render details (one block, 0x3200ece0,
for both FBO renders) carry ZLS control `0x00140018`, and the re-entry
`0x00144018` (b14 added). Neither has the store bits.

In the captures the DDK sets ZLS up only for **EGL window surfaces**: the
`fbd` and `fbds` FBO renders have no store bits, and its window renders with
a depth buffer have them (**capture**). That it does so only when a frame is
flushed mid-scene (kick flag 0x80) is **unconfirmed**; the split-window
captures below store depth whether or not the frame is split. Its ZLS
buffer is 5 bytes per padded pixel (**unconfirmed** for the DDK; the open
driver allocates that and runs on the board, **hardware, open driver**). It
sends, as deferred render-details updates (section 1 of STATE.md;
**capture**):

| render details | value | from |
|---|---|---|
| +0x2c ZLS control | `surface bits & 0xf80ee007 \| 0x111000 \| (xtiles-1)<<3` (the `surface bits & 0xf80ee007` part **unconfirmed**) | window d0: `001710b4` |
| +0x04 ZLS request base | buffer & 0xfff00000 | `3e500000` |
| +0x30, +0x34 | **depth** address - base (both words) | `00081000` |
| +0x38, +0x3c | b31 \| **stencil** address - base (both words), stencil after 4 bytes/pixel of depth | `801da000` |

STATE.md had +0x30/+0x34 as depth/stencil store and +0x38/+0x3c as
depth/stencil load; the depth address goes into both words of the first
pair and the stencil address into both of the second (**capture**;
**hardware, open driver**).

**Store, proven on a CE5300 board** (**hardware**). Replaying `fbd` natively
(control: 0 of 345600 pixels off the DDK's frame) with the FBO's ZLS control
set to `0x0017101c` (the DDK's FBO word plus b2, b12, b16, b17: the window
path's bits) makes the 3D store the FBO's depth: after the frame the depth
buffer holds float32 depths (`3f800000` = 1.0 where only the clear was,
`3f400000` = 0.75 under the red triangle) even though GL asked for 16 bits.
Single-bit runs on the re-entry render, with the store on:

| ZLS control | result |
|---|---|
| `0x00140018` (DDK) | no store, no load |
| `0x00141018` (+ b12) or `0x00151018` (+ b12, b16) | no store |
| `0x0017101c` (+ b2, b12, b16, b17) | **depth stored**, float32 |
| + b14 (`0x0017501c`, the DDK's "re-entry" bit) | stored, **not loaded**: red still covers green |
| + b13 or + b15 | no visible change |
| + b19 | the stored depths are negated (`bf400000`): a format bit (name **guess**) |
| + b0 or + b1, stencil words left at the DDK's `c1b00000` | **3D lockup**; once the recovery failed (`SGXInitialise failed (24)`) and the board had to be power-cycled |
| + b0 (`0x0017501d`), stencil words +0x38/+0x3c = `00081000` (valid memory) | no lockup; a **depth-only render**: the re-entry triangle's depth is stored (0.75 in 3306 texels) but no color is written. Still no load |

The packed depth-stencil FBO (`fbds`, DEPTH24_STENCIL8) gets ZLS control
`0x162018` (b13 and b17 on top of the depth-only word) and its stencil
address 0x8000 after the depth (+0x38 = `80089000`); its depth is lost the
same way (the re-entry triangle covers the first one) (**capture**).

So the lockups were most likely the DDK's placeholder stencil words: `c1b00000`
is b31 | b30 | 0x01b00000, an address past the end of the ZLS request base's
range. Bits that make the 3D touch the stencil address then fault. With
valid stencil words b0 ran without a lockup (above), and so does the load
below, which sets b1 (**hardware**). **Point +0x38/+0x3c at valid memory
before trying ZLS bits.**

**Load, from a split window frame** (**capture**). `zls-*` (2026-11-08) is a
720×480 window with a 16-bit depth buffer, depth test LESS, clear depth 1.
Draw 1 is a green triangle at depth 0.5. Draw 2 is the same triangle in red
at depth 0.75. Something runs between the two draws (probe
`PROBE_FLUSHMID`). The DDK's frame is green in every variant: it keeps the
depth.

| between the draws | kicks (TA flags / 3D flags) | ZLS control of each render |
|---|---|---|
| nothing | 0x71 / 0, then 0x62 / 0x580 | `001710b4` (one render) |
| glFlush, glFinish | 0x71 / 0, 0x60 / 0, then 0x62 / 0x580 | `001710b4` (one render) |
| 1-pixel glReadPixels | 0x73 / 0x580, then 0x61 / 0x180, then 0x62 / 0x580 | `001710b4`, then `001770b6` |

- **glFlush and glFinish don't split the render.** They kick the TA only
  (3D flags 0) and the scene stays open, so a single render at the end sees
  every draw's depth.
- **Only a readback splits it.** The first render's ZLS control (`001710b4`)
  stores depth. The second render's (`001770b6`, a deferred update at its
  last kick) loads and stores it. The TA-only kick between them carries
  `001170b2`, load only, in its render details.
- **The bits.** The base is `0x111000 | (xtiles-1)<<3`. The open driver
  uses these words and they work on the board (below; **hardware, open
  driver**).
  - **Store** is b2 | b17 | b18 (`0x060004`). This matches the store proven
    above.
  - **Load** is b1 | b13 | b14 (`0x006002`). The section's earlier tries had
    b1 only with the placeholder stencil words, which locked up; they had b14
    without b1, which didn't load.
- **The buffer and its addresses.**
  - ZLS request base +0x04 = `3e500000`.
  - Depth: +0x30 = +0x34 = `00081000`.
  - Stencil: +0x38 = +0x3c = `801da000`.
  - Stencil starts 0x159000 bytes after depth: 736 × 480 × 4 bytes, so one
    float32 per pixel of the tile-padded width (23 tiles of 32 px). One
    stencil byte per pixel follows (**unconfirmed**).
  - The same addresses serve the store and the load.
- **Every DDK window render with a depth buffer stores depth** (`001710b4` at
  the terminating kick), split or not.
- **Proven natively on a CE5300 board** (**hardware**, 2026-11-08, an open
  Mesa driver on the DDK microkernel). A 96×64 surface (xt 3) is split
  mid-frame by glFlush, glFinish or a 1-pixel readback. The second draw is
  farther away, depth LESS. The result is green in all 12 cases, with 24/0,
  24/8 and 16/0 depth/stencil configs, and there were no lockups.
  - Store with `00171014`, then load + store with `00177016`.
  - +0x04 = buffer & 0xfff00000; +0x30/+0x34 = the depth offset; +0x38/+0x3c
    = b31 | the stencil offset. The stencil offset is the depth offset plus
    padded width × padded height × 4, with the height padded to 16.
  - **The ZLS buffer must be GPU read-write** (`PVRSRV_MEM_READ | WRITE`).
    Read-only memory made the store fault inside the buffer (BIF_FAULT), and
    a 3D recovery followed.
  - **TA command 3D flags b7 | b8 (0x180)** are needed on every render that
    uses ZLS (the DDK sends 0x580 = 0x400 | 0x180). Without them nothing is
    stored. Which of the two bits does it is not isolated.

## 8. Cube maps

**capture**; the open driver lays its cube faces out this way, the 0x800
alignment included (**hardware, open driver**).

Six faces in GL order (+X, −X, +Y, −Y, +Z, −Z), one after the other, each a
complete mip chain in the layout of section 1. Face spacing:

| capture | chain | face stride |
|---|---|---|
| c8 (8×8, no mips) | 0x100 | 0x100 |
| c8m (8×8, 4 levels) | 0x154 | **0x154**: packed |
| c16m (16×16, 5 levels) | 0x554 | 0x800 |
| c32m (32×32, 6 levels) | 0x1554 | 0x1800 |

So faces are packed when level 0 is smaller than one 16×16 tile and aligned
otherwise. Every texel of all four captures sits where `check.py --cube
--face-stride` predicts (384, 510, 2046 and 8190 texels).

**The alignment is 0x800 bytes in every format** (**capture**, 2026-11-02:
probe cubes with full chains and NEAREST_MIPMAP_NEAREST, the DDK's texture
buffer divided by six):

| format | 8×8 | 16×16 | 32×32 | 64×64 |
|---|---|---|---|---|
| L8 | 0x55 (packed) | 0x800 | 0x800 | 0x1800 |
| LA88, RGBA4444 | 0xaa (packed) | 0x800 | 0x1000 | 0x3000 |
| RGBA8888 | 0x154 (packed) | 0x800 | 0x1800 | 0x5800 |

**Mip levels sampled without mipmapping.** The sampler then spaces the faces
by level 0 alone (T0 b18 set): with our chain-spaced faces every face but +X
came from the wrong place, and no T0 bit tried (b17, b19, b20, b21-26)
changes that (**hardware**). The DDK keeps such a cube in different storage
(**capture**): a 16×16 RGBA cube with five levels uploaded and a MIN filter
of LINEAR samples a 0x1800 buffer (six 0x400 faces, level 0 only), the same
cube with NEAREST_MIPMAP_NEAREST a 0x3000 one. The driver keeps a copy of
the level-0 faces for that case (`res->cube0`; **hardware, open driver**).

**NPOT faces** (**capture**). The DDK samples a 12×12, 15×15 or 20×20 cube
as one linear face (T1 layout 3, a buffer of one face's bytes at the 8-texel
pitch) and fails every NPOT cube test in dEQP-GLES2 (**hardware**); layout 2
has no NPOT form that we know of.

## 9. What is proven on hardware, pixel for pixel

Every row is **hardware**.

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
| replay control, `bbase` | rp-bctl, the capture replayed | 0 of 345600 px vs the DDK's readback |
| wrap code 3 = mirror once | b33 / b32 vs the control | mirrored below 0, clamped above 1, per axis |

## 10. GPU blits of our own: copies, mipmaps, sub-rectangles

**hardware** (`blitest` on the board, checked by `check_blit.py`), except
where a fact carries its own tag.

`tools/tex/blitest.c` runs the transfer queue on a context it builds itself:
no DDK library and no capture. Every word comes from `sgxtex`:
- the 0xb4-byte HW transfer context (`sgxtex_xctx_words`);
- the secondary PDS program, a DOUTU to a `nop [end]` shader;
- the pixel-event program (the same code `tools/gen` assembles);
- the emit, nop and copy shaders;
- the per-blit command, background block, pixel program and end-of-tile
  program.

Before any GPU run, those builders were checked against the 364 words the
DDK writes for its two transfer contexts in the p64 capture; all match
(**capture**).

**Setting a context up:**

| buffer | heap, attribs, size |
|---|---|
| CCB | kernel data (3), 0x4f, 0x10000 + 0x4c8 |
| CCB control | kernel data (3), 0x4f, 8 bytes: write offset, read offset |
| USSE code | pixel shader (4), 0xd, 0x800 (end-of-tile, emit, nop, copy programs) |
| "ISP stream" | general (0), 0xd, 0x4800 (the background block) |
| PDS code | PDS pixel (6), 0xd, 0x1800 (secondary program at +0, pixel event at +0x20) |
| pixel PDS | PDS pixel (6), 0xd, 0x400 (the background pixel program) |

The context buffer is `{1, 0, PD (kernel), CCB, CCB control, 0 ...}` with the
PID at word 0x2a; the DDK leaves the rest uninitialized (**unconfirmed**).
Register it with PD offset 8. Per blit, the client:
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
sweep of each word on a CE5300 board fits u = w3·(1-s-t) + w5·s + w7·t exactly (and
the same for v). The DDK always writes (0,0), (1,0), (0,1); words 18, 20 and
21 hold the vertices themselves, in a fixed-point form not decoded further
(**capture**).
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

**Proven on a CE5300 board** (`blitest`, `check_blit.py`): every destination byte is
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

## 11. Render targets the sampler reads directly

The DDK never converts a render-to-texture target: it renders it in the
layout the sampler reads (section 4; **capture**). The open driver renders
its texture targets the same two ways (**hardware, open driver**).
- **Power-of-two** sizes are rendered twiddled: PBE sideband b13, PBE pitch
  = the width, sampled as T1 layout 0.
- **Other sizes** are rendered linear, with the pitch rounded up to 8
  texels. A 100×60 RGBA8888 FBO gets PBE pitch 104 (`00338000`), sideband
  0x601 and a 0x6180-byte buffer, and is sampled as T1 `6d06303b` (layout 3,
  100×60) from the same address (`fbnpot`).

`sgxtex_rt_surface` and `sgxtex_rt_pbe` give exactly the DDK's words for
both (**capture**): 128×64 gives `003f8000`, sideband 0x2601 and T1
`0d07f03f`; 100×60 gives `00338000`, 0x601 and `6d06303b`. The same PBE
words work for a TA/3D render's end-of-tile program and for a transfer-queue
blit, and render to texture is replay-proven for RGBA8888 and RGB565
(section 4; **hardware**).

## 12. Tear-free presentation

**Back buffer plus swap blit** (**hardware**). `blitest present`:
- wraps the real `/dev/fb0` the way the DDK's EGL does;
- fills a 720×480 BGRA8888 back buffer (pitch 736);
- blits it to fb0 on its own transfer context.

fb0 read back afterwards equals the back buffer, 0 of 345600 pixels. Submit
to completion took **1.3 ms**, so a frame rendered off-screen reaches the
display in about a tenth of a 60 Hz frame.

**Flipping is not available** (**hardware**). fb0 is `simplefb` over the
fixed buffer the CEFDK left the display controller scanning out (0x7fc00000,
720×480, pitch 2944). It has no virtual height and no pan, so FBIOPAN flips
can't work.

**Vsync: not established.** The display controller (VDC, 8086:2e61,
BAR 0xdfb00000) has the i9xx layout (**hardware**, register reads):
- pipe A's timing reads 857×525 total and 720×480 active (`035902cf`,
  `020c01df`);
- pipe A's config (+0x70008) read `80000000` (enabled);
- its status (+0x70024) changed between reads (`342` → `302`).

That status is where an i9xx pipe keeps its vblank bit (b1, write 1 to
clear; **unconfirmed** on this controller). Two cautions for anyone
continuing (**hardware**):
- A read-only scan of the whole 1 MB BAR hung the board outright, with no ping
  and a power cycle needed: holes in this window are not safe to read. Read
  only known registers.
- A few minutes after the cycle the whole VDC block started reading 0,
  timings included, while its PCI command and power state were unchanged.
  The cause is unknown; the reads before were read-only, and the test's one
  write (clearing the vblank bit) came after it had already read 0. The
  picture stayed up throughout (the user saw Minecraft on the screen
  afterwards): scanout continued while the registers read 0, so something
  gates register access, not the pipe.

Until a vblank source is proven, swapping with the 1.3 ms blit is the
tear-light path, and rendering straight into the fb0 wrap (section 6) is the
zero-copy one.

## 13. Border color (CLAMP_TO_BORDER, GL_CLAMP)

**Why ours fails for every format** (**hardware, open driver**: piglit
`texwrap formats bordercolor`). `wrap()` in `sgx_texture.c` gives CLAMP,
CLAMP_TO_EDGE and CLAMP_TO_BORDER the same T0 code, 2, and no texture word
carries a color. Every read past the edge returns the edge texel, so the
result doesn't depend on the format. GL_CLAMP adds nothing of its own:
st/mesa rewrites it when the screen's `caps.gl_clamp` is false, as it is
here. It saturates the coordinate and samples with CLAMP_TO_BORDER under
LINEAR filtering, or CLAMP_TO_EDGE under NEAREST (`_mesa_lower_gl_clamp`).
The driver only has to do CLAMP_TO_BORDER.

**The texture unit has no border mode in its wrap codes, but code 3 is
mirror once** (**hardware**: replays of `bbase` on a CE5300 board,
2026-11-11, T0's wrap fields changed; the control replay matches the
capture pixel for pixel).

- T0's wrap fields are 3 bits each: S in b6-8, T in b3-5 (§5 of STATE.md).
  The DDK's ES2 uses codes 0, 1 and 2 only (**capture**).
- **Code 3 is GL_MIRROR_CLAMP_TO_EDGE:** for a coordinate below 0 the
  texture is mirrored once, and above 1 it clamps to the edge texel. With
  S 3 and T 2 only the S axis mirrors, so the fields are per axis.
- **Codes 4, 6 and 7 in S return (0, 0, 0, 0) for every sample**, inside
  [0, 1] as well, whatever T is: one dump for all six of 44, 66, 77, 42, 62
  and 72.
- **Code 5 isn't a border either.** With both axes 5, every sample with
  either coordinate inside [0, 1] returns 0; where both are outside it
  returns the same corner texel as clamp to edge. With S 5 and T 2,
  everything is 0 except where s is outside [0, 1] and t is below 0, which
  reads the clamped texel at half its value and half its alpha.
- No code returns a constant outside [0, 1] and the texels inside, so no
  wrap code is a border mode, and the shader lowering below stays. 4-7 are
  likely reserved (**guess**).
- Mesa's driver for the next PowerVR generation
  (`src/imagination/csbgen/rogue/texstate.xml`) numbers its address modes
  the same way: 0 repeat, 1 mirror, 2 clamp to edge. It adds 3 mirror once
  then clamp, 4 clamp to border and 5 GL_CLAMP. Its border color comes from
  a table in memory, indexed by a sampler-word field, with the table's base
  in a register. That the SGX545 numbers codes 3-5 the same way is a
  **guess** from the three that match. The GPL kernel's `sgx545defs.h` names
  no texture-unit register at all, so it can't show a table base either way.
- The DDK's desktop GL lists `ARB_texture_border_clamp` (PIGLIT.md, "The
  DDK's desktop GL"), so that driver does border clamp, in the texture unit
  or in the shader. It needs X, so it gives no captures.
- The DDK's ES2 has no border clamp: its extension string lists neither
  `EXT_texture_border_clamp` nor `OES_texture_border_clamp` (**capture**,
  `PROBE_EXTS=1`, 2026-11-11). The ES2 captures can't show a border word.

**Tests, in order** (Gaps, §13):

1. `PROBE_EXTS=1` on the DDK's ES2 and ES1. Done: no border extension (above).
2. A DDK capture of the 8×8 checker sampled over [-1, 2], as the base for
   replays. Done: `bbase` (2026-11-11), a screen-covering triangle with
   `v = p.xy * 1.5 + 0.5` and CLAMP_TO_EDGE on both axes. Its T0 is
   `07fe0090`, in the pixel PDS program's data at `0x2e018038` (T1 to T3
   follow: `0d007007 f0001000 00800800`). The texture fills the middle third
   of the screen in each axis.
3. Replays of it with both wrap fields set to 3, 4, 5, 6 and 7, then S
   alone, each read back. A border mode shows a constant outside [0, 1]. A
   mirror-once mode shows the texture mirrored once, then clamped. Done:
   code 3 mirrors once, and no code is a border (above).
4. If one code had shown a constant: which constant, and the T0 and T3 bit
   scans for a field that moves it. Not needed, since none did.
5. No code gives a border, so the border is shader work (below).

**Lowering, if the texture unit has no border color.** Per sample, in the
fragment shader, choose the border color (a uniform per sampler) for any
coordinate outside [0, 1] on a CLAMP_TO_BORDER axis. With LINEAR filtering
the choice is per tap: the four-tap path the sRGB filter already uses
(`lower_srgb_filter`, UPSTREAM-READINESS M10) samples the 2×2 taps with
NEAREST, so each tap that falls outside takes the border before the weights
mix. The border color is a GL float color. For integer, sRGB and luminance
formats it goes through the same conversion and swizzle as the texel.

The driver does this since mesa-gallium c01cac3 (`lower_tex_filter`, the
sRGB filter generalized; the border colors are driver constants at k 960 +
4 per sampler), for a CLAMP_TO_BORDER texture sampled at one level: one
tap for NEAREST, four for LINEAR, two in 1D. Mipmapped minification still
clamps to the edge. It hasn't run on the board yet (**unconfirmed**). A
native border mode, if the replays find one, replaces it.

## 14. Volume (3D) textures

- **The SGX545 has them.** The GPL kernel's `sgxfeaturedefs.h` gives the 545
  `SGX_FEATURE_VOLUME_TEXTURES`, and it is the only core in that header that
  has it (docs/porting/cores/sgx530.md lists the 530's features against
  it). The DDK's desktop GL lists `EXT_texture3D`. The DDK's ES2 doesn't
  list `OES_texture_3D` (**capture**, `PROBE_EXTS=1`, 2026-11-11), so no ES2
  capture can show a volume; the step 3 tests below are the evidence.
- **The open driver has none:** `caps->max_texture_3d_levels = 0`, and
  `sgx_resource.c` gives a PIPE_TEXTURE_3D no device copy. Mesa still
  advertises GL 2.1, since its version check doesn't look at 3D levels below
  4.1. GL 1.2 makes them core, and 35 piglit tests fail.
- **The sample instruction.** `smp` dimension 2 is "3D". The vendor
  assembler accepts it, and the DDK's compiler uses it for samplerCube (ISA.md,
  hw 1c: **oracle**, corpus). A volume sample is likely dim 2 as well, with
  the texture words telling volume from cube (**guess**). A plain
  `textureCube(s, v)` is a non-dependent sample: the USP sample block gives
  it coordinate dimension 3 (ISA.md, "USP texture-sample blocks": capture,
  corpus). A plain `texture3D` is likely the same form (**guess**). Which
  DOUTI bits carry the dimension isn't known: PDS.md lists only the 2-D
  (0x200) and projected (0x400) bits.
- **The texture words and the layout** (**hardware**: open-stack black-box
  tests on a CE5300 board, 2026-11-11, `tools/isa/hwt_vol*.py`, a 4×4×D
  RGBA8888 volume written by hand, a dependent `smp.3d` at (s, t, r)):
  - **The depth is T3's low bits, as depth - 1.** 1 gives 2 slices, 2 gives
    3 (r splits into thirds), 3 gives 4, and so on to 7 for 8; single bits
    b3-b6 fit 9, 17, 33 and 65 slices. With 0, the value in every 2D word,
    r isn't read. The field is at least b0-6 wide; b7-10 also move the
    address and are likely the same field (**unconfirmed**).
  - **T1 layout 1 with a depth is a twiddled volume**, Morton order with y
    first: index bits y0, x0, z0, y1, x1, z1, then the remaining z bits (z2
    at bit 6, z3 at bit 7 for 4×4×D). Checked at (2,0,0) = 16, (0,2,0) = 8,
    (1,1,1) = 7, (3,3,3) = 63, (3,0,0) = 18, (0,3,0) = 9, (0,0,3) = 36, and
    slices 0-7 of an 8-deep one. Sizes where x and y aren't 4 are untested.
  - Layout 1 with depth 0 reads linear rows. Layouts 0 and 7 are 2D
    twiddled and never read r; layout 4 is strided; layout 5 returns 0.
  - Wrap R is still open. T0 b0-2, a 3-bit field just below wrap T (b3-5)
    and wrap S (b6-8), is 0 in every 2D capture and is the candidate
    (**guess**).

**Tests** (Gaps, §14):

1. `PROBE_EXTS=1`: whether the DDK's ES2 lists `OES_texture_3D`. Done: it
   doesn't, so step 2 can't run and step 3 is the path.
2. If it does, DDK captures sampling with `texture3D`:
   - `PROBE_TEX3D=4 PROBE_TEXSIZE="4 4"`, RGBA8888, `PROBE_TEXPAT=1`;
   - 8×8×8 with `PROBE_TEXLEVELS=4`;
   - 8×8×2 and 2×2×8 (which axis the layout favors);
   - an NPOT one, 6×5×3;
   - wrap R repeat against clamp (`PROBE_TEXWRAP="0x2901 0x2901 0x812F"`).

   Each gives the T words, the sample form and the texel layout.
3. If it doesn't, black-box tests with the open stack: a dim-2 `smp` on a
   texture whose T1 layout is 1, 4, 5 or 7, over a `PROBE_TEXPAT`-style
   volume written to memory by hand. Read back which texel each (s, t, r)
   returns, with T0 b0-2 and T3 varied one field at a time. Done for the
   layout and the depth (above); wrap R, mipmaps and filtering across r are
   still to run.

**What the driver needs:** a PIPE_TEXTURE_3D upload in T1 layout 1, twiddled
in the order above, with T3's low bits = depth - 1, and a dependent `smp.3d`.
Wrap R, mipmapped volumes, NPOT depths and the non-dependent (DOUTI) form
come from the remaining tests (Gaps, §14).

## 15. Depth textures and shadow compare

- **Why 96 piglit tests crash** (**hardware, open driver**, PIGLIT.md):
  - Mesa always exposes `ARB_depth_texture` (GL 1.4 core).
  - `sgx_tex_format` has no depth code, so `is_format_supported` refuses
    every depth format for sampling.
  - A `GL_DEPTH_COMPONENT` texture gets no format, and the test crashes
    later on.
- **The DDK's ES2 samples depth by converting it to float on upload**
  (**capture**: `dt16`, `dt32`, `dt24s8`, 2026-11-11, a 16×16 texture with
  `PROBE_TEXPAT=1` sampled with `texture2D`):
  - All three formats get the same texture words: T0 `00060e00`, T1
    `d200f00f`, T2 the texture, T3 `00800800`. Only the texels differ.
  - T1 is layout 6 (strided) and format **0x12**, a code no other capture
    shows. T0 holds the stride: (pitch - 1) = 31, so rows are 32 texels
    (0x80 bytes) apart for a 16-texel row.
  - Every texel is a 32-bit float, the depth normalized to [0, 1]: depth16
    texel 1 is `0x37800080` (1/65535), and depth32 and d24s8 hold their
    value / (2^32 - 1) and / (2^24 - 1). The stencil byte of d24s8 is
    dropped.
  - The DDK's shader returns (d, d, d, 1): the centre texel (136 / 65535
    for depth16, about 8e-6 for the others) reads back as (1, 1, 1, 255) and
    (0, 0, 0, 255). The sampler itself writes the depth to r0 only and
    leaves r1-r3 alone (**hardware**, `hwt_pcf*.py`, 2026-11-11), so the
    replication is a swizzle in the shader.
  - So 0x12 is a one-channel F32 format (**guess** for formats other than
    depth: no `lf32` capture shows which code a float luminance texture
    gets). The TPU doesn't filter it: with mag LINEAR (T0 b12) a sample
    still returns the nearest texel, plain or compared (**hardware**).
  - The DDK's ES2 has no `EXT_shadow_samplers`, so its captures show no
    compare at all.
- **Shadow compare in the sampler** (**hardware**: open-stack black-box
  tests on a CE5300 board, 2026-11-11, `tools/isa/hwt_pcf*.py`, the DDK's
  depth16 words with every texel 0.5, a dependent `smp.2d.pcf r0, r8, r4`):
  - `smp` has two PCF bits (ISA.md, hw 1c: **oracle**). h5 (`.pcf`) is
    accepted with F32 coordinates only, and h6 (`.pcf16`) with F16
    coordinates, never with C10.
  - `.pcf` compares against a **reference in the third coordinate** (r10
    for (s, t, ref) in r8-r10). The result lands in r0 only.
  - **T3 b29-31 is the compare function**, in GL's `ref OP texel` order.
    0 always returns 0, which is why a T3 from a plain sample compares to
    nothing:

    | T3 b29-31 | GL function |
    |---|---|
    | 0 | NEVER |
    | 1 | GREATER |
    | 2 | EQUAL |
    | 3 | GEQUAL |
    | 4 | LESS |
    | 5 | LEQUAL |
    | 6 | NOTEQUAL |
    | 7 | ALWAYS |

    Every code was checked at ref 0.25, 0.5 and 0.75 against 0.5, and code 4
    also at 0, 0.25, 0.49, 0.5, 0.51, 0.75 and 1. Equality is exact (F32).
  - **T3 b16-17 is the result format:** 0 F32 (1.0 = `0x3f800000`), 1 U8
    (`0xff`), 2 F16 (`0x3c00`), 3 F32.
  - T0 b0-2 has no effect here. T3 b13, b14 and b19 page-fault; T0 b21-27
    (the strided pitch) read zero or fault when changed; T0 b30 and b31 move
    where the result lands. Drivers should leave those bits as the DDK sets
    them.
  - Not tested: whether ref is clamped to [0, 1], `.pcf16`, projected
    forms, and cube shadow.
- **What GL needs** (from the DDK's desktop GL extension list, PIGLIT.md):
  - `ARB_shadow`, `EXT_shadow_funcs` (all eight compare functions) and
    `ARB_shadow_ambient`;
  - DEPTH_TEXTURE_MODE: LUMINANCE, INTENSITY or ALPHA, a swizzle of the
    depth or compare result;
  - in GLSL 1.10, `shadow1D`, `shadow2D` and their projected forms.

**Tests** (Gaps, §15):

1. DDK captures with `PROBE_TEXFMT=depth16`, `depth32` and `d24s8`, each with
   `PROBE_TEXPAT=1`, sampled with `texture2D`. Done: format 0x12, F32 texels,
   (d, d, d, 1) (above).
2. `PROBE_EXTS=1`: whether the DDK's ES2 lists `EXT_shadow_samplers`. Done:
   it doesn't, so there's no compare to capture.
3. Black-box tests with the open stack: a `smp.2d.pcf` on a format-0x12
   texture (T1 `d200f00f`, T0 `00060e00` for 16×16, F32 texels written by
   hand) with (s, t, ref) and ref swept across the stored values. That shows
   whether the result switches from 0 to 1, and which T0 or T3 field changes
   the function. Done: the compare, T3 b29-31 and T3 b16-17 (above).

**The driver path:** upload depth as F32 in format 0x12, strided, the way the
DDK does. A NEAREST compare is one `smp.pcf` with the reference as the extra
coordinate and T3 b29-31 from the compare function. Since the TPU doesn't
filter 0x12, LINEAR is four taps with a mix in the shader: four `smp.pcf`
taps, or plain samples with `nir_lower_tex_shadow` (which etnaviv and r300
use), along the four-tap path of §13. DEPTH_TEXTURE_MODE and the (d, d, d, 1)
of a plain sample are swizzles in the shader, since the sampler writes r0
only.

## Gaps

Facts above tagged **unconfirmed**, each with the hardware test or capture
that would confirm it. Shipping code must not rely on an unconfirmed fact
until it is confirmed.

### 3. The transfer queue

- Command word 1 is 0x300 when the transfer has a depth/stencil surface →
  a capture of a DDK transfer with a depth/stencil surface, or a `blitest`
  run with word 1 = 0x300 and a depth/stencil block, reading back what the
  3D writes.
- Command word 4 holds the depth/stencil block's address minus the base →
  the same capture or `blitest` run.

### 6. Presentation

- The DDK's back buffer plus swap blit is tear-free with two buffers → a
  vblank source (section 12) to time the blit against, or a moving-edge test
  on the display presented through the blit.

### 7. Depth and stencil across renders

- The DDK sets ZLS up only when a frame is flushed mid-scene (kick flag
  0x80); the `zls-*` captures already show window renders storing depth
  without a split → captures of window frames with and without a split,
  logging each kick's flags next to its render's ZLS words.
- The DDK's ZLS buffer is 5 bytes per padded pixel → a capture of the DDK's
  ZLS buffer allocation for a window with a depth buffer, its size compared
  with the padded pixel count.
- ZLS control's bits outside the base are the surface's bits masked with
  `0xf80ee007` → captures of windows with different depth/stencil configs
  (16/0, 24/0, 24/8) and a diff of +0x2c.
- One stencil byte per pixel follows the depth in the ZLS buffer, and it is
  stored and loaded → a hardware split-frame stencil test (stencil written
  before a 1-pixel readback, tested after), with the ZLS buffer read back.

### 10. GPU blits of our own

- The DDK leaves the 0xb4-byte transfer context buffer uninitialized past
  the words it sets → captures of that buffer from two DDK runs with
  different prior memory contents, compared word by word.

### 12. Tear-free presentation

- Pipe A status (+0x70024) b1 is the vblank bit on this controller, cleared
  by writing 1 → a hardware run that reads only +0x70024 at short intervals
  and sees b1 set about every 16.7 ms and clear after a write of 1 (read
  only named registers; see the cautions in section 12).

### 13. Border color

- The open driver's CLAMP_TO_BORDER lowering (mesa-gallium c01cac3) has
  not run on the board → piglit `texwrap formats bordercolor` on the
  CE5300 board.
- Code 3 under LINEAR filtering and on mipmapped textures → a `bbase`-style
  replay with T0 b12 set and a mipmapped texture.

### 14. Volume textures

- The non-dependent volume sample (a dimension-3 USP sample block, DOUTI
  bits unknown) → an open-stack PDS test of a plain `texture3D`.
- Wrap R in T0 b0-2 → the step 3 tests with T0 b0-2 swept and r outside
  [0, 1].
- Whether T3 b7-10 belong to the depth field; the twiddle order when x or y
  isn't 4; NPOT depths; mipmapped volumes; LINEAR across r → further step 3
  tests.

### 15. Depth textures

- Format 0x12 is a general one-channel F32 format, not a depth-only code
  → a DDK `PROBE_TEXFMT=lf32` capture (OES_texture_float), compared with
  `dt16`'s T1.
- Whether the reference is clamped to [0, 1]; `.pcf16` with F16
  coordinates; projected and cube shadow samples → further §15 step 3
  tests.

### Open questions

These are untested behavior, not stated facts.

- Which single bits enable the ZLS load (b1, b13, b14) and the store (b2,
  b17, b18), and which of TA 3D flags b7 and b8 is needed (section 7) →
  single-bit hardware runs on the split-frame test.
- The cut-over between CPU and transfer-queue uploads (between 256 and
  4096 texels; section 2) → captures of power-of-two textures of 512, 1024
  and 2048 texels, noting which submit a transfer.
- A vblank source for vsync; why the VDC registers went to 0 (section 12).
- The fixed-point format of the background block's vertex words 18, 20 and 21
  (sub-rectangles don't need them; section 10) → captures of blits to
  several destination sizes, diffing those words.
- PBE word 2 b15: it must be set for the PBE to write; what clearing it
  selects is unknown.
- Formats beyond the ones probed: compressed textures (PVRTC/ETC), float
  textures, the PBE format codes 2 and 6 and up → captures of uploads and
  renders in those formats.
