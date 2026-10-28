# SGX545 TA/3D render state

What the Cedarview DDK 1.7 hands the SGX545 to draw a frame, apart from the
shaders and the PDS programs (`docs/ISA.md`, `docs/PDS.md`): the client
part of the TA command, the VDM control stream, the TA state block, the
render target, the texture words, the HW render context, the render details
and the parameter buffer.

`decode.py` annotates every one of these words in a capture:

    python3 tools/state/decode.py CAPTURE.bin [--kick N] [--brief]

`--brief` drops the address-only header lines, so `diff` of two captures'
outputs shows just the state that changed.

`build.py` is the other direction. It computes the words a frame built
from scratch has to supply, from GL-level parameters and only the formulas
in this file:

- the render-target numbers;
- the PB descriptor and free list;
- the TA state block;
- the end-of-tile PBE program;
- the HW render context.

With `--check CAPTURE` it compares them with what the DDK wrote:

    python3 tools/state/build.py --check CAPTURE.bin [--w W --h H --pbuffer --cull 0x405 ...]

Every field is tagged:

- **[C] confirmed by a replay:** flipped in the blob-free replay
  (`tools/decode/mkreplay.py --poke`) on the EA3, and the pixels changed as
  predicted. The runs are listed under "Hardware proof" at the end.
- **[D] confirmed by a diff:** two probe captures that differ in one GL call
  differ in exactly this field. Also used for a field read straight from the
  DDK code that writes it, when the captures agree with it.
- **[G] guess.**

## Where the knowledge comes from

- **libGLESv2 decompiled** (Ghidra, `tools/pds/DumpDecomp.java`; addresses are
  Ghidra's, image base 0x10000):
  - `FUN_0004a3a0` (0x3a3a0): the draw. It emits the TA state block and the
    VDM index/vertex words.
  - `FUN_00049a00` (0x39a00): computes the ISP words, MTE control and
    stencil words.
  - `FUN_0004e7a0` (0x3e7a0): the PBE (pixel back end) words.
  - `FUN_00037670` (0x27670): glTexParameter to texture-word bits.
  - `FUN_00049180`, `FUN_000497f0`: the VDM terminate and state words.
  - `FUN_00017c20`: the VDM vertex-program words.
  - `FUN_0002e2d0` (0x1e2d0): fills the kick structure, which is ctx+0x160,
    and calls SGXKickTA.
  - glDepthFunc, glStencil*, glColorMask, glCullFace and friends: the
    context fields each of these feeds.
- **libsrv_um decompiled** (same script; output in
  `<scratchpad>/gp/srv_kick.c`):
  - `SGXKickTA` (0xd1f0): builds SGXMKIF_CMDTA, the render details and the
    deferred render-details updates.
  - `SGXAddRenderTarget` (0x10860) and its helper `FUN_0001f990`: the macrotile
    and TE numbers, the region headers, the tail pointers and the RT data.
  - `SGXCreateRenderContext` (0xf040): the 68-byte HW render context.
  - `FUN_0001e980`: the PB descriptor and the free list.
  - `FUN_000301d0` (srv_pds.c): the end-of-tile USSE program.
- **Captures.** `tools/probe/probe.c` has a knob for each piece of state (see
  its header). One-change captures were taken on the EA3 on 2026-10-28 and
  are listed under "Captures".

The GPL kernel's `sgx545defs.h` names only the BIF, USE code-base, PDS
exec/invalidate and event registers. It has no ISP, TSP, TE, MTE or PBE
registers, so the names used here come from the DDK code, or from the
Eurasia register conventions where marked [G].

## A frame, end to end

    CCB: SGXMKIF_CMDTA  -- header (0x58) | CMDTA_SHARED (0x1e0, kernel) | tail (0x70) | deferred updates
      header +0x38 -> render details (kdata, 0x1ec): 3D values; +0x5c -> pixel event PDS program
                         pixel event ds1[1] = DOUTU -> end-of-tile USSE program: LIMM r0-r3 = PBE words, EMITPIX
      header +0x24 -> PB descriptor (kdata, 0x3c) -> free list (3D params heap)
      header +0x40 -> RT data (kdata) -> region headers, tail pointers, background object block
      tail +0x18   -> VDM control stream (general heap)
           43040e00 0e022403   state program 0x3040e000 -> DOUTD 12 dw from 0x30401000 (TA state block) -> USSE state emit
           80c00003 <index buf> f0000000 003fffff 73040104 02c00082    draw: indices + vertex PDS program
           ...                 c0000000 terminate
      TA state block: header bits | ISPA | ISPB | ... | PDS secondary | PDS word | PDS primary | viewport | ...
           PDS primary -> pixel PDS program: DOUTU (fragment shader) + DOUTI (varyings) + DOUTT (texture words)

A single-triangle frame (the probe) is two TA kicks into one scene:

- **Kick 1** (`bFirstKickOrResume`) runs the VDM stream. That stream is the
  clear (an object covering the whole screen), then the triangle, then the
  terminate words.
- **Kick 2** (`bLastInScene`) runs only the terminate words
  (`43040006 0400e100 63040000 0e022401 c0000000`) and starts the 3D.

## 1. The TA command (SGXMKIF_CMDTA, client part)

The command sits in the kernel-data CCB, starting 0x58 bytes before
`ui32CCBOffset`. This is SGXKickTA's `local_878` structure, 0x2a8 bytes plus
the deferred updates.

### Header, cmd+0x00 to +0x58

| off | ref.bin | field | tag |
|---|---|---|---|
| 0x00 | 0x2c0 | command size, rounded up to 64 bytes (`0x2b0 + 8·n` deferred updates) | D |
| 0x04 | 0x71 / 0x62 | TA flags, from kick flags (ctx+0x164); see the bit table below | D |
| 0x08 | 04001800 | background object, PDS secondary word (kick +0x108) | D |
| 0x0c | 00030001 | background object, PDS middle word | D |
| 0x10 | 0c00060e | background object, PDS primary (0x2e0060e0: samples the render target itself, i.e. "load the old contents") | D |
| 0x14 | 0x50000000 | `0x40000000 + 0x10000000` if kick flag 0x2000000 is clear; else `n<<27` from the render context +0x40 | D |
| 0x18 | 0x50000000 | constant | D |
| 0x1c | 0 / 0x400 / 0x580 | 3D flags: b0 = !kick.b1, b1 = kick.b11, b3 = kick.b12-13, b4, b7 = kick.b19, b8 = kick.b20, b10 = kick.b10/b9 (render), b12 = kick.b10 | D |
| 0x20 | 0 | kick +0x00 | G |
| 0x24 | 3200e000 | **PB descriptor** (kdata) | D |
| 0x38 | 3200e560 | **render details** (kdata, 0x1ec) | D |
| 0x3c | 3200e7a0 | render-target dependency block (kdata, 0x28: sync addresses) | G |
| 0x40 | 3200aac0 | **RT data** (kdata) | D |
| 0x44 | 3200ab10 | RT data entry for this render (`+0x18 + idx·0x38`) | D |
| 0x48 | 0 | kick +0x08 (with kick flag 0x20) | G |
| 0x4c, 0x50 | 0 | multi-RT values (kick +0x60, +0x64) | G |
| 0x54 | 0 / 6 | number of deferred render-details updates at +0x2a8 | D |

### CMDTA_SHARED, cmd+0x58 to +0x238

This part is filled by the kernel at kick time (sync objects, status
updates). The client leaves it zero.

### Tail, cmd+0x238 to +0x2a8

Most of these come from the render target (`SGXAddRenderTarget`); see
section 6 for the formulas.

| off | ref.bin | field | tag |
|---|---|---|---|
| 0x238 | 20000000 | TE AA: `0x20000000`, b31 = x2 multisample in X, b30 = in Y | D (formula) |
| 0x23c | 82010018 | TE MTILE1: b31 = 4×4 macrotile mode, macrotile X boundaries (in 32-px tiles) at b22 / b12 / b0 = 8, 16, 24 | D |
| 0x240 | 02010018 | TE MTILE2: macrotile Y boundaries (in 16-px tiles) = 8, 16, 24 | D |
| 0x244 | 0001d016 | TE screen: last tile X (b0-11) = 22, last tile Y (b12-) = 29 | D |
| 0x248 | 0x40 | tiles per macrotile (8 × 8) | D |
| 0x24c | 0 / 0x2400000 | TA flags 2: b22-23 from kick flag 0xa00, b19, b25 | D (formula) |
| 0x250 | 3e518000 | **VDM control stream base** for this kick | C |
| 0x254 | 0xa2 | kick +0x104 | G |
| 0x258 | 20006000 | region header array of this render (TE PSG region base) | D |
| 0x25c | 20000000 | tail pointer array (TE TPC base) | D |
| 0x260, 0x264 | 358637bd | two 1e-6 floats, constants in SGXKickTA | D (constant) |
| 0x268 | 01df02cf | screen size `(h-1)<<16 \| (w-1)` | D |
| 0x26c, 0x270 | 0 | kick +0xec, +0xe8 | G |
| 0x274 | 10000000 | 3D params heap base (PB base & 0xfff00000) | D |
| 0x278 | 0 | kick +0x20 << 24 | G |
| 0x27c-0x298 | 0 | kick +0x1c..+0x40 (multisample / output values) | G |
| 0x29c | 0 | number of RT data entries - 1 | D |
| 0x2a0 | 0 | RT +0x68 << 12 | D (formula) |
| 0x2a4 | 0x88 | RT +0x14 (0x88 when RT flag b0 and no AA) | D (formula) |
| 0x2a8.. | | deferred render-details updates: (device address, value) pairs, `count` at +0x54 | D |

The deferred updates exist because the render details are written in full
only when SGXKickTA starts a new render (kick flag 0x4). Later kicks into
the same render send only the words that changed. In the probe frame with
a depth buffer, kick 2 sends six:

- ZLS control;
- the ZLS request base;
- the depth and stencil store and load addresses.

## 2. The VDM control stream

The stream is 32-bit words in the general heap. Each kick's TA command
points at the part that kick runs.

| words | meaning | tag |
|---|---|---|
| `4 \| addr>>4`, info | **state program** (PDS vertex heap): info b26-31 = data rows of 16 B; low bits `0x2022400 \| buffer index (0-3, rotating) + ((attribute dwords+3)>>2)` for a TA state block, `0xe100 \| ((n+3)>>2)` for a plain state/secondary DMA | D (rows), G (low bits) |
| `6 \| addr>>4`, info | state program, second form (the TA terminate: 4 × DOUTA + DOUTU) | D |
| `8…`, idx, w2, w3, `n \| addr>>4`, info | **draw**: see below | D |
| `c0000000` | terminate | D |

The draw is six words. libGLESv2 emits them in `FUN_0004a3a0`, with the
counts from `FUN_00017c20`:

| word | ref.bin | meaning |
|---|---|---|
| 0 | 80c00003 | `0x80000000 \| 0x00c00000 \| prim<<26 \| index count`. prim from libGLESv2's table: triangles 0, lines 1, points 2, strip 3, fan 4 (the bits are `0x4000000·k`) [D] |
| 1 | 3e517004 | index buffer address; bit 0 is cleared for 16-bit indices, bits 0-1 for 32-bit [D] |
| 2 | f0000000 | `(batch-1)<<28`, \| 0x1000000 for 32-bit indices, \| index offset [D] |
| 3 | 003fffff | max index (constant) [D] |
| 4 | f3040d82 | vertex PDS program `addr>>4`, top nibble = (vertices per USSE task - 1). **It is not a type nibble**: the VDM knows from the draw header that this word follows [D] |
| 5 | 01c00082 | b0-6 = PDS data rows, b7-21 = ceil(input attribute bytes/16), b22-23 = min(n,3), b24-31 = ceil(output dwords/4) [D] |

`glDrawArrays` draws through a static 16-bit identity index buffer.

### The state programs and what they DMA

| program | DMA | USSE task | what |
|---|---|---|---|
| 0x3040e000 (`43040e00 0e022403`) | 12 dw from 0x30401000, secondary attributes, kind 3 | 0x3c008700 | the clear's TA state block |
| 0x3040e080 (`43040e08 0e022405`) | 25 dw from 0x30401080, kind 0 | 0x3c008a00 | the triangle's TA state block |
| 0x30401120 (`43040112 0800e103`) | 10 dw from 0x304010e0, kind 1 | 0x3c000080 | vertex secondary attributes: the block's own address, then the vertex shader's uniforms and driver constants (2/255, 1/255, 2⁻¹⁵, 2⁻¹⁶) |
| 0x30400060 (`43040006 0400e100`) | none | 0x3c000080 | empty secondary update (the end of the stream) |
| 0x30400000 (`63040000 0e022401`) | DOUTA (0x2000,0), 3 × (0x16000e, 0x100) | 0x3c000000 | TA terminate |

- The static state program at 0x3040e000 and the 399 copies after it is
  `tst; bra; doutd ds0[0..1]; tst; bra; doutd ds0[2..3]; doutu ds0[4..5]`.
  That gives two optional DMAs. The rotating buffer index in the info word
  picks one of four copies.
- **The state-emit USSE programs are just `emit` instructions.** They copy
  the DMA'd attributes to the MTE (macro tiling engine). The block is the
  state; the program only forwards it. (The emit/LIMM opcode class `0x1f`
  is not in `tools/isa` yet.)

## 3. The TA state block (ISP/TSP/MTE)

The block is the first word of a state-program DMA. The header word says
which groups follow, in this order. libGLESv2 `FUN_0004a3a0` writes it,
with the context dirty bits in parentheses:

| header bit | words | group | source | tag |
|---|---|---|---|---|
| 0x1 | 1 | ISPA | ctx+0x780, objtype bits 15-18 replaced per draw | D |
| 0x2 | 1 | ISPB (present when ISPA b9) | ctx+0x784 | D |
| 0x4 | 1 | ISPC, front stencil (present when ISPA b8) | ctx+0x788 | D |
| 0x8, 0x10, 0x20 | 3 | back-face ISPA, ISPB, ISPC (two-sided stencil) | ctx+0x78c..0x794 | D |
| 0x40 | 3 | PDS: secondary, middle word, primary | ctx+0xcac/cb0, frag+0x64, ctx+0xc9c/ca0 | D |
| 0x80 | 2 | region clip (clears and driver objects only): `b31 \| xmin<<16 \| xmax`, `ymin<<16 \| ymax`, in 32×16-px tiles | RT+0x114, +0x118 or the scissor | D |
| 0x100 | 6 | viewport (dirty 0x100) | ctx+0x13c..0x150 | C |
| 0x200 | 1 | always 0 (dirty 0x200) | | D |
| 0x400 | 1 | output select (dirty 0x1000) | vertex program +0x248 | D |
| 0x800 | 1 | W clamp, `0x3727c5ac` = 1e-5 (dirty 0x200) | | D |
| 0x1000 | 1 | MTE control (dirty 0x400) | ctx+0x798 | D |
| 0x4000 (+0x8000) | 2 | TSP texcoord sizes, texcoord precision mask (dirty 0x1000) | program +0x24c, +0x250 | D |

- The clear's header is `d4c3` and the triangle's `df43`.
- The type-4 VDM info word's low bits follow this block's length:
  `(dwords+3)>>2` (12 → 3, 13 → 4, 18 → 5), ORed with the DDK's 2-bit
  counter (`ctx+0xc80`) [D].
- The DMA reads 25 dwords for the triangle's 18-word block. The extra words
  are whatever follows; the header alone says where the block ends.

### ISPA (ctx+0x780)

| bits | field | tag |
|---|---|---|
| 0-7 | stencil reference (single-sided; front/back in ISPC otherwise) | D (code) |
| 8 | ISPC follows (stencil enabled with a stencil buffer) | D |
| 9 | ISPB follows: always set | D |
| 11 | two-sided stencil | D (code) |
| 15-18 | object type, set per draw (0x10000 for points in a mode-7 draw) | G |
| 20 | depth write disable | D |
| 21 | no colour write: colour mask 0, or a blend that keeps the destination (ZERO, ONE) | D |
| 22-24 | depth compare, `GL enum - 0x200`: never, less, equal, lequal, greater, notequal, gequal, always. Depth test off, or no depth buffer, gives always + no write (`0x01d00000`) | C (greater) |
| 25-27 | pass type: 0 opaque, 1 translucent (any glBlendFunc; `b1`/`b2`/`b3` captures), 2 translucent + discard, **3 viewport/scissor generate** (`viewport`/`scissor` captures), 4 punch-through (discard/alpha test), 5 depth-only (stencil/discard special case) | D |

### ISPB (ctx+0x784)

| bits | field | tag |
|---|---|---|
| 4-7 | colour write mask: R = 8, G = 4, B = 2, A = 1 | D |
| 8-15 | line/point width × 16 (1.0 → 0x10) | D |
| 23 | depth bias (POLYGON_OFFSET_FILL) | D |

### ISPC: stencil (ctx+0xa0 front, +0xa4 back)

| bits | field | tag |
|---|---|---|
| 0-7 | write mask (glStencilMask) | D |
| 8-15 | compare mask (glStencilFunc mask) | D |
| 16-18 | zpass op | D |
| 19-21 | zfail op | D |
| 22-24 | sfail op | D |
| 25-27 | compare, `GL enum - 0x200`; 7 (`0xe000000`) when stencil is off | D |

The op codes are 0 keep, 1 zero, 2 replace, 3 incr, 4 decr, 5 invert,
6 incr_wrap and 7 decr_wrap.

Confirmed field by field by the `s1`..`s6` captures:

- `0400f0ff` = EQUAL, compare mask 0xf0, write mask 0xff;
- `0453f0ff` = ops ZERO/REPLACE/INCR;
- the reference lands in ISPA b0-7 (`01d00355`).

**glClearStencil** is the clear object getting ISPC `0e0200ff` (always,
zpass = replace, write mask 0xff) with the clear value as its ISPA
reference (`cstencil`).

### PDS words

- **Word 0 (secondary) and word 2 (primary):**
  `(data bytes/16)<<26 | (addr - 0x2e000000)>>4` [D].
  - The secondary program DMAs the fragment uniforms and runs the secondary
    USSE program.
  - The primary program runs the fragment shader: DOUTU, plus DOUTI and
    DOUTT per input.
- **Word 1:** from the fragment program object +0x64.
  - b18-24 = (secondary attribute dwords + 3)>>2 [D].
  - b0, b16-17 and b25-28 come from the compiled shader [G]: the clear
    gives `00030001`, the uniform shader `160f0001`.

### Viewport (6 floats)

The words are, in order: x offset, x scale, y offset, y scale, z offset and
z scale.

- x offset = x + w/2, and x scale = w/2.
- On a window, y scale = -h/2 and y offset = H - (y + h/2). On a pbuffer
  (EGL type 4), y scale = +h/2 and y offset = y + h/2.
- z offset = (n+f)/2, and z scale = (f-n)/2.
- Status: x offset [C]; the rest [D] (`viewport`, `drange` and `pb`
  captures).

### MTE control (ctx+0x798)

- **b0-1, cull:** 0 = none, 1 = cull CW-in-screen, 2 = cull CCW-in-screen.
  - GL CCW front with cull BACK gives 1; cull FRONT gives 2.
  - With CW front the two swap.
  - FRONT_AND_BACK is 0 here; GL drops those draws itself.
  - Status: C. On the replay, 2 removes the probe triangle and 1 keeps it.
    In the captures, `cullb` gives 1, and `cullf` and `cullcw` give 2.
- **The rest is a GL constant, `0x1555620`** [G]. The clear object uses `0x40`.

### Output select (vertex program +0x248) and TSP words

- **Output select:** `(vertex output dwords)<<16 | 0x3000`, with `| 0x100`
  when the shader writes the point size.
  - 4 dwords are the position. Each texcoord set adds its size.
  - The clear's own value is `0x00081800` (8 dwords: position and colour).
  - Status: D.
- **TSP word 0:** 3 bits per texcoord set: 1, 2, 3 components, or 5 for 4
  components [D].
- **TSP word 1:** a bitmask of the sets whose precision value is at most 2 (libGLESv2 0x203bc: `cmpl $0x2, 0x4c(set)`, `ja` skips) [D].

### Blending and the colour mask are shader code

- **The SGX545 has no fixed-function blender.**
  - glBlendFunc and glBlendEquation only change ctx+0xf8/+0xfc. Those feed
    the fragment-shader variant, which gets blend code appended. They also
    set ISPA's pass type (translucent) and b21.
  - The clear colour and glBlendColor go into the variant's constants.
- **The colour mask is both shader code and ISPB.** It is ISPB b4-7, plus a
  masked `0x90b00601 | mask<<11` move appended to the shader
  (libGLESv2 `FUN_000459.. `, 0x35b00), plus ISPA b21 when the mask is 0.

So reproducing GL blending means generating the shader epilogue, not
setting a state word.

- **The captures agree.** `b1` (SRC_ALPHA, 1-SRC_ALPHA), `b2` (ONE, ONE)
  and `b3` (+ FUNC_SUBTRACT) differ from the base only in ISPA pass type
  0 → 1, plus the fragment shader.
- **Colour mask 1 0 1 1** gives ISPB mask 0xb and a different clear shader.
- **Colour mask 0** sets ISPA b21 and ISPB mask 0. The clear turns into a
  full-screen object that samples the previous buffer instead (a masked
  clear changes nothing).

### Scissor and partial viewports: pass type 3 objects

There is no per-pixel scissor rectangle in the state.

When glScissor or glViewport covers less than the surface, libGLESv2
inserts these objects ahead of the draws (`scissor` and `viewport`
captures):

1. **Load object (scissor only):** a full-screen object that samples the
   previous buffer (texture layout 6, T0 `00b60e90`, T1 `cc2cf1df`). It
   keeps what lies outside the scissor.
2. **Reset object:** a full-surface object with ISPA `07e00100` (pass type
   3, no colour write, depth write on, always) and ISPC `00000000`, over
   the whole region clip.
3. **Rectangle object:** a 4-vertex strip (VDM prim 3) with screen-space
   x, y, z positions of the rectangle (the `scissor` capture's vertices
   are 100..300 × 330..430 = GL (100,50,200,100) flipped into a 480-high
   window). It has ISPA `07e00100` and ISPC `02000000`, and its region
   clip is the rectangle's tiles (`80030009 0014001a` = x 3..9, y 20..26).
4. **The clear:** region-clipped the same way.
5. **The draws:** unchanged, with the full-surface viewport transform when
   only the scissor changed.

For pass-type-3 objects the ISPC word's b25-27 select reset (0) or set
(1) of the visible region [D].

## 4. The render target

### The end-of-tile program (pixel event → EOT USSE program)

- Render details +0x5c points at the pixel event PDS program (0x2e006000,
  from libsrv_um's static template).
- Its ds1[1] is a DOUTU word for the end-of-tile USSE program, here
  0x37004000.
- `FUN_000301d0` builds that program as four LIMMs that load the PBE words
  into r0-r3, then two EMITPIX:

      LIMM  lo = v & 0x1fffff | reg<<21,  hi = 0xfca00000 | (v>>21 & 0x1f)<<4 | (v>>26)<<12
      EMITPIX lo 0x00200080, hi 0xfb200100                        (the four PBE words)
      EMITPIX lo (side & 0x3f)<<22 | 0x00208180,
              hi 0xfb240000 | (side & 0x3000)<<10 | (side>>3 & 0x1f8)   (the sideband word)

### PBE words (`FUN_0004e7a0`)

| word | ref.bin | field | tag |
|---|---|---|---|
| 0 | 00000000 | clip min `y0<<16 \| x0` | D |
| 1 | 01df02cf | clip max `y1<<16 \| x1` (719, 479) | C |
| 2 | 016f8000 | b16-27 = (stride in pixels)/2 - 1 (367 → 736 px); b15 = set unless the surface's `param_1[6]` is non-zero: set for the back buffer *and* for fb0 (the swap), and with it cleared the PBE writes nothing at the address (replay, `docs/TEX.md` section 6), so it is not a scanout flag; b28-29 = rotation (0x10000000·k), b30-31 = downscale/AA (0x40000000·k) | C (stride) |
| 3 | 3e265000 | address, & ~3 (moved for rotated surfaces) | D |
| side | 0x600 | b0-5 = PBE format code (format table +0x84: 0 BGRA8888, 1 RGBA8888, 3 RGB565, 4 RGBA5551, 5 RGBA4444, see `docs/TEX.md`); b9 = always; b10 = dither (GL_DITHER); b12 = `param 7`; **b13 = twiddled destination** (table `DAT_0006d5bc` = {0, 0x2000} by memory layout; the transfer queue's texture uploads set it) [C]; b6-11 += `param_2[5]<<6` for format codes 3-14 | D |

- The back buffer of a window is BGRA8888, top row first, with a pitch of
  720 rounded up to 32 = 736 px: 0x159000 bytes = 736 × 480 × 4.
- glReadPixels returns RGBA, bottom row first.

### Format codes

- Every surface EGL gave the probe was BGRA8888, PBE format code 0:
  windows, and pbuffers whether 5 6 5 or 8 8 8 8 was requested (`pb`,
  `pb8888`, `pbd`).
- So code 0 = BGRA8888 [D]. Other codes come from libGLESv2's per-format
  table (entry +0x84); its entries are filled at run time and were not
  read.

### Screen and tiles

- The TE tile is 32 × 16 pixels. SGXAddRenderTarget computes
  `xtiles = (w+31)/32` and `ytiles = (h+15)/16`.
- **Region clip** (TA block group 0x80, clear object only):
  - word 0 = `0x80000000 | (xmax tile)`, with b12-21 = xmin;
  - word 1 = ymax tile, with b12-21 = ymin;
  - here 22 and 29;
  - status: D (`scissor` capture).
- glScissor is not a hardware rectangle; see "Scissor and partial
  viewports" in section 3.
- **Pbuffers** (`pb` 64×32, `pb2` 128×48): 2×2 macrotile mode
  (MTILE1 = MTILE2 = `01004004`, 16 tiles per macrotile), positive viewport
  y scale, and PBE stride = width rounded up to 32. All of it is
  reproduced by `build.py`.

## 5. Texture state (DOUTT, 4 words per sample)

The pixel PDS program sends DOUTT right after the DOUTI that starts the
sample. libGLESv2 `FUN_00047b90` builds each word as `(per-unit state & mask)
| override`. The per-unit state is ctx+0xa00 + unit·0x40, copied from the
texture object.

| word | field | tag |
|---|---|---|
| 0 | see the table below: filter (min, mag, mip), wrap S/T, format | D |
| 1 | b0-11 = height-1, b12-23 = width-1; b24-31: see the table below | D |
| 2 | texture base address (texture heap 0xf0000000; the render target for the background object) | D |
| 3 | see the table below | D |

From the texture object (glTexParameter, `FUN_00037670`):

- **wrap S:** texobj+0x1c b6-8: 0 repeat, 1 mirror (0x40), 2 clamp (0x80);
- **wrap T:** texobj+0x1c b3-5: 0 repeat, 1 mirror (8), 2 clamp (0x10);
- **min filter** (texobj+0x50): NEAREST `0x1e0000`, LINEAR `0x1e0400`,
  N_MIP_N `0x1a0000`, L_MIP_N `0x1a0400`, N_MIP_L `0x1a0200`,
  L_MIP_L `0x1a0600`;
- **mag filter** (texobj+0x54): NEAREST 0, LINEAR `0x1000`.

### Texture words, measured

Every capture below is an 8×8 checker, nearest filtering, repeat, unless
named otherwise; each differs from `t8888` in one thing.

| capture | T0 | T1 | T2 | T3 |
|---|---|---|---|---|
| t8888 (RGBA/UNSIGNED_BYTE) | 07fe0000 | 0d007007 | f0001000 | 00800800 |
| t888 (RGB/UNSIGNED_BYTE) | 07fe0000 | **0d**007007 | | |
| t565 | 07fe0000 | **05**007007 | | |
| t4444 | 07fe0000 | **02**007007 | | |
| t5551 | 07fe0000 | **04**007007 | | |
| tl8 / ta8 / tla88 | **4**7fe0000 | **1a** / **19** / **1b**007007 | | |
| tlin (min+mag LINEAR) | 07fe**14**00 | | | |
| tminlin / tmaglin | 07fe**04**00 / 07fe**10**00 | | | |
| tclamp (S, T clamp) | 07fe00**90** | | | |
| tclamps (S clamp) | 07fe00**80** | | | |
| tmirror (S, T mirror) | 07fe00**48** | | | |
| t16x8 / t8x16 / t64 | | 0d00**f**007 / 0d0070**0f** / 0d0**3f03f** | | |
| tnpot (12×6, clamp) | 07fe0090 | **6**d00**b005** | | |
| tmip (16×16, NEAREST_MIPMAP_NEAREST) | 07f**a**0000 | 0d00f00f | | |
| tmip2 (LINEAR_MIPMAP_LINEAR, mag LINEAR) | 07f**a1600** | | | |
| tmipb (mipmaps generated, min NEAREST) | 07fe0000 | | | |

**T0** (the texture object's +0x1c | +0x50 | +0x54, ORed with the unit's
constant):

| bits | field | tag |
|---|---|---|
| 3-5 | wrap T: 0 repeat, 1 mirror, 2 clamp | C (mirror) |
| 6-8 | wrap S: 0 repeat, 1 mirror, 2 clamp | C (mirror) |
| 9 | mip filter linear | D |
| 10 | min filter linear | D |
| 12 | mag filter linear | C |
| 17-20 | 0xf without mipmaps, 0xd with (b18 = mipmapping off) | D |
| 21-26 | 0x3f in every capture (an LOD clamp?) | G |
| 30 | set for L8, A8 and LA88 (channel replication?) | D |

**T1:**

| bits | field | tag |
|---|---|---|
| 0-11 | height - 1 | D |
| 12-23 | width - 1 | D |
| 24-28 | format: 0x02 RGBA4444 (stored ARGB4444), 0x04 RGBA5551 (stored ARGB1555), 0x05 RGB565, 0x0c BGRA8888 (render target), 0x0d RGBA8888 (RGB888 is uploaded as 8888), 0x19 A8, 0x1a L8, 0x1b LA88 | D |
| 29-31 | layout: 0 twiddled (power of two), 3 linear (NPOT; no stride field), 6 strided (T0 then holds the pitch: (pitch in texels - 1) low 3 bits in b9-11, the rest in b17-26 [D], `docs/TEX.md` section 3). Memory layouts 0 and 3 are in `docs/TEX.md` section 1 | D |

**T2:** the base address. The mip levels follow the base level; the chain
length is implied by the size, since no word changed with the level count.

**T3:** the shader half of `(state & mask) | override`. It depends on the
sample instruction, not the texture: `tex1` gives `00800800` and `tex2`
`01028800` [D].

## 6. HW render context, render details, parameter buffer

### HW render context (68 bytes, SGX_REGISTER_HW_RENDER_CONTEXT)

`SGXCreateRenderContext` builds it. The kernel copies it, writes the PD
physical address at byte offset 8, and returns its device address (here
0x3000e000).

| word | ref.bin | field |
|---|---|---|
| 0 | 0x21 | flags: b0 = 1, b5 = per-context PB [D] |
| 1 | 0 | |
| 2 | 0 | page directory, filled by the kernel (`ui32OffsetToPDDevPAddr` = 8) [D] |
| 3 | 3200f000 | CCB base (kdata, 0x10420 bytes = 0x10000 + 0x420 of control) [D] |
| 4 | 3200aa40 | CCB control: write offset, read offset (kdata, 8 bytes) [D] (0x3200aa60 is a transfer context's) |
| 5 | 0 | |
| 6 | 3200e000 | PB descriptor (kdata, 0x3c) [D] |
| 7 | 10462000 | 0x40-byte scratch block in the 3D params heap (the microkernel's per-context data) [D] |
| 8-15 | 0 | |
| 16 | pid | creating process ID (replaying the captured one is harmless) [D] |

### PB descriptor (kdata, 0x3c, `FUN_0001e980`)

The PB is allocated as the requested size + 0x22000 bytes in the 3D params
heap. Here that is 0x400000 + 0x22000 = 0x422000 bytes = 0x422 pages.

| off | ref.bin | field |
|---|---|---|
| 0x00 | 0 | flags: b1-4 = app-hint thresholds overridden |
| 0x04 | 0x422 | pages in the PB |
| 0x08 | 04210000 | free list head: `last page<<16 \| first page` |
| 0x0c | 04200000 | `(last page - 1)<<16` |
| 0x10 | 10422000 | free list address |
| 0x14 | 0 | 0 at kick 1; 0x19 after the first TA (pages in use?) [G] |
| 0x18, 0x1c | | copies of 0x08, 0x0c |
| 0x28 | 0x432 | TA threshold = pages + 0x10 |
| 0x2c | 0x400 | ZLS threshold = pages - 0x22 |
| 0x30 | 0x400 | global threshold = pages - 0x22 |
| 0x34 | 0x300 | PDS threshold = min(ZLS, TA) - 0x100 |
| 0x38 | 3200e03c | PB block list (kdata, 0x10 bytes): {pages, first page \| last page<<16, PB descriptor address, 0} (`FUN_0001e980`; the capture has 0x3200e000) |

All [D] (formula and capture agree).

**Free list** (3D params heap, 0x40000 bytes, `FUN_0001e980`): one u32 per
page, `prev<<16 | next`, indexed by page number from the heap base. Page 0 has prev 0xffff, and the last page has next
0xffff: `ffff0001 00000002 00010003 … 041f0421 0420ffff`. The client writes
it once, before the first kick.

### Render target (`SGXAddRenderTarget`)

For w × h, no multisampling:

- `xt = (w+31)>>5` and `yt = (h+15)>>4`. For 720×480 that is 23 × 30.
- **Macrotile mode 0** (w·h < 0x4b000 pixels, the MTModePixelThreshold
  hint): 2×2 macrotiles.
  - `mx = ((xt+1)>>1 + 3) & ~3` and `my = ((yt+1)>>1 + 3) & ~3`.
  - MTILE1 = `mx | mx<<12 | mx<<22`, and MTILE2 likewise from my.
- **Mode 1** (larger): 4×4 macrotiles.
  - `mx = ((xt+3)>>2 + 3) & ~3`, and the X boundaries are mx, 2mx, 3mx.
  - Y likewise.
  - MTILE1 gets b31.
- TE screen = `(yt-1)<<12 | (xt-1)`.
- Tiles per macrotile = mx·my (tail +0x248).

### RT data (kdata, `nbuf·0x38 + 0x18` bytes; nbuf = 2)

- **Header:**
  - [0] = a 4-byte kdata word (frame counter?);
  - [1] = 1;
  - [2] = padded tiles in X (mx·4 = 0x20);
  - [5] = nbuf.
- **Per entry (0x38 bytes from +0x18):**

  | word | field |
  |---|---|
  | 0 | HW render context device address |
  | 1 | render details of the render in flight |
  | 2 | (mx·my)>>4 |
  | 3 | mx·my·0xc |
  | 4 | background object block (3D params heap, 0x200 bytes) |
  | 5 | 0x88-byte TA-data block |
  | 6 | 0x40-byte TA-data block |
  | 7 | 0x50-byte TA-data block |
  | 8 | tail pointer array |
  | 9 | its size |
  | 10 | number of RTs |
  | 11 | |
  | 12 | 4-byte kdata word |
  | 13 | 0x10 |

  All [D] from the code and the capture; the purposes of the 0x40, 0x50
  and 0x88 TA-data blocks are [G].

### Region headers and tail pointers (TA data heap)

- **Region headers:** two arrays (double-buffered, 0x20006000 and
  0x20003000) of `(32·32 padded tiles) × 12 bytes = 0x3000`.
  - Render details +0x20 is the one this render uses and +0x28 the next.
  - The tail's +0x258 names the TA's.
- **Tail pointers:** one array of `max(mx,my)² × 4` bytes rounded to a
  power of two = 0x1000.
- **The TA writes both.** The client allocates them zeroed and never writes
  them. After kick 2 a region header reads `00000000 00001000 00000000`
  (offset of that tile's control stream in the PB, 0x80 apart), and the
  tail pointers `8040xxxx`.

### Background object block (3D params heap, 0x200 bytes)

`FUN_0001f990` fills it once per RT. The render details point at it as
`0x2000000 | (addr - PB base)>>4` (+0x48).

- [0-2] = 0.
- [3-8] = a 2-D rotation matrix (identity + RT+0x30 for no rotation).
- [10] = 0x202.
- [15] = 0x100000.
- [16] = 0x1000, [17] = 0x3f80.
- [18] = `((w+0x1000)·0x100)<<8 | 0x10`, and [19] = 1.0.
- [20] = `(h+0x1000)·0x100`, [21] = 0x1000, [22] = 0x3f80.
- These are screen-space vertices in fixed point, offset by 0x1000.

### Render details (kdata, 0x1ec; SGXKickTA copies 0xd0 bytes of it)

| off | ref.bin | field | tag |
|---|---|---|---|
| 0x00 | 10000000 | 3D request base (PB base & 0xfff00000) | D |
| 0x04 | 0 / 3e500000 | ZLS request base: lowest of the depth/stencil load/store addresses & 0xfff00000 | D |
| 0x08 | 0x80 | kick +0x90 \| 0x80 | G |
| 0x0c | 000f0000 | kick +0x70 \| +0x18 | G |
| 0x10 | 80000000 | RT +0x10 \| b31 | D |
| 0x14 | 0 / 2 | depth bias units (glPolygonOffset units, rounded; ctx+0xdc) | D (`polyoff`) |
| 0x18 | 0 / 1.0 | depth bias factor (float) | D (`polyoff`) |
| 0x1c | 0 | kick +0x7c | G |
| 0x20 | 20006000 | region headers of this render | D |
| 0x24 | 0 | kick +0x4c | G |
| 0x28 | 20003000 | region headers of the next render | D |
| 0x2c | 0 / 001710b4 | ZLS control: `drawable bits & 0xf80ee007 \| 0x111000 \| (xt-1)<<3` (kick +0x6c) | D |
| 0x30, 0x34 | | depth / stencil store addresses - ZLS base | D |
| 0x38, 0x3c | 80000000 | b31 \| depth / stencil load addresses - ZLS base | D |
| 0x40 | 3f800000 | ISP background depth: the depth every tile starts from (kick +0x10, 1.0). glClearDepth does **not** change it: the clear writes depth through its own vertices' z, with depth write on in its ISPA (`cdepth`) | C |
| 0x44 | 0x300 | kick +0x80 \| +0x14 | G |
| 0x48 | 02046300 | background object: `0x2000000 \| (block - PB base)>>4` | D |
| 0x4c, 0x50 | 0, 0x88 | RT +0x1c, +0x18 (0x88 = RT flag b0) | D |
| 0x54 | 0 | 2 when the render context has a TA dependency | G |
| 0x58 | 0x30 | constant | D |
| 0x5c | 2e006000 | pixel event PDS program (end of tile) | D |
| 0x60 | 5 | its data rows | D |
| 0x64 | 1 | kick +0x8c | G |
| 0x68 | 1 | RT flag b0 | D |
| 0x6c-0xc8 | 0 | kick +0xc0..+0xe4 and +0x94..+0xbc (multisample / PBE extras) | G |
| 0xcc | 0x100 | b8 = kick flag 0x8000000 | D |
| 0x13c | 07c00000 | `n<<22` from the render context +0x3c, default 0x7c00000 | D |

## Captures

These were taken on the EA3 with `probe-st` (`tools/probe/probe.c`) under
pvrtrace on 2026-10-28, all rc=0. Unless noted, each differs from its base
in exactly one GL call.

Base captures and their bases:

- `base` and `ref` (`tools/decode`'s ref.bin) are the probe defaults;
- `d0` is a depth buffer (config 5 6 5 0 24 0);
- `s0` is depth + stencil (… 24 8);
- `t8888` is one 8×8 RGBA8888 texture
  (`PROBE_TEX=1 PROBE_ATTRS=2`, `v = a1*4`, `gl_FragColor = texture2D(t0, v)`).

| capture | change | what moved |
|---|---|---|
| dless / dnever / dgreater | PROBE_DEPTH func (write on) | ISPA b22-24 = 1 / 0 / 4, b20 clear |
| dmask0 | depth mask 0 | ISPA b20 |
| cdepth | glClearDepthf(0.5) | the clear's vertex z (1.0 → 0.5) and its ISPA depth write |
| drange | glDepthRangef(0.25, 0.75) | viewport z scale 0.5 → 0.25 (offset stays 0.5) |
| s1..s6 | stencil func/ref/masks/ops | ISPA b8 + sref, ISPC (section 3), DMA 25 → 16 dwords |
| cstencil | glClearStencil(90) | the clear gets ISPC `0e0200ff`, sref 90 |
| b1 / b2 / b3 | blend funcs, equation | ISPA pass type 1, fragment shader |
| cmask / cmask0 | glColorMask 1011 / 0000 | ISPB b4-7; ISPA b21; the clear becomes a load |
| scissor / viewport | glScissor / glViewport (sub-rectangle) | pass-type-3 objects, region clips, the triangle's viewport words |
| cullb / cullf / cullcw | cull back / front / back with CW front | MTE ctrl 1 / 2 / 2 |
| polyoff | glPolygonOffset(1, 2) | ISPB b23; render details +0x14 = 2, +0x18 = 1.0 |
| dither0 | glDisable(GL_DITHER) | PBE sideband b10 |
| t565 … tla88 | texture format | T1 b24-28, T0 b30 |
| tlin … tmirror | filters, wraps | T0 |
| t16x8 … tnpot | sizes | T1 |
| tmip, tmip2, tmipb | mipmaps | T0 b9, b18 |
| pb, pb2, pb8888, pbd | pbuffer 64×32, 128×48, 8888, + depth | RT numbers, PBE words, viewport sign |

Every capture's TA state block, PBE words, RT numbers, PB descriptor and
free list are rebuilt from the formulas by `build.py --check` (run with
each capture's GL state). They all match bit for bit: ref, d0, dless, s1,
drange, cullb, polyoff, dither0, cmask, b1, pb and pb2.

## Hardware proof (blob-free replays)

- **Setup:** `tools/decode/mkreplay.py <capture> --poke VA:word` on the EA3
  (10.0.0.113), with the static x86_64 `tools/replay/replay`, no DDK loaded,
  2026-10-28.
- **The check:** each result is compared with a prediction computed from
  the capture's own glReadPixels output (scratchpad `w/pred.py`).
- **The rule:** one field per run; the control runs come first.
- **Health:** no run produced an SGX hardware recovery.

| run | item | poke | prediction | result |
|---|---|---|---|---|
| p0 | — | none (ref.bin) | identical | 0 of 345600 differ |
| p1 | 2, TA state block | triangle block +0x18, viewport x offset 360.0 → 180.0 (`0x30401098:0x43340000`) | the frame shifted 180 px left | 0 differ |
| p2 | 3, ISPA | ISPA `01d00200` → `01100200` (dcmp always → greater, write off) | no triangle (0.5 > 1.0 is false) | 0 differ |
| p3a | 3 (control for p3b) | ISPA → `00500200` (less) | unchanged | 0 differ |
| p3b | 6, render details | p3a + render details +0x40 = 0.25 | no triangle (0.5 < 0.25 is false) | 0 differ |
| p4 / p4b | 3, MTE ctrl | `01555620` → `01555622` / `01555621` | no triangle / unchanged | 0 / 0 differ |
| p5 | 4, PBE | EOT LIMM r1: clip max (719,479) → (359,239) (`0x37004010:0x002f0167,0xfca00070`) | only 360×240 written, the rest stays 0 | 0 differ |
| p6 | 4, PBE | EOT LIMM r2: stride 736 → 640 (`0x37004018:0x005f8000,0xfca00090`) | the frame laid out at pitch 640 | 0 of 353280 differ |
| p7 | 1, TA command | kick 1's VDM base (cmd+0x250) 0x3e518000 → 0x3e518020, which skips the clear | triangle on the untouched zero buffer | 0 differ |
| p8a | 5 (control) | none (t8888) | identical | 0 differ |
| p8 | 5, DOUTT | T0 `07fe0000` → `07fe1400` (min+mag linear, `0x2e018038`) | the DDK's own `tlin` readback | 0 differ (42484 pixels changed) |
| p9 | 5, DOUTT | T0 → `07fe0048` (S, T mirror) | the DDK's own `tmirror` readback | 0 differ |

p8 sets the min and mag bits together. The triangle samples a magnified
texture, so only mag is proven by the pixels; min stays [D].

## Building a frame from scratch

Done in `tools/gen` (see `docs/GEN.md`): a frame built from these formulas
alone is pixel-exact on the EA3. Two things it needed that this file didn't
say:

- every vertex program ends with an epilogue phase;
- the kernel-data heap must be laid out as the DDK lays it out, because the
  microkernel reads the previous client's render details.

The list of what a frame needs, in order:

1. **Device memory:** allocate the heaps' buffers.
   - The kernel-data CCB, PB descriptor, render details and RT data;
     everything else can be anywhere in its heap.
2. **The PB:** allocate it in the 3D params heap, then write the free list
   and the PB descriptor (section 6).
3. **The RT data and region-header arrays:** compute the macrotile numbers
   and fill the background object block (section 6). Allocate the region
   headers and tail pointers zeroed.
4. **The HW render context (68 bytes):** register it and keep its device
   address for `sCommand.ui32Data[1]` and RT data [0].
5. **The render details:** PB base, region headers, background depth 1.0,
   the background object word and the pixel event program, with the
   end-of-tile program's four PBE words (section 4).
6. **The VDM stream:**
   - for each draw, a state program DMAing the TA state block, a
     secondary-attribute state program (vertex uniforms) and a draw;
   - then the terminate sequence.
7. **The TA commands:** kick 1 (first) runs the stream; kick 2 (last in
   scene) runs the terminate words and renders. Each has the header, the
   tail from the RT numbers, and the VDM base.

The kernel fills CMDTA_SHARED, and the microkernel fills the region headers
and tail pointers.
