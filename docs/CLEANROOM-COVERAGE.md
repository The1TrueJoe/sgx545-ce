# Documentation coverage of the shipped code

Which section of `docs/` specifies each hardware fact the code we ship uses,
and which facts no doc states yet. The goal is that every fact can be checked
against the docs alone: the spec side fills the gaps listed here, then the
code is checked against the docs.

Scope (um-re 1900475, 2026-11-04): `mesa/src/gallium/drivers/sgx545`,
`mesa/src/gallium/winsys/sgx545`, `mesa/src/sgx545` (isa, pds, compiler) and
`drm/` (the powervr_sgx kernel driver and its tools; tests get one row each).

A **hardware fact** is any of these:
- a magic number;
- a register or state-word layout, or a bit field;
- a size or alignment, a heap address, a format;
- the order or structure of hardware words;
- a kernel or firmware interface fact;
- hardware behaviour the code relies on ("X hangs the GPU").

Ordinary Gallium, NIR, C or DRM logic is not.

| status | meaning |
|---|---|
| covered | every value, bit and layout the block uses is stated in the doc section named |
| partial | the doc describes the area, but values, bits or behaviour the code uses are not stated; they are listed under "Facts the docs need" |
| missing | no doc states it |
| n/a | the file or block holds no hardware facts |

## Summary

Counts are rows of the tables below (one row per file or block of facts).

| area | covered | partial | missing | n/a |
|---|---|---|---|---|
| gallium driver (`drivers/sgx545`) | 44 | 27 | 2 | 12 |
| winsys (`winsys/sgx545`) | 44 | 34 | 2 | 3 |
| compiler (`sgx545/compiler`) | 33 | 26 | 0 | 5 |
| USSE instruction set (`sgx545/isa`) | 5 | 0 | 0 | 1 |
| PDS (`sgx545/pds`) | 4 | 0 | 0 | 1 |
| kernel driver (`drm/`) | 33 | 44 | 5 | 2 (+ 7 test rows) |
| **total** | **163** | **131** | **9** | **24** |

The instruction encodings (USSE and PDS) are generated from ISA.md and PDS.md
and are covered. The gaps are in the state around them:
- the kernel's register map and MMU formats;
- the firmware and render-command structures;
- hardware rules that so far live only in code comments (lock-ups, limits,
  formulas);
- the lowp C10 arithmetic the compiler uses.

## The largest gaps

1. **The SGX register map** (`drm/powervr_sgx/pvrsgx_regs.h`). These
   offsets and fields aren't stated in any doc; the name may appear, but not
   the offset or the bits:
   - `SOFT_RESET` 0x80 and its bits;
   - `CLKGATECTL` 0x0 / 0x4, `CORE_ID` 0x1c, `CORE_REVISION` 0x20;
   - `EVENT_STATUS` 0x12c, `STATUS2` 0x118, `HOST_CLEAR2` 0x114,
     `MASTER_INTERRUPT` bit 31;
   - `EVENT_KICK` 0xac8, `KICK2` 0xae8;
   - `BIF_CTRL` 0xc00, `BIF_INT_STAT` 0xc04, `BIF_FAULT` 0xc08;
   - the directory lists at 0xc38 + 4(n - 1), `BANK_SET` 0xc74;
   - which registers are safe to read on a cold core.

   Home: DRM-DRIVER.md, or a register section in UKERNEL.md.
2. **MMU entry formats:**
   - page-directory and page-table entries: the valid bit, the page-size
     field, and the PTE bits (WRITEONLY, READONLY, CACHECONSISTENT,
     EDMPROTECT);
   - the reset and bring-up sequence with its values, and the fault-drain
     loop.

   Home: DRM-DRIVER.md §4.
3. **Firmware interface details:**
   - the host-control word indexes;
   - the power-off values and the cleanup type values;
   - the cache-control bits;
   - the GETMISCINFO layout and the core clock;
   - the TA command's `CMDTA_SHARED` offsets and flags;
   - the render-details words +0xd0, +0xd8, +0xdc, +0x104, +0x110, +0x114;
   - tail +0x24c;
   - the firmware file's limits.

   Home: UKERNEL.md, UAPI.md §4, STATE.md §1.
4. **The services bridge ABI** that `winsys/sgx545_pvr.c` uses:
   - the calls and structures: `ALLOC_DEVICEMEM`, `WRAP_EXT_MEMORY`,
     `SGX_DOKICK` with its `SGX_CCB_KICK` fields,
     `SGX_FLUSH_HW_RENDER_TARGET`, `PVRSRV_SYNC_DATA`;
   - the allocation attribute bits (`SGXWS_ATTRIBS_COLOR` 0x10000003, bit 28);
   - the kernel build defines.

   No doc states them. Home: WINSYS.md.
5. **Hardware rules that live only in code comments.** Each needs a doc
   statement with its evidence (home: STATE.md, GEN.md, PDS.md, UAPI.md):
   - the final `mov o0` needs mask = 1, or the GPU locks up;
   - a pixel secondary DMA task must `wdf` before it ends;
   - a PDS middle word that under-declares attributes locks up the GPU;
   - 64 / output dwords vertices per task hung; 128 / round4(out), at most 16,
     works;
   - DMAs over 16 dwords lose data;
   - the RT data frame counter and render details +0xdc must advance, or the
     3D locks up on the fourth frame;
   - one render in flight per target;
   - a frame that samples a freed texture locks up the 3D;
   - iterating a set the vertex program doesn't write locks up the GPU, and so
     does the DDK's placeholder colour iteration;
   - a second render context with its own PB slows renders and then locks up.
6. **Formulas the code uses but no doc states** (home: STATE.md, PDS.md,
   GEN.md):
   - DOUTD bits 22-24 = (n - 1) & 3;
   - the PDS middle word: b0-15 PA units, b16-17, b18-24 secondary dwords,
     b25-28 = 12 / units;
   - VDM draw word 2's batch of 16, and word 5's b22-23;
   - the pixel-event program's data words and its two tasks;
   - the state-emit and phase USSE programs;
   - the vertex driver constants.
7. **Textures** (home: TEX.md, STATE.md §5, PDS.md):
   - the raw channel order the sampler returns for T1 formats 0x0c, 0x0d and
     0x05;
   - T3 = 0x00800800 for anything but a single 2-D sample;
   - the cube face stride, and NPOT cube faces;
   - the DOUTI of a non-dependent cube read;
   - the F16 smp coordinate layout, and skipinv on smp;
   - what TSP word 1 means.
8. **lowp C10 arithmetic**, the compiler's colour path (home: ISA.md):
   - sop3 / sop3l / sopwm on C10 values (only sop2's formula is measured);
   - the C10 alpha of the internal registers;
   - the `nosched` rule for internal registers;
   - frcp / flog2 / fexp2 with a C10 source and `f16d`;
   - `pck.f16.c10` / `pck.c10.f32` / `pck.c10.c10`: scale, lanes and dmask per
     channel;
   - the C10 constant bit layout, and the rounding a writer must use.
9. **The fragment SA block sa0-sa8** needs a register map. Only sa3/sa4
   (discard) and sa5 (front face) are stated, and the front face without
   saying in which space. Home: ISA.md "Fragment shaders" and STATE.md.
10. **Limits** (home: STATE.md, UAPI.md):
    - the VDM index count, and partial primitives;
    - the guard band and the render-target row pitch;
    - the shader and screen caps: 16 attributes, 64 temps, 8 samplers, 2048
      texture size, LOD bias 16;
    - LIMITS_GET's TA kick count: 64 reported, 2 accepted.

## Doc conflicts

Places where two docs, or a doc and the code, disagree; one side needs
fixing.

- **Tail-pointer size.**
  - STATE.md §6's `max(mx,my)² × 4` gives 0x100 at 720×480, but its own value
    is 0x1000.
  - UAPI.md squares the padded tile count; the kernel driver rounds the side
    up first.
- **Vertex secondary constants:** STATE.md §2 has 2⁻¹⁵ and 2⁻¹⁶; GEN.md and
  the code have 2/65535 and 1/65535. *Resolved:* 2/65535 and 1/65535, read from the DDK's captures (STATE.md §2).
- **Vertices per task:** GEN.md says 64 / output dwords; the code and
  DRIVER-UPSTREAM.md say 128 (64 hung).
- **PDS program alignment:** GEN.md and UAPI.md say 32 bytes; the winsys uses
  0x40, and its comment says 32.
- **Colour-mask sopwm:** STATE.md §3 and ISA.md "Phases" shift the mask by 11
  (`<< 11`), but ISA.md "sopwm" stores x y z w in h12 h13 h14 h11.
- **Background block words 3-8:** STATE.md §6 calls them a rotation matrix;
  TEX.md §10 measures them as texture coordinates.
- **CCB 0x10420:** STATE.md §6 reads 0x420 as control; the code reads it as
  room past the ring. *Resolved:* it is room past the ring (STATE.md §6, HW
  render context word 3 and "The command ring").
- **RT data header word 1:** STATE.md §6 says it is 1; the clients and the
  kernel driver leave it at 0. *Resolved:* it is a counter the microkernel
  advances, and its starting value doesn't matter (STATE.md §6 "RT data").
- **`phas.i` target origin:** ISA.md's builder table says it counts from the
  code heap, while UAPI.md and ISA.md's `ba` say from the code base the task
  runs from. The two differ past the first 512 KB.
- **TA command +0x20:** UAPI.md §4.1 says it is 0; the kernel driver writes
  the frame index there.
- **`share_kernel_heaps`:** DRM-DRIVER.md §4 says it maps the kernel heaps
  only under the DDK microkernel; the code maps them for any image without
  the secure bit.
- **Render completion:** DRM-DRIVER.md §5 lists it as still to come; the
  code does it.
- **Transfer tile limits:** 127 / 255 don't follow UAPI.md §3.2's rule.
- **Event program:** SRVINIT.md `-o` describes the open microkernel's event
  program as DOUTD + DOUTU; UKERNEL.md 4a says a bare DOUTU.
- **Directory-list base:** UKERNEL.md §6 says it is "shifted"; the code writes
  the raw physical address. *Resolved:* raw is right; SGX545 has no 36-bit MMU
  (UKERNEL.md §6 "The register map").
- **Discard phase-0 `phas.i`:** ISA.md lists `fa440800|t`; the winsys's phase
  word is `[wait=1]`.
- **Code comments behind the docs:**
  - `sgx545_draw.c:244` says the hardware has no fans; STATE.md has VDM prim 0
    + b28.
  - `sgx545_draw.c:1009` tags sa4 as a guess; STATE.md confirms ALWAYS (a7).
- **DRIVER-UPSTREAM.md §2 behind the docs:** it still lists gl_FragCoord,
  gl_PointCoord, the pass types and sa3/sa4 as gaps.

## Filled since the map was made

Spec-side additions on 2026-11-04 (render state / TA3D). The row tables and
the summary counts below haven't been re-tallied yet. The code check should
re-read these rows against the sections named:

- **Kernel: "TA command and render structures"**, all except the
  tail-pointer formula and which TA-data blocks are per entry:
  - STATE.md §1: the TA flags bit table, from the DDK microkernel's tests;
    +0x1c b14 and up; +0x20; +0x24c b22; the CMDTA_SHARED offsets; the HW
    device sync list;
  - STATE.md §6: render details +0xd0..+0x148; the RT data header [0]/[1],
    the entry alternation and the renders in flight; the command ring.
- **Kernel: "Host control block and firmware commands"**: UKERNEL.md §1 has
  the host-control word table, the flag bits and the timer values. §3 has
  the doorbell and kicker, the `ui32CacheControl` bits, the power and
  cleanup values, and the GETMISCINFO layout. The open gap is the CE5300
  core clock, which is unmeasured.
- **Kernel: "Register map"**: UKERNEL.md §6 "The register map", except the
  registers safe to read on an unreset core and the BAR0 size (DRM-DRIVER.md).
- **Winsys: "TA command", "Render details", "CCB", "Sync data"**:
  STATE.md §1 and §6, as above. Render details +0x110 stays [G]: the
  microkernel doesn't read it.
- **Winsys: "RT allocations and RT data"**: the RT data part is filled
  (STATE.md §6). The allocation order is not.
- **Gallium: `get_rt`**: STATE.md §6 "Renders in flight", with what a third
  kick does still open.
- **Gallium: `sgx545_screen.h:52-59`**: UKERNEL.md §9 (two contexts sharing a
  PB work; a second PB per context is still a gap).
- **Winsys: "Vertices per task", "Vertex secondary attributes"**: STATE.md §2
  (the rule, the capture table, the hang, the slot layout and the constants).
  Where the uniforms start stays G: no capture on disk has vertex uniforms.
- **Winsys: "`sgxws_draw` TA block", word 1**: partly. STATE.md §3 "PDS words"
  has b0-15, b16-17 and the lockup. b25-28 is still a gap: the captures on
  disk don't fit the reported `12 / units` rule.
- **Winsys, from the three DDK captures still on disk (base4, e1tex, vp5;
  2026-11-08):**
  - "State-emit programs" (the program text) and "TA terminate" (its task is
    the 2-dword state emit; the DOUTA values' meaning stays G): STATE.md §2.
  - "DOUTD control and chunking": b22-23 = (dwords - 1) & 3 holds for every
    secondary DMA in the captures, so it is a field of the DMA, not a DDK
    habit (STATE.md §2). The 16-dword limit stays in PDS.md's list.
  - "End-of-tile program" (`phas.i [wait=7]`, temps 4) and "Pixel-event
    program" (data, code and its three tasks): STATE.md §4. What each in1 bit
    is stays G.
  - "Background object" T3 `00800000`: STATE.md §5.
  - "Vertices per task": corrected. The rule holds for compiled shaders;
    the DDK's built-in clear programs run 8 per task (STATE.md §2).
- **Still open in STATE.md §2-§5, needing new captures or hardware pokes:**
  - the PBE stride encoding (word 2 = 0 for stride 1) and word 2 b15;
  - TSP word 1;
  - VDM draw words 2 ("batch") and 5 (n);
  - PDS word 1 b25-28;
  - the background T0 b21-26 and PDS middle word `00030001`;
  - the clear's vertex-program output order;
  - the subtract modes of constant-colour blending;
  - the terminate's DOUTA meanings;
  - the in1 bits of the pixel event.
- **New:** UKERNEL.md §5 "Where the DDK microkernel raises the host
  interrupt": every SW_EVENT site and what triggers it, the fact that no
  completion waits for the timer, the hardware-done to IRQ times, and where
  the open microkernel differs. UKERNEL.md §6 "Parameter-buffer / DPM setup"
  is rewritten from the DDK's code: the free-list loads, the counter save,
  0x63c, and the DPM modes. A fresh scene clears and doesn't load.

## Code against docs

Places where the code, not the docs, falls short. They are listed for the code
pass that follows; nothing is changed here.

- **C10 uniforms past sa63:** the compiler doesn't keep them below sa64, but
  ISA.md "Format control" says operands can only name registers 0-63 while
  C10 is on.
- **i<n> in slots without the i bank:** written as r(60 + n). That is right
  only because every such operand is C10-flagged.
- **idx in the generic destination slot:** the slot allows it, but ISA.md's
  pck destination list doesn't. Only ima16 writes idx.
- **USSE assembler notation:** it doesn't yet take all of ISA.md's notation:
  efo `i=`, dot `cs=`/`as=`, `firh.s8`, st `am3` `.q`, rlp `unused2=`, fdual
  `sd=`/`sb=`/`sc=`.
- **fcos with both f16 and c10:** h7 is encoded wrong, because the operand
  struct holds one format.

## By area

### gallium driver: `mesa/src/gallium/drivers/sgx545`

Facts that live behind `sgxws.h` (winsys words) are in the winsys section;
`ci/` (test expectations) is not audited.

#### Table

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `meson.build` | build list | | n/a: no hardware facts |
| `sgx545_public.h` | screen entry point | | n/a: no hardware facts |
| `sgx545_program.h` | prototypes | | n/a: no hardware facts |
| `sgx545_texture.h` | prototypes (cites TEX.md §1, STATE.md §5; the facts are in `sgx545_texture.c`) | | n/a: no hardware facts |
| `sgx545_screen.h:15-42` | debug flags | | n/a: no hardware facts |
| `sgx545_screen.h:52-59` | one HW render context per process: a second render context with its own parameter buffer makes the microkernel's renders slow, then lock up | UAPI.md §2 "CREATE_CONTEXT" (one render context per VM until tested, G15), UAPI.md §10 gap table G15 (a PB away from the 3D-params heap base hangs; one shared PB per VM), UKERNEL.md §3 "Cleanup (type 5)" | partial |
| `sgx545_context.h:30-40, 57-60` | constants in SA from a base slot, `SGX545_MAX_CONSTS` (sa127 ends them); memory block per draw, base register for block j = address + 512·j | ISA.md "What a compiler needs" → "Constants past sa127"; ISA.md "hw 1d: ld" → **Address** (`ld.a.d #i` = base + 4·(i+1), 128 dwords per base) | covered |
| `sgx545_context.h:32-34` `struct sgx545_const` fmt codes 1/2/3/0x10/0x11 | constant kinds (compiler interface) | COMPILER-UPSTREAM.md §3 | n/a: compiler ABI (encodings are in `fill_consts`, below) |
| `sgx545_context.h:44-46`, `sgx545_draw.c:1002-1005`, `sgx545_program.c:563` | `SGX545_SA_FRONT` = sa5; value `!front_ccw` (bit 0 clear when gallium's front_ccw) for the compiler's gl_FrontFacing | ISA.md "What a compiler needs" → "Fragment shaders" (g16 bit 0 = CW in memory; `xor r, g16, sa[front_sa]`; "Not yet drawn by our own programs") | partial |
| `sgx545_context.c:20-50, 176-184` | fences by kick number: renders on the one render context finish in kick order | TEX.md §4 "Render to texture" (the microkernel runs 3D renders in kick order on the context) | covered |
| `sgx545_context.c:82-85` | hangabort: stop before "the recovery loop wedges the GPU" | UAPI.md §8 "Hardware recovery as userspace sees it" (device lost; TEX.md §7's failed recovery needing a power cycle), PDS.md "Open questions" (a failed recovery needs a reboot) | partial |
| `sgx545_resource.h:36`, `sgx545_resource.c:26-47, 112, 120, 137, 211, 237` | render targets that aren't textures: rows of a multiple of 32 px (`align(width0, 32)`); scanout dumb buffers widened to 32 px "rows of whole tiles"; imported/dumb stride must be a multiple of 128 bytes | STATE.md §4 "PBE words" (stride field (pixels)/2 − 1; window pitch 720 → 736), "Screen and tiles" (pbuffer PBE stride = width rounded up to 32; TE tile 32×16) | partial |
| `sgx545_resource.c:140-141, 174-175, 520-521, 559-560`; `sgx545_draw.c:659` | BO alignment 0x1000, sizes rounded to 4096 | UAPI.md §5 "Heap layout" (page size 4 KiB; T2 and PBE address are full addresses), STATE.md §4 "PBE words" word 3 (`& ~3`) | covered |
| `sgx545_resource.c:102-132` | cached dumb buffer as target: kernel flushes the CPU cache around every job that lists it (SGX doesn't snoop) | UAPI.md §2 "CREATE_BO, GET_BO_MMAP_OFFSET", WINSYS.md "What each backend does" (shared buffers) | covered |
| `sgx545_resource.c:169-184` | textures in the texture heap; a 4-byte format without alpha has its X byte read as alpha, so the device copy starts all 0xff | TEX.md §1 "Texel formats" (RGB uploaded as R,G,B,0xff under 0x0d), STATE.md §5 T1 (0x0d "RGB888 is uploaded as 8888") | covered |
| `sgx545_resource.c:193-224` | fbdev wrap: page-aligned BGRA mapping, pitch 32 px | TEX.md §6 "Presentation" (fb0 wrapped, pitch 736 = 720 rounded to 32), §12 | partial (as the 32-px row above) |
| `sgx545_resource.c:368-407` `sgx545_resource_target` | BGRA8 target: PBE format 0, load-object T1 format 0x0c; RGBA8: PBE format 1, T1 0x0d ("swaps R and B against 0x0c: the PBE's order back"); a texture as target: `va` = the level/face offset, stride = w (twiddled, T1 layout 0) or align(w, 8) (linear, layout 3); other targets: strided T1 layout 6 at the RT's pitch, level 0 only, 4-byte formats only | STATE.md §4 "PBE words", "Format codes"; TEX.md §4 "Render to texture", §11 "Render targets the sampler reads directly"; STATE.md §3 "Scissor and partial viewports" (load object T1 layout 6, format 0xc) | partial |
| `sgx545_resource.c:345-353` | a texture freed while a recorded frame still samples it: that frame reaching the GPU after the free locks up the 3D, so the frame is flushed and the GPU idled before the free | none | missing |
| `sgx545_resource.c:409-608` transfers, ghosting, `tex_write`, `alt_dirty` | CPU-copy bookkeeping | | n/a: no hardware facts |
| `sgx545_resource.c:552-584` `sgx545_resource_cube0` | level-0 face copy for cube maps sampled without mipmapping | TEX.md §8 "Cube maps" → "Mip levels sampled without mipmapping" | covered |
| `sgx545_texture.c:25-60` `sgx545_tex_format` | T1 codes: RGBA8/RGBX8 (+sRGB) → 0x0c, BGRA8/BGRX8 (+sRGB) → 0x0d, B5G6R5 → 0x05, A8 → 0x19, L8 → 0x1a, L8A8 → 0x1b; 0x0c over R,G,B,A bytes gives texels in GL order through the compiler's xyzw unpack; 0x0d swaps R and B against 0x0c | TEX.md §1 "Texel formats"; STATE.md §5 T1 b24-28 | partial |
| `sgx545_texture.c:62-72, 145-156, 241-246` | linear layout for non-power-of-two sizes: rows padded to 8 texels | TEX.md §1 "Linear: sizes that are not powers of two, T1 layout 3" | covered |
| `sgx545_texture.c:23, 130-191, 248-264` `encode_box`, `sgx545_tex_decode_level` | twiddled layout: square tiles of t = min(w, h, 16), Morton order inside with y in bit 0, tiles row-major; level bytes = w·h·bpp | TEX.md §1 "Twiddled: power-of-two sizes, T1 layout 0" | covered |
| `sgx545_texture.c:74-84, 114-124` `chain_offset`, `sgx545_tex_offset`, `sgx545_tex_size` | mip level n+1 right after level n, no padding, each level by its own size | TEX.md §1 "Mipmaps" | covered |
| `sgx545_texture.c:86-112` `face_stride_levels` | cube faces in GL order; packed when width < 16; with levels > 1 each face's chain aligned to 0x800 in every format; one level and width ≥ 16: aligned to one tile's bytes (16·16·bpp; RGBA8888 16×16 = 0x400 measured, other formats [G]) | TEX.md §8 "Cube maps" | partial |
| `sgx545_texture.c:86-124` with `sgx545_draw.c:892-904` | NPOT cube faces: laid out as linear faces (pitch 8) at the same strides, sampled as T1 layout 2; sizes ≥ 16 texels or mipmapped are not drawn because they lock the 3D | TEX.md §8 "NPOT faces" (no NPOT form of layout 2 known); PIGLIT.md "Open bug: NPOT cube maps lock the 3D" | partial (lock-up covered; the layout below 16 texels is missing) |
| `sgx545_texture.c:139, 151-153, 183-184` | RGBX8 uploads get 0xff in byte 3 | TEX.md §1 "Texel formats" | covered |
| `sgx545_texture.c:210-218` `sgx545_tex_uses_cube0` | a cube with levels sampled with no mip filter reads faces spaced by level 0 alone | TEX.md §8 → "Mip levels sampled without mipmapping" (T0 b18) | covered |
| `sgx545_texture.c:267-280` `wrap` | 0 repeat, 1 mirror, 2 clamp (CLAMP, CLAMP_TO_EDGE, CLAMP_TO_BORDER all 2) | STATE.md §5 T0 b3-5 / b6-8 | covered |
| `sgx545_texture.c:289-293` | view from a later base level of a 2-D texture: T2 = that level's offset, T1 size = that level's size | TEX.md §1 "Mipmaps" (T2 is the base; chain length implied by the size); STATE.md §5 T2 | partial |
| `sgx545_texture.c:294-298` | no mipmapping on a linear (NPOT) texture: it locks the 3D | PIGLIT.md "Results (EA3, 2026-11-03)" (NPOT 2D textures sampled through their mipmaps lock the 3D) | covered |
| `sgx545_texture.c:300-312` T0 | wrap T b3-5, wrap S b6-8, mip linear b9, min linear b10, mag linear b12, b17-20 0xf / 0xd with mipmaps, b21-26 0x3f, b30 for L8/A8/LA88 | STATE.md §5 "Texture words, measured" (T0 table; b21-26 [G]) | covered |
| `sgx545_texture.c:313-318` T1, T2 | T1 = layout (2 cube, 0 twiddled, 3 linear) << 29 \| format << 24 \| (w−1) << 12 \| (h−1); T2 = base (or the cube0 copy) | STATE.md §5 T1, T2; PDS.md "DOUTT" | covered |
| `sgx545_texture.c:319-335` strided render target | T1 layout 6, format 0x0d for B,G,R,A memory and 0x0c for R,G,B,A; T0 = (pitch−1) low 3 bits in b9-11, the rest in b17-26, wraps, mag linear b12 | STATE.md §5 T1 layout 6; TEX.md §3 "The transfer queue" (pitch split), §10 "Sources and filters" (strided sources sample nearest) | partial |
| `sgx545_texture.c:336-337` T3 | 0x00800800 for every sample (2-D, cube, projected, merged, dependent) | STATE.md §5 T3 (depends on the sample instruction: tex1 00800800, tex2 01028800) | partial |
| `sgx545_state.c:30-56` `blend_sel` | GL factors as sop2 selectors zero/s1/s2/s1a/s2a/asat + complement; s1 = shader colour, s2 = tile; SRC_ALPHA_SATURATE = asat when the buffer has alpha | STATE.md §3 "Blending and the colour mask are shader code"; ISA.md "sop2 (hw 10 …)" | partial |
| `sgx545_state.c:58-67` `blend_swap_sel`, `:153-168` | reverse subtract by trading sources (`sop2 o0, o0, pa<c>`) and renaming selectors; asat not swappable | STATE.md §3 "Blending …" (Reverse subtract) | covered |
| `sgx545_state.c:69-80, 124-126` `blend_op` | ops add/sub/min/max; min and max run with both factors one | ISA.md "sop2" (cop/aop 0 add 1 sub 2 min 3 max; `dst = (src1*c1) cop (src2*c2)`), STATE.md §3 | covered |
| `sgx545_state.c:89-146` `blend_bfactor`, sopwm form | constant factors: the sopwm products and an add; reverse subtract / subtract with constants by `swap` and `atrade`; SRC_ALPHA_SATURATE alpha factor 1 | STATE.md §3 "Constant-colour blending" | partial |
| `sgx545_state.c:177-197` | blend state object | | n/a: no hardware facts |
| `sgx545_state.c:206-447, 449-553, 602-697` | CSOs, queries, CPU blit | | n/a: no hardware facts |
| `sgx545_state.c:558-600` `sgx545_blit_gpu` | GPU blit needs: a sampleable source, a destination that is a 4-byte linear render target | TEX.md §3, §11; STATE.md §4 | covered |
| `sgx545_draw.c:28-49` `get_rt` | a target's TA/3D structures have one render in flight, so two targets rendered back to back need a set each | STATE.md §6 "RT data" (nbuf = 2 entries) | missing |
| `sgx545_draw.c:103-118` clear mask | colour bytes written: B b0, G b1, R b2, A b3 of packed BGRA8 | STATE.md §3 "Blending …" (colour mask sopwm, one bit per byte) | covered |
| `sgx545_draw.c:123-124` | every render starts at depth 1.0 (render details +0x40), so a clear to 1.0 draws nothing for depth | STATE.md §6 "Render details" +0x40 | covered |
| `sgx545_draw.c:125-130, 925-945` | a buffer without alpha: its X byte is read as alpha wherever it is sampled; clears write 1.0 there; draws into such a texture keep the tile's alpha | TEX.md §1 "Texel formats" | covered |
| `sgx545_draw.c:137-146` `depth_func` | ISPA depth compare = GL enum − 0x200 (PIPE_FUNC order = GL order); 0 when off | STATE.md §3 "ISPA" b22-24 | covered |
| `sgx545_draw.c:148-160` `stencil_word` | ISPC: write mask b0-7, compare mask b8-15, zpass b16-18, zfail b19-21, sfail b22-24, func b25-27; op codes 0 keep … 7 decr_wrap | STATE.md §3 "ISPC: stencil" | covered |
| `sgx545_draw.c:162-182` `build_stencil` | two-sided: the first group is the faces CCW in the target's memory (GL's back face with front_ccw in a y-flipped target) | STATE.md §3 TA state block header bits 0x8/0x10/0x20 | covered |
| `sgx545_draw.c:184-205` `build_state` | ISPB colour mask R 8, G 4, B 2, A 1; cull, front_ccw, viewport scale/translate, line width (the winsys words) | STATE.md §3 "ISPB", "MTE control", "Viewport", "Lines and points" | covered |
| `sgx545_draw.c:207-230` `build_clip` | the hardware clips to the guard band, not to the viewport; the viewport rectangle ∩ scissor is what the draw may reach | STATE.md §3 "Scissor and partial viewports: pass type 3 objects" (item 8: a partial viewport gets reset + set objects) | partial |
| `sgx545_draw.c:244-298, 1090-1119` primitives | hardware prims used: triangles, triangle strip, lines, points; fans, line strips/loops, quads, polygons drawn as lists; comment: "primitives the hardware doesn't have" includes the fan; points need a vertex-program size (else nothing) | STATE.md §2 draw word 0, §3 "Lines and points" (prim 0 + b28 is a fan; no line strip or loop; pt1 draws nothing) | covered (the code comment contradicts STATE.md on fans) |
| `sgx545_draw.c:488-544` `fill_consts` | constant encodings: f32; F16 half at a shift; C10 = clamp(lrint(clamp(v, −2, 2)·255 in double), −512, 511) & 0x3ff at a shift; kind 0x10 = F16 of c/255; F16 and C10 values sharing an SA word | ISA.md "Float classes" (h8 C10: signed 10-bit channel at 10·chan, read as clamp(v/255, ±2)); ISA.md "Hardware checks" (hwt_p3, hwt_p4); ISA.md "sop2" (C10 channel layout in a register) | partial |
| `sgx545_draw.c:546-566` `mem_consts` | constants past SA in a per-draw block, n base registers | ISA.md "Constants past sa127", "hw 1d: ld" Address | covered |
| `sgx545_draw.c:568-593, 1178-1185` `varyings_match` | DOUTI source 0 = gl_FragCoord, set + 1 = a varying, ≥ 0x20 not a varying (0x22 the discard's depth plane); sample half = coordinate set + 1, 2 comps for 2-D, 3 for projected; TSP sizes 3 bits per set, 5 = 4 components, sets < 10; iterating a set the vertex program doesn't output (or with more components) locks the GPU (EA3) | PDS.md "DOUTI"; STATE.md §3 "Output select … and TSP words"; ISA.md "Vertex shaders" (10 sets) | partial |
| `sgx545_draw.c:596-838` vertex cache | 16-bit indices rebased into a < 65536 range; vertices aligned to 64 bytes, indices to 16; 2 MB chunks | STATE.md §2 draw word 1 (16-bit indices: bit 0 clear); UAPI.md §5 constraints (index buffer 2- or 4-byte aligned; DMA sources on 32, G10) | covered |
| `sgx545_draw.c:738, 1218-1250` index limit, whole primitives | draws split at `SGXWS_MAX_INDICES` (strips longer than it become lists; lists in pieces of whole primitives); counts trimmed to whole primitives because "the VDM takes the count as it is" | STATE.md §2 draw word 0 (`… \| prim<<26 \| index count`) | partial |
| `sgx545_draw.c:887-908` | NPOT cube maps of ≥ 16 texels, or mipmapped, lock the 3D: not drawn | PIGLIT.md "Open bug: NPOT cube maps lock the 3D" | covered |
| `sgx545_draw.c:916-951` colour epilogue | blend, then the colour mask as bytes of packed BGRA8 (B b0 G b1 R b2 A b3); for a texture target without alpha the blend's alpha is the tile's (tile·1 + colour·0) | STATE.md §3 "Blending …" (sop2, sopwm mask), ISA.md "sop2" | covered |
| `sgx545_draw.c:986-987` pass type | blend 1; discard 4; discard + blend 2; else 0 | STATE.md §3 "ISPA" b25-27; ISA.md "Phases, visibility feedback and discard" → "The draw state" | covered |
| `sgx545_draw.c:989-1001`, `sgx545_program.c:714, 785` fragment SA block | fragment constants from sa9 (`fsk + 9`); `sec_dwords` = 9 + constants, at least 9 when a driver word is used; blend colour in sa2 as A<<24 \| R<<16 \| G<<8 \| B; temps ≥ 2 for the constant blend | STATE.md §3 "Constant-colour blending" (sa2, packed BGRA8, `604080bf`); ISA.md "Constants past sa127" (from sa9/sa10) | partial |
| `sgx545_draw.c:1006-1013` discard words | sa3 = 0xe000 \| (GL depth func − 0x200, 7 with the test off) \| 8 without depth writes; sa4 = 0xee00 (code tags it [G]) | STATE.md §3 "Alpha test (ES1)" (sa0/sa1 word formats; "the Mesa driver's discard atst8 (`f9340406`, sa3/sa4) carries these same two words with the alpha function ALWAYS (`0xee00`)"; ALWAYS [C, a7]); ISA.md "Phases …" (atst8 src1 = 3, src2 = 4) | covered |
| `sgx545_draw.c:1014-1031` dependent samples | the 4 DOUTT words of a dependent sample in 4 consecutive SA registers | ISA.md "The texture-sample sequence a compiler must emit" | covered |
| `sgx545_draw.c:1039-1053` | one DOUTT (4 words) after each sampling DOUTI | PDS.md "DOUTT", STATE.md §5 | covered |
| `sgx545_draw.c:1085-1088` fixed uniform-colour program | colour as two F16 pairs at `SGXWS_FS_UNIFORM_OFF` of its SA block | GEN.md "The API" (a fragment shader that writes a mediump uniform vec4), "What a frame is made of" (fragment uniforms FP16) | partial |
| `sgx545_draw.c:1120-1133` | compiled programs: 4 dwords per attribute slot; fixed position programs: 2 (x, y) or 3 (x, y, z) dwords | PDS.md "DOUTD"; GEN.md "The API" (xy or xyz position shader) | covered |
| `sgx545_draw.c:340-460, 462-486, 1189-1325` | CPU vertex gathering, staging, sparse expansion | | n/a: no hardware facts |
| `sgx545_program.c:35-261, 353-367, 433-516, 805-1010` | NIR plumbing, legacy varying remap, variants, fixed-program matching | | n/a: no hardware facts |
| `sgx545_program.c:273-351` `assign_varyings` | a set per slot in slot order; gl_PointCoord is set 0 (2 components) of a sprite and everything else moves up one; the point size is the output dword after the sets; ten sets of four components | STATE.md §3 "Lines and points"; ISA.md "Vertex shaders" (output buffer, varyings to sets, gl_PointSize) | covered |
| `sgx545_program.c:383-416` `insert_samples` (smp, wdf) | smp template patched: dst l21-27 = raw texel temp, h7 clear (r); src1 l7-13 = the state SA, bank l30-31 = 3 (sa), h17 clear; then `wdf` = hi `0xf9200000 \| drc`, lo 0 | ISA.md "Operand slots and bank codes", "hw 1c: smp", "The smp instruction …", "The texture-sample sequence …", "hw 1f: the special class" → "other" (op 1 wdf, h0 DRC; h20-21 = 2) | covered |
| `sgx545_program.c:417-427` | branch relative offsets in l0-17, signed, in instructions, moved when samples go in | ISA.md "hw 1f" → "flow control" (`br`), "Program structure and calling conventions" | covered |
| `sgx545_program.c:613-630` | an instruction names SA in 7 bits; vertex constants from sa10, fragment from sa9, plus 4 per dependent sample, must end at sa127; past it the numbers wrap and the 3D locks up | ISA.md "Registers", "Constants past sa127"; PIGLIT.md "Uniforms past sa127" | covered |
| `sgx545_program.c:634-657` vertex program words | `in_dwords` = 4·attribute slots; `out_dwords` = 4 + set sizes + 1 with a point size; TSP sizes 3 bits per set (1, 2, 3, 5 for 4); TSP word 1 = 0 except bit 0 for a sprite's coordinate set ("with a set's bit in it, the TPU samples at a constant coordinate") | STATE.md §3 "Output select … and TSP words", "Lines and points" (ES2 point draws: TSP words 2 and 1); ISA.md "Vertex shaders" | partial |
| `sgx545_program.c:659-691` | dependent samples' state words in SA after the constants: sa(9 + constants + 4j) | ISA.md "Constants past sa127", "The texture-sample sequence …" | covered |
| `sgx545_program.c:692-702` | a discarding program must end in the feedback `nop` (hi `0xf8000140`, lo 0) | ISA.md "hw 1f" → "flow control" (`nop` h6-8 = 5), "Phases, visibility feedback and discard" (USP's discard `nop` at the feedback label) | covered |
| `sgx545_program.c:715-759` DOUTI building | one DOUTI per iteration in PA order; varying source = set + 1; gl_FragCoord (iterator 14) → source 0; sample half merged into the previous plain DOUTI when it iterates the same set; a separate DOUTI sampling a set already iterated samples at coordinate 0 (EA3); otherwise a pure sample (source 0x24 = 0x12000); b25-26 = 3; F16 iteration (`prec1`, b28); w1 0x10 for gl_PointCoord; no placeholder colour iteration (source 0x20): it hangs the GPU when the vertex program outputs no colour | PDS.md "DOUTI"; ISA.md "Lists and blocks" (list A); ISA.md "What a compiler needs" → "Fragment shaders" | partial |
| `sgx545_program.c:760-787` | discard: one more plain DOUTI, source 0x22, 3 components, after the inputs; pa index = registers the iterations fill (an F32 component 1, an F16 pair 1, a sampled texel 1); `pa_dwords` ≥ that + 3 | ISA.md "Phases, visibility feedback and discard" (`0x7811000`, pcoeff pa<n>), "What a compiler must emit to discard"; ISA.md "Lists and blocks" (registers per list-A entry); ISA.md "The texture-sample sequence …" (one chunk for any ≤ 32-bit texel) | covered |
| `sgx545_program.c:790-793` | last plain iteration b29, last sample b30 | PDS.md "DOUTI" | covered |
| `sgx545_program.c:703-708` | gl_FragDepth not written (depthf form not traced) | ISA.md "Program structure …" (gl_FragDepth guess) | n/a: no fact used (warning only) |
| `sgx545_screen.c:24-53` debug options | flushrt: `SGX_FLUSH_HW_RENDER_TARGET` hangs a srvinit-started microkernel | GEN.md "Layout: the microkernel reads the previous client's render"; NOTES.md "A frame from code alone" | covered |
| `sgx545_screen.c:87-126` shader caps | VS 16 inputs, FS 10 inputs (sets), VS 11 outputs, 64 temps, 8 samplers, 16384 instructions, constants sa9 (sa10 vertex) to sa127, mediump two to a register | ISA.md "Registers", "Constants past sa127", "Vertex shaders" (10 sets) | partial |
| `sgx545_screen.c:93-98, 285-292` | a non-dependent (PDS) read needs the coordinate to be a whole varying of its own set; a projected one a whole vec4 at component 0 | ISA.md "The texture-sample sequence …" (non-dependent fragment read: coordinate = an untouched varying); PDS.md "DOUTI" sample half | partial |
| `sgx545_screen.c:129-174` screen caps | max 2-D texture 2048, cube levels 12, no 3-D textures, 1 render target, line width 1-16, point size 1-511, granularity 0.1, LOD bias 16, fragment coord origin upper left at half-integer centres, PCI 8086:089b, no fixed-function blender | STATE.md §3 "Lines and points" (line width clamp [1, 16], ISPB `ff`; points clamped to 511), §3 "Blending …"; PDS.md "DOUTI" (gl_FragCoord at pixel centres in memory order); STATE.md §4 "PBE words" (top row first); DRM-DRIVER.md (8086:089b) | partial |
| `sgx545_screen.c:177-232` `is_format_supported` | windows/scanout: BGRA8 only (PBE code 0); render targets: BGRA8/RGBA8 (codes 0/1), not 565 because T1 0x05 hands the compiler's unpack R and B swapped; depth Z16/Z24X8/Z24S8, kept per tile by the ISP within one render (no ZLS); sampler formats = those with a T1 code, plus render targets sampled strided; 1D/2D/RECT/CUBE targets | STATE.md §4 "Format codes"; TEX.md §1, §3, §4, §7 "Depth and stencil across renders (ZLS)"; PIGLIT.md "Results" (no ZLS) | partial |
| `sgx545_screen.c:243-275` `sgx545_screen_destroy` | under a srvinit-started microkernel a render context's cleanup never completes (bridge −EINVAL after its wait); the next connection's TA then runs against a parameter buffer the microkernel doesn't use: no pages allocated, nothing drawn, ~270 ms renders, then lockups | UKERNEL.md §3 "Cleanup (type 5)" (RT/PB cleanups not completing; a busy resource leaves the status clear) | partial |

Counts (rows with hardware facts): **covered 44, partial 27, missing 2**; 12 rows n/a.

#### Facts the docs need

##### `sgx545_screen.h:52-59` (partial) — UAPI.md §2 "CREATE_CONTEXT" / UKERNEL.md §9
- Under the DDK microkernel, a second HW render context with a parameter buffer of its own (two pipe contexts, gl4es under glretrace) makes renders slow and then locks up. The docs attribute the second-context hang to PB placement (G15: a PB away from the 3D-params heap base); they don't say whether a second, correctly placed PB per context works, or that one shared render context is the safe choice.

##### `sgx545_context.h:44-46` / `sgx545_draw.c:1002-1005` (partial) — ISA.md "Fragment shaders"
- The sense of the front-face SA word in a stated coordinate space: driver writes bit 0 = `!front_ccw` (gallium/GL window winding), i.e. bit 0 = 0 when GL-CCW is front for a y-flipped target. ISA.md says "bit 0 = 1 when counter-clockwise is front" without naming memory vs GL space, and says it is not yet drawn by our programs; the driver comment says it is A/B-checked against the DDK. State the A/B result and the space.
- That sa5 is the driver/compiler convention (`front_sa`), and the fragment driver block's register map (see the SA block row).

##### `sgx545_context.c:82-85` (partial) — UAPI.md §8
- Repeated SGX recoveries in a loop (a client that keeps submitting after a hang) end with the GPU wedged; the docs record a single failed recovery needing a power cycle, not the loop.

##### Render-target pitch (partial) — STATE.md §4 "PBE words" / "Screen and tiles"
- Whether rows of a multiple of 32 px (128 bytes at 4 bpp) are a hardware requirement of the PBE (or of the strided sampler) or only the DDK's choice. The PBE stride field ((pixels)/2 − 1) only forces an even pixel count. The driver rejects imported dma-bufs whose stride is not a multiple of 128 bytes and widens dumb buffers to 32 px "rows of whole tiles".

##### `sgx545_resource.c:368-407` `sgx545_resource_target` (partial) — TEX.md §4 / §11, STATE.md §5 T1
- What raw order T1 0x0c and 0x0d deliver: the load object and the strided sampling read a BGRA8 target with 0x0c (identity, the tile's own byte order) and an RGBA8 (PBE code 1) target with 0x0d, while sampled through the compiler's unpack the choice is the reverse. One table: memory byte order × T1 code → channel order seen by `smp`/DOUTT raw and after `pck` unpack.
- Rendering into a mip level other than 0 and into a cube face: PBE address = that level/face's offset in the texture layout, PBE pitch = that level's width (twiddled) or align(w, 8) (linear), sideband b13 by that level's layout. TEX.md §11 shows level-0 2-D FBOs only.
- That only 4-byte formats can be strided targets (T1 layout 6) and only at level 0.

##### `sgx545_resource.c:345-353` (missing) — UAPI.md §7.5 or §8, or TEX.md
- A frame that samples a texture whose memory was freed before the frame reached the GPU locks up the 3D (st's bitmap cache freed at teardown). State it as hardware behaviour (no fault reported / a 3D lockup), so a driver keeps sampled buffers alive until the render is done.

##### `sgx545_texture.c:25-60` `sgx545_tex_format` (partial) — TEX.md §1 "Texel formats"
- RGBA8 textures (R, G, B, A bytes) sampled with T1 0x0c give GL-order texels through an xyzw unpack (byte-identical to the DDK on the EA3: 16×16, 64×64, NPOT, nearest and linear); 0x0d is "0x0c with R and B swapped". TEX.md lists 0x0d for RGBA uploads (the DDK's choice, with libusc's zyxw reorder folded into the sample) and doesn't say what each code returns to the shader.
- T1 0x05 (RGB565): the channel order it hands the compiler's unpack (R and B swapped against GL, per `sgx545_screen.c:190-192`).
- sRGB: no T1 code decodes sRGB (the driver maps each sRGB format to its linear twin and decodes in the shader); say so with the formats probed.

##### `sgx545_texture.c:86-112` `face_stride_levels` (partial) — TEX.md §8
- Face stride of a one-level cube map ≥ 16 texels: one tile's bytes alignment (16·16·bpp) is measured only for RGBA8888 16×16 (0x400); L8, LA88, RGBA4444 and RGB565 one-level cubes and larger sizes are unmeasured.
- Face stride when only part of the mip chain is allocated (last_level < full chain): measured only for full chains.

##### NPOT cube faces (partial) — TEX.md §8 "NPOT faces"
- The layout the TPU expects for a T1 layout-2 cube whose faces are not powers of two (the driver lays out linear faces at an 8-texel pitch, packed as for POT faces < 16, and draws only those < 16 texels without mipmaps). TEX.md says no NPOT form of layout 2 is known; a capture of the DDK's NPOT cube (or a hardware sweep) is needed.

##### `sgx545_texture.c:289-293` base level (partial) — TEX.md §1 "Mipmaps"
- That T2 may point at any level of a chain and T1 carry that level's size, with the levels after it read as that texture's mips (GL_TEXTURE_BASE_LEVEL). TEX.md implies it (chain implied by the size) but doesn't state or test it.

##### `sgx545_texture.c:319-335` strided T0 (partial) — TEX.md §10 "Sources and filters"
- Whether mag-filter LINEAR (T0 b12) works on a layout-6 (strided) source: TEX.md says strided sources "sample nearest" because the pitch occupies the min-filter bit; the driver sets b12 for a linear mag filter.
- Maximum strided pitch: (pitch − 1) >> 3 must fit b17-26 (pitch ≤ 8192 texels).

##### `sgx545_texture.c:336-337` T3 (partial) — STATE.md §5 T3
- The T3 word for: a cube sample (smp dim 3), a projected PDS sample (DOUTI 0x400), a sample merged into an iteration, and a dependent `smp` reading the words from SA. Only `tex1` (single 2-D sample: 0x00800800) and `tex2` (0x01028800, not decoded) are measured; the driver uses 0x00800800 for all.

##### `sgx545_state.c:30-56` `blend_sel` (partial) — ISA.md "sop2"
- The asat selector's value: min(s1.a, 1 − s2.a) (GL SRC_ALPHA_SATURATE), and that it can't be expressed with the sources traded. ISA.md names it "src alpha saturate" only.

##### `sgx545_state.c:89-146` sopwm constant blending (partial) — STATE.md §3 "Constant-colour blending"
- Subtract and reverse subtract with constant factors: the final sop2's op and the trade of products (`swap`, `atrade`: a subtract on the other channel that runs the other way gets its products in r0 and r1 traded). STATE.md: "Subtract and reverse subtract with constant factors are untested".
- MIN/MAX on one channel with constant factors on the other (factors forced to ONE on the MIN/MAX channel).
- The sopwm selector numbering (1 asat, 2 s1, 3 s1a, 6 s2, 7 s2a) is in ISA.md; the mapping from GL constant factors to the five-instruction form's X/Y operands should be a table.

##### `sgx545_draw.c:28-49` `get_rt` (missing) — STATE.md §6 "RT data" or UAPI.md §2 "CREATE_RT_DATASET"
- One render target's TA/3D structures (RT data, render details, region headers) support one render in flight; two targets rendered back to back (back buffer, then the swap's fb0) need a set each to avoid waiting. STATE.md has nbuf = 2 entries without saying what a second render in flight on the same set does.

##### `sgx545_draw.c:207-230` `build_clip` (partial) — STATE.md §3 "Scissor and partial viewports"
- The hardware's clipping: geometry is clipped to a guard band, not to the viewport, so pixels outside a partial viewport are reached unless pass-type-3 objects (or the region clip) bound them. Item 8 of that section shows the DDK's objects but not the guard-band fact or its size.

##### `sgx545_draw.c:244-298` primitives (covered; code comment out of step)
- No doc fact missing. The comment "primitives the hardware doesn't have" lists the triangle fan, which STATE.md "Lines and points" has as VDM prim 0 + b28 [D]. Either the comment changes or the doc notes why fans aren't used.

##### `sgx545_draw.c:488-544` `fill_consts` (partial) — ISA.md "Float classes" / "Hardware checks", or COMPILER-UPSTREAM.md §3
- C10 encoding from a float: c = round-to-nearest-even of clamp(v, −2, 2)·255 computed in double (0.3·255 = 76.500003 → 77), clamped to −512..511, stored as the low 10 bits at bit 10·chan. ISA.md gives the read side (clamp(v/255, ±2)) but not the rounding a writer must use to match.
- F16 constant encoding: IEEE binary16, round to nearest even from f32 (ISA.md states pck's f32 → f16 truncates; the driver's constants round).
- Kind 0x10: the F16 of a C10 value = c/255 rounded to nearest, as `pck.f16.c10` gives it (ISA.md has `pck.f16.c10` but not its rounding).
- Several F16 halves / C10 channels share one SA word (OR'd at their shifts; the word zeroed first).

##### `sgx545_draw.c:568-593` `varyings_match` (partial) — PDS.md "DOUTI" or STATE.md §3 "Output select … and TSP words"
- A fragment DOUTI that iterates or samples a set the vertex program doesn't output (TSP size 0), or with more components than it outputs, locks up the GPU (EA3; colour placeholder, gl4es neverball shaders).
- DOUTI sources from 0x20 on are not varyings (0x20 colour, 0x22 depth plane, 0x24 = the pure-sample pattern); PDS.md lists 0x20 and 0x22 but not the range rule.

##### Index limit and whole primitives (partial) — STATE.md §2 "The VDM control stream"
- The width of the draw word's index-count field (bits 0-21 below `0x00c00000`?) and the largest count a single VDM draw takes (`SGXWS_MAX_INDICES` in the winsys).
- What the VDM does with a count that isn't a whole number of primitives (the driver trims because "the VDM takes the count as it is").

##### Fragment SA block (partial) — ISA.md "Fragment shaders" or STATE.md §3 "PDS words"
- A register map of the fragment driver block sa0-sa8: sa2 blend colour (STATE.md), sa3/sa4 discard visibility words, sa5 front face (ours), the rest unused; constants from sa9; the secondary DMA covers sa0..sa(8 + constants) whenever any driver word is used.
- Vertex block: constants from sa10 (what sa0-sa9 hold for our programs: the block's own address, the driver constants 2/255, 1/255, 2/65535, 1/65535 per STATE.md §2 and GEN.md "Smaller things", and sa10's point-size word).

##### `sgx545_draw.c:1085-1088` fixed uniform-colour program (partial) — GEN.md
- The fixed fragment program's text and its SA layout: the colour as F16 pairs (R | G << 16, B | A << 16) at `SGXWS_FS_UNIFORM_OFF`.

##### `sgx545_program.c:634-657` TSP word 1 (partial) — STATE.md §3 "Output select … and TSP words"
- The effect of a set's bit in TSP word 1: with it set for a set the pixel program samples (PDS sample), the TPU samples at a constant coordinate; the driver leaves word 1 = 0 for every set except a sprite's coordinate set 0 (bit 0). STATE.md states the DDK's rule (precision ≤ 2) but not what the bit does.

##### `sgx545_program.c:715-759` DOUTI building (partial) — PDS.md "DOUTI"
- A separate (unmerged) sample DOUTI of a set that an earlier plain DOUTI already iterates samples at coordinate 0 (EA3); merge it into that DOUTI instead.
- The DDK's placeholder colour iteration (source 0x20) in a shader without varyings hangs the GPU when the vertex program outputs no colour.
- Projected samples (0x400): PDS.md says "captures only show 2-D"; hardware-check `texture2DProj` through the PDS.

##### Shader caps (partial) — ISA.md "Register files and operand limits" / "Vertex shaders"
- Vertex attribute limit: 16 attribute slots of 4 dwords (PA budget 0x80 minus header).
- Temporaries available to a program: 64 (ISA.md says r0-r123 allocatable, 6-bit numbers under format select); state which limit applies.
- Texture units: 8 samplers per fragment program.
- Maximum program length (16384 instructions; `br`/`ba` reach 18 bits).
- VS outputs: position + 10 sets = 11 vec4 slots.

##### Non-dependent sampling conditions (partial) — ISA.md "The texture-sample sequence …" / PDS.md "DOUTI"
- A PDS sample reads its coordinate from the start of a set: a projected read needs a whole vec4 varying at component 0 of its own set; a varying sharing a set with another (fog before a texture coordinate) forces a dependent `smp`.

##### Screen caps (partial) — TEX.md / STATE.md
- Maximum texture width/height (driver 2048; T1's 12-bit fields allow 4096) and cube size; maximum render-target size.
- LOD bias range (driver 16; LOD bias isn't wired up).
- Line-width granularity: ISPB holds width × 16 truncated (1/16 steps), the driver reports 0.1.
- No 3-D textures known (PIGLIT.md says the DDK desktop GL has `EXT_texture3D`).

##### `is_format_supported` (partial) — STATE.md §4 "Format codes", TEX.md §7
- Which depth formats the ISP supports per tile, and that within one render the format doesn't matter (the stored depth is float32 whatever GL asked; TEX.md §7 shows it for ZLS store only).
- RGB565 as a render target: the PBE code 3 path is proven (TEX.md §4); what keeps it out is the sampling order of T1 0x05 (above), which TEX.md should state.

##### `sgx545_screen_destroy` (partial) — UKERNEL.md §3 "Cleanup (type 5)" or SRVINIT.md
- With a srvinit-started DDK microkernel, a render context's cleanup never completes (the bridge returns −EINVAL after its wait); a new services connection's TA then runs against a parameter buffer the microkernel doesn't use (no pages allocated, nothing drawn, ~270 ms per render, then lockups). The connection and HW context must be kept for the process's next screen.

#### Notes for spec-side sessions

- DRIVER-UPSTREAM.md §2 still lists as gaps several facts the docs now state: gl_FragCoord (PDS.md "DOUTI" source 0, capture 2026-11-02), gl_PointCoord (PDS.md w1 0x10, STATE.md "Lines and points"), pass types 1/2/4 (STATE.md ISPA b25-27), the discard sa3/sa4 table (STATE.md "Alpha test (ES1)").
- `sgx545_draw.c:1009` tags sa4 = 0xee00 [G]; STATE.md "Alpha test (ES1)" has ALWAYS confirmed [C, a7] (with reference byte 0x80; the driver's word has reference 0).
- `sgx545_draw.c:244` comment vs STATE.md on fans (above).
- No row uses a fact that a doc contradicts, apart from the fan comment and the front-face wording.

### winsys: `mesa/src/gallium/winsys/sgx545`

PDS instruction words built through `mesa/src/sgx545/pds` count as covered
by PDS.md. Checked here: the data values, masks, task words and VDM/TA
referrer words around them, and the programs `tools/gen/mkusse.py` emits
into `sgx545_usse_progs.h`. A value that appears only in DRIVER-UPSTREAM.md
(an audit table, not a specification) counts as not specified.

#### Table

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| sgxws.h:49-64 GL enums, VDM prim codes | depth/stencil compare = GL enum - 0x200; VDM prim 0 tri, 1 lines, 2 points, 3 strip | STATE.md §3 "ISPA (ctx+0x780)" (b22-24), §3 "ISPC: stencil"; §2 draw word 0; §3 "Lines and points" | covered |
| sgxws.h:67-77 `sgxws_target` | BGRA8888 top-first; stride "even, or 1"; PBE format code; T1 format/layout 6 strided, 3 linear (pitch rounded to 8 texels), 0 twiddled with the PBE writing twiddled | STATE.md §4 "PBE words", "Format codes"; §5 T1 table; TEX.md §11 "Render targets the sampler reads directly", §4 "Render to texture" | partial |
| sgxws.h:81-103 `sgxws_prog` | temporaries (DOUTU in fours), TSP sizes 3 bits per set, precision mask, point size as last output dword, up to 10 DOUTIs/sets | PDS.md "DOUTU" word 0 b24-31; STATE.md §3 "Output select ... and TSP words"; ISA.md "Vertex shaders" (sets 0-9) | covered |
| sgxws.h:105-149 blend description, `sgxws_bsel`, `SGXWS_BLEND_CONST_SA` 2 | sop2 epilogue form, selector codes zero/s1/s2/s1a/s2a/asat, complement; sopwm colour mask, byte order B b0 G b1 R b2 A b3; constant colour in sa2 | STATE.md §3 "Blending and the colour mask are shader code", "Constant-colour blending"; ISA.md "sop2 (hw 10 ...)" | covered |
| sgxws.h:157-180 `sgxws_state` | ISPB colour mask R8 G4 B2 A1; pass types 0/1/2/4; sprite = object type 4; ISPC words; face 0 = CCW-in-memory group | STATE.md §3 ISPA, ISPB, ISPC tables; header bits 0x8/0x10/0x20 row; "Lines and points" | covered |
| sgxws.h:182-209 `sgxws_draw` | 16-bit indices, `SGXWS_MAX_INDICES` 0xffff; `verts_va` 16-byte aligned, `idx_va` even; constant blocks at base + 512·j into an SA base register | UAPI.md §5 "Index buffer: 2 or 4-byte aligned"; ISA.md "Constants past sa127", "hw 1d: ld" "Address" | partial |
| sgxws.h:270-297 program wrapping summary, builtins | vertex phases + epilogue; fragment `phas`, shader, colour out; secondary `phas`, code, `nop [end]`; FS_UNIFORM FP16 at fs_consts[9..10], 11 dwords | ISA.md "Phases, visibility feedback and discard", "Program structure and calling conventions"; GEN.md "Vertex programs end with an extra phase"; NOTES.md "Findings" (FP16 uniforms); STATE.md "Constant-colour blending" (11-dword DMA) | covered (details in the frame.c rows) |
| sgx545_frame.h:46-59 `enum sgx545_heap` | heap ids 0 general, 1 TA data, 3 kernel data, 4 pixel shader, 5 vertex shader, 6 PDS pixel, 7 PDS vertex, 8 sync, 10 3D params, 14 texture (0xf0000000) as the low byte of the services heap ID | NOTES.md "Findings" ("Heaps (id low byte = SGX_*_HEAP_ID)", bases only); TEX.md §10 setup table (ids 0, 3, 4, 6); NOTES.md "Blob-free replay" (heaps 1, 8, 10 unnamed); STATE.md §5 T2 (texture heap 0xf0000000) | partial |
| sgx545_frame.h:68-76 `heap_base[]`, `usse_base_index[]` | per-heap device base; first code base register of the pixel / vertex USSE heap | UAPI.md §5 "Heap layout" (code bases 0-9, 10-13; `usse_base_index`); SRVINIT.md "Init scripts" | covered |
| sgx545_frame.h:223 `SGXWS_ATTRIBS_COLOR` | services allocation attributes 0x10000003 for a colour buffer | none (SRVINIT.md "Allocations" names 0x4f / 9 only; TEX.md §10 "allocate them without attribs b3") | missing |
| sgx545_frame.c:30-35 `PDS_ALIGN` 0x40, `USSE_ALIGN` 0x80, `DMA_ALIGN` 0x20 | placement of PDS programs, USSE programs, DMA sources | GEN.md "Smaller things" ("Placement"); UAPI.md §5 (G10; code base `addr>>7`) | partial |
| sgx545_frame.c:56-80 `sgxws_f16` | constants as IEEE half, round to nearest even | ISA.md "Hardware checks" (hwt_p1/p4 F16 values), "Hardware constants" (c45 = 0x3c00 pair) | covered |
| sgx545_frame.c:104-121, 170-179 `sgxws_bo_alloc`, `ring_init` | heap per use (render target general, texture heap 14, vertex general); attribs 0x10000003 / 0x3 / 0x1; rings 0x1000-aligned, first 0x100 bytes unused | TEX.md §2 (texture heap, attribs 0x3); SRVINIT.md "Allocations" (attribute names); GEN.md "Smaller things" ("Rings start 0x100 in") | partial |
| sgx545_frame.c:228-241 `sgxws_usse_upload` | `phas.i` targets relocated by (va - USSE heap base)/8: absolute instruction index from the heap base | ISA.md "phase / 0xfe-0x100" (pc l0-17, absolute); ISA.md "What the draw-time builder emits" ("byte address relative to the code heap, /8"); UAPI.md "UPLOAD_CODE" ("absolute from the code base the task runs from") | partial |
| sgx545_frame.c:249-270 `doutu_w0`, `doutu_w1_pixel` | w0 base = (off>>19) + base index, offset in the 512 KB window, temps4 = (t+3)>>2; w1 pixel b2, b24 iterations, b25 samples | PDS.md "DOUTU: start a USSE task (3 words)" | covered |
| sgx545_frame.c:272-278 `usse_limm` | LIMM lo/hi word formula | STATE.md §4 "The end-of-tile program"; ISA.md "other (h20-21 = 2)" op 4 `limm` | covered |
| sgx545_frame.c:282-406 PDS helpers, `pds_emit`, `pds_word` | MOVS/ALU/TST/BRA/SHL/HALT via the encoder; rows of DS0 DS0 DS1 DS1 then code; TA/3D state word `rows<<26 \| (va - PDS pixel base)>>4` | PDS.md "Program layout", "Instruction encoding", "How programs are referenced"; STATE.md §3 "PDS words" | covered |
| sgx545_frame.c:396-399 `clear_douti` | DOUTI iterate source 0x20 (colour), 4 components, plain, last | PDS.md "DOUTI" iterate half (0x20 colour, `21c10000`) | covered |
| sgx545_frame.c:415-451 `pds_pixel` | DOUTU, a DOUTI per input, DOUTT (T0,T1 from a DS1 pair, T2,T3 from a DS0 pair) after a sampling DOUTI | PDS.md "What the DDK generates" (0x1e970), "MOVS" swizzle, "DOUTT" | covered |
| sgx545_frame.c:453-475 `pds_secondary` | optional DOUTD then DOUTU | PDS.md "What the DDK generates" (0x1ed20/0x1ee40); STATE.md §2 "The state programs and what they DMA" | covered |
| sgx545_frame.c:477-515 `dma_ctrl`, `pds_dma_task` | DOUTD ctrl: dwords-1, register, b22-24 = (n-1)&3, b25 secondary; one DOUTD per 16 dwords; consecutive registers | PDS.md "DOUTD" (fields; b22-24 "unknown"); STATE.md §2 table (kinds 3/0/1 for 12/25/10 dwords); GEN.md source table [G] list | partial |
| sgx545_frame.c:589-602 ring sizes | pixel code 1 MB of the 5 MB ten code bases reach; PDS pixel 32 MB, PDS vertex 28 MB, general ~2.8 GB | UAPI.md §5 "Heap layout" | covered |
| sgx545_frame.c:612-622, 1275-1278 `sec_wait` | phas.i #0,#0 [wait=7] (0, 0xfa440700); secondary prologue up to `wdf`; `nop [end]` (0, 0xf8040140); a pixel secondary DMA task that ends without waiting for its DMA locks the GPU (EA3) | ISA.md "flow control" (`nop`), "phase" (`phas.i`), "Program structure and calling conventions" (prologue), "What the draw-time builder emits" (word values) | partial |
| sgx545_frame.c:623-625 + mkusse.py:45-54 `state_emit[1..32]` | phas; prologue; `mov o<r>, pa<r> [rcnt rpt<=16]`; `emit.t3 r0,#0,#0`; `op0xd6 #0`; `emit.t1 o0,#0,#0 [end f3_20]` | STATE.md §2 ("The state-emit USSE programs are just emit instructions"); ISA.md "other" (emit, op0xd6 encodings), "Moves" (rpt up to 16) | partial |
| sgx545_frame.c:627-642 `empty_state`, `terminate` | empty secondary update; VDM type-6 TA terminate: DOUTA (0x2000, reg 0), 3 × DOUTA (0x0016000e, reg 1), DOUTU of `state_emit[2]` | STATE.md §2 state-program table (`63040000 0e022401`, `43040006 0400e100`); PDS.md "DOUTA", "What the DDK generates" (0x1fd60) | partial |
| sgx545_frame.c:644-648 `pds_empty_p`, `clear_pds` | empty pixel task; clear pixel program (copy_fs + colour DOUTI) | STATE.md §1 header +0x08 (background secondary `04001800`); §3 "PDS words" (clear `04001800 00030001 ...`) | covered |
| sgx545_frame.c:650-655 identity index buffer | glDrawArrays through 16-bit identity indices | STATE.md §2 ("a static 16-bit identity index buffer") | covered |
| sgx545_frame.c:687-719 `rt_numbers` | xt, yt; mode threshold 0x4b000; mx/my per mode; MTILE1/2 (b31 in mode 1); TE screen; tiles per macrotile; padded tiles mx·2 or mx·4; region headers padx·pady·12; tail pointers next-pow2(max(padx,pady))²·4 | STATE.md §6 "Render target", "RT data", "Region headers and tail pointers"; §1 tail +0x23c..+0x248; UAPI.md "CREATE_RT_DATASET" (region and tail-pointer formulas, G14) | covered (UAPI.md; STATE.md §6 conflicts, see §3) |
| sgx545_frame.c:750-809 `eot_program` | 7 instructions: phas.i [wait=7], LIMM r0-r3 = PBE words, EMITPIX ×2; PBE words clip min 0, clip max (h-1)<<16\|(w-1), ((stride-1)>>1)<<16 \| b15, address & ~3; sideband b9 \| format \| b10 dither \| b13 twiddled (layout 0); DOUTU temps 4 | STATE.md §4 "The end-of-tile program", "PBE words", "Format codes"; TEX.md §3 table (program = phas, 4 × limm, 2 × emitpix); TEX.md §11 | partial |
| sgx545_frame.c:811-869 `pixel_event` | 29-instruction program: tests in1 & 8 / & 2 / & 4, `(in1 op0xd.s1 ds1[0]) & 0x100`, DOUTA (in2, ds1[0]), EOT task word built from in0<<3 & 0xff8 \| 0x800000, in0<<3 & 0x3ff000, in1<<20 & 0x400000; data ds0[1] = 0, ds1[1] = EOT DOUTU w0 (temps 4), ds0[2] = pe_emit DOUTU w0, ds0[3] = DOUTU w1 b22\|b23, ds1[2..4] = 8, 2, 4, ds0[4..8] = 0x100, 0xff8, b23, 0x003ff000, b22 | PDS.md "What the DDK generates" (pixel-event program decoded), "DOUTU" word 1 ("an OR of the tile position fields, 0x00800000 and 0x00400000"), "Arithmetic and logic" (AND masks 8, 2, 4, 0x100, 0xff8, 0x3ff000), text examples (`shl ... #0x14`) | partial |
| mkusse.py:134-143 `pe_ptoff`, `pe_emit` | in1 & 8 task: `phas.i; ptoff [end]`; ds0[2] task: `phas.i; limm r0,#0; emit.t0 r0,#0,r0 [end f3_20 imm=512]` | ISA.md "visibility test" (`ptoff` encoding, name a guess), "other" (`emit` fields, targets unknown); GEN.md "What a frame is made of" (lists "the two pixel-event tasks") | partial |
| sgx545_frame.c:871-908 `background` | DOUTU copy_fs, w1 samples; DOUTI sample set 1, the 0x12000 pure-sample bits, last; T0 strided = (p-1)&7<<9 \| (p-1)>>3<<17 \| 0x90, else 0x07fe0090; T1 layout<<29 \| format<<24 \| w-1<<12 \| h-1; T2 = target; T3 = 0x00800000; bg words {empty secondary rows 1, 0x00030001, primary} | STATE.md §1 +0x08..+0x10; §5 T0/T1 tables; TEX.md §11; PDS.md "DOUTI" (`40012001`); STATE.md §3 "Scissor ..." (load object T0 `00b60e90`, T1 `cc2cf1df`) | partial |
| sgx545_frame.c:955-987 `state_block` | `tst; bra; doutd; tst; bra; doutd; doutu` program; VDM `0x40000000 \| va>>4`; info `rows<<26 \| (n+0x2000) \| 0x2020400 \| (dwords+3)>>2` with rotating n | STATE.md §2 (state words, static state program); PDS.md "What the DDK generates" (static VDM state); GEN.md source table | covered (low bits [G] there) |
| sgx545_frame.c:989-1000 `vertices_per_task` | 128 / round4(output dwords), at most 16; 64 / output dwords hung at 12 output dwords | STATE.md §2 draw word 4 (top nibble = vertices per task - 1); GEN.md source table [G] ("64 / output dwords, at most 16"); stated only in DRIVER-UPSTREAM.md §3 audit | partial |
| sgx545_frame.c:1002-1037 `vdm_draw` | MUL16 in0 × stride → ds1[0x32]; DOUTD.x2.s27 per ≤16 dwords, kind 0, primary; DOUTU.cc3; VDM words `0x80c00000\|prim<<26\|count`, idx & ~1, `(16-1)<<28`, 0x003fffff, `(vpt-1)<<28\|va>>4`, info `ceil(out/4)<<24 \| ceil(in bytes/16)<<7 \| min(vpt-1,3)<<22 \| rows` | STATE.md §2 draw table; PDS.md "MOVS" (`.x2.s27`, `.cc3`), "What the DDK generates" (0x1f3d0), "How programs are referenced" (VDM type 7/f info) | partial |
| sgx545_frame.c:1069-1109 `vpgen_object`, `apply_clip` | header d485, ISPA 0x07e00100, ISPC 0 / 0x02000000, region clip words, output select 0x40000, MTE 0x40, TSP 0, 9 dwords kind 0, reset triangle / set strip geometry | STATE.md §3 "Scissor and partial viewports: pass type 3 objects" | covered |
| sgx545_frame.c:1111-1170 `sgxws_clear` | vertices x,y,z,ARGB (16 B) at (0,0),(2w,0),(0,2h); header d4c3 (+ISPC); ISPA 0x01c00200/0x01d00200, b21, stencil ref; ISPB mask; ISPC 0x0e0200ff; PDS words (rows 1, 0x00030001, rows 2); whole-target region clip; output select 0x81800; MTE 0x40; TSP 5, 0; DMA n dwords kind (n-1)&3; 8 vertices per task; depth write skipped for depth 1.0 before any draw | STATE.md §3 header/ISPA/ISPB/ISPC/"PDS words"/"Output select"/"MTE control"/glClearStencil; §2 (12 dw, kind 3; clear draw `73040104 02c00082` in "A frame, end to end"); §6 render details +0x40; NOTES.md "Findings" (clear vertices) | covered |
| sgx545_frame.c:1172-1209 `ispa`, `mte_ctrl` | ISPA b9, pass type b25-27, object type b15-18 (1 lines, 3 points, 4 sprites), stencil b8 + ref, b11 two-sided, depth compare / write; MTE 0x1555620 \| cull 1/2 by winding | STATE.md §3 "ISPA", "MTE control (ctx+0x798)" | covered |
| sgx545_frame.c:1284-1349 `sgxws_draw` TA block | header bits (df43, +4, +0x38); ISPB line width ×16 at b8-15, max 0xff; PDS middle word `units \| 3<<16 \| ((sec+3)>>2)<<18 \| min(12/units, 0xb)<<25`, units = pa dwords / 4 (min 1); viewport 6 floats; 0x200 word; output select; W clamp 0x3727c5ac; MTE; TSP; DMA max(25, n+5) with kind (dma-1)&3 | STATE.md §3 header table, ISPB, "PDS words", "Viewport (6 floats)", "Output select", "MTE control"; §3 notes on the 25/27-dword DMA | partial |
| sgx545_frame.c:1268-1282 fragment secondary DMA | constants in the PDS pixel ring; DOUTD chunks; DOUTU w1 = 4 (pixel b2); memory-block bases patched into SA words | PDS.md "DOUTU" word 1 b2; STATE.md §3 "PDS words" | covered |
| sgx545_frame.c:1351-1367 vertex secondary attributes | sa0 = block address, sa1 = 0, sa2-5 = 2/255, 1/255, 2/65535, 1/65535, sa6-9 = 0, shader constants from sa10; DMA task with the empty task; VDM info `rows<<26 \| 0xe100 \| (n+3)>>2` | STATE.md §2 table (row 0x30401120); GEN.md "Smaller things" ("The DMA constants"); ISA.md "Constants past sa127" (constants from sa9/sa10) | partial |
| sgx545_frame.c:1377-1386 `sgxws_vdm_end` | `4\|empty_state>>4`, `1<<26\|0xe100`, `6\|terminate>>4`, `3<<26\|0x2022401`, `0xc0000000` | STATE.md §1 "A frame, end to end" (kick 2 words), §2 table | covered |
| sgx545_frame.c:1620-1667 `sgxws_upload_vs` | phase 0: `phas.i #p1, #temps/4` (0xfa440000\|t, no wait), prologue, `nop [end]`; phase 1: `phas.i #p2 [wait=2]`, shader, `emit.t3 r0,#0,#0 [end f12 f10]`; epilogue `phas.i #0,#0 [wait=7]`, `emit.t1 r0,#0,#0 [end f12 f10 f3_20]` | GEN.md "Vertex programs end with an extra phase"; ISA.md "phase" (phas.i fields), "other" (emit), "flow control" (nop) | partial |
| sgx545_frame.c:1669-1696 `sgxws_blend_words` | sop2 hi base 0x80800000 (class + skipinv), c1 h6-8/h24, c2 h3-5/h15, a1 h20-21/h11, a2 h9-10/h2, cop l18-19, aop l16-17, end h18, dst l21-27 bank h0-1, src banks; swap form | ISA.md "sop2 (hw 10 ...)", "Operand slots and bank codes", "Common fields"; STATE.md §3 "Blending ..." | covered |
| sgx545_frame.c:1698-1755 `blend_const_words` | 4 × sopwm (hi 0x90807800 / 0x90800800 + selector tables) + sop2 0x81808804; X/Y operand per factor; swap; `atrade` reordering for alpha subtract | STATE.md §3 "Constant-colour blending"; ISA.md "sopwm (hw 12 ...)", "sop2" | partial |
| sgx545_frame.c:1775-1783 `wmask_words` | `sopwm o0, pa<c>, pa<c> [skipinv end mask=m c1=c2=zero cop=aop=max]` = lo 0xa0000000\|c<<7\|c, hi 0x90b40601 with mask bits h12 h13 h14 h11 | ISA.md "sopwm" (mask order h12,h13,h14,h11), "Text notation and defaults"; STATE.md §3 colour-mask bullet | covered (STATE.md's `mask<<11` form conflicts, see §3) |
| sgx545_frame.c:1798-1830 discard program | `phas.i #phase1, #t [wait=1]` (0xfa440100\|t); pcoeff (0xb0000004\|n<<14\|(n+1)<<7, 0xf8300004); shader minus its feedback nop; atst8 [end] (0xf0000184\|c<<21\|c<<14, 0xf9340406); `nop` (0, 0xf8000140) so phase 1 starts on an even instruction; blend / mask without end, or `mov o0, pa<c> [skipinv rcnt]` (0x28a10001); `phas.i #0,#0 [end wait=7]` (0xfa4c0700) | ISA.md "What the draw-time builder emits", "What a compiler must emit to discard", "visibility test" | partial |
| sgx545_frame.c:1831-1846 single-phase fragment program | `phas.i #0,#0 [wait=7]`; shader; blend/mask, or `mov o0, pa<c> [skipinv end mask=1]` (0xa0000000\|c<<7, 0x28851001); the same mov without the mask locks the GPU (EA3) | ISA.md "What the draw-time builder emits" (single phase), "Moves (hw 5)", "Text notation and defaults" (mask default) | partial |
| sgx545_frame.c:1848-1862 `sgxws_upload_fs_secondary` | phas.i [wait=7], code, `nop [end]` | ISA.md "Phases" bullet in "Program structure", "flow control" (`nop`) | covered |
| sgx545_frame.c:1864-1881 `clear_pds_masked` | masked clear = colour DOUTI + phas + colour-mask sopwm on pa0 | STATE.md §3 colour-mask bullets ("a different clear shader"); ISA.md "sopwm" | covered |
| mkusse.py:24-32 `PROLOGUE` | `nop [nosched]`, `test.and.ne p0,#0,#0 [skipinv nodst=0]`, `(p0) ld.a.d pa0,pa0,#0 [skipinv rpt=4]`, `wdf` at the start of every vertex/state task | ISA.md "How the corpus builds control flow" / "Program structure" (stated for secondary programs only) | partial |
| mkusse.py:35-42 `VS_EPILOGUE` | `phas.i #0,#0 [wait=7]`, `emit.t1 r0,#0,#0 [end f12 f10 f3_20]` | GEN.md "Vertex programs end with an extra phase" | covered |
| mkusse.py:59 `empty` | `phas.i #0,#0 [end wait=7]` | ISA.md "phase" (encoding) | covered |
| mkusse.py:63-74 `clear_vs` | o0-o2 = pa0-2, o3 = c3 (1.0), o4-o7 = `pck.f32.u8 [scale]` of pa3 lanes 2, 1, 0, 3; 8 output dwords | NOTES.md "Findings" (clear vertex = x,y,z,ARGB8888); ISA.md "pck" (lanes, scale = /255), "Hardware constants" (c3); STATE.md §3 output select 0x00081800 | partial |
| mkusse.py:77-85 `vpgen_vs` | `mov o0,pa0 [rpt=2]; mov o2,c3; mov o3,c3` | STATE.md §3 "Scissor and partial viewports" | covered |
| mkusse.py:87-90 `copy_fs` | `mov o0, pa0 [skipinv end rcnt]`: the colour iterator / background sample already the packed pixel | TEX.md §3 table (copy shader `phas.i; mov o0, pa0 [end]`); PDS.md "DOUTI" (0x20 colour, 4 components) | partial |
| mkusse.py:94-115 `vs_xy`, `vs_xyz` | phase 0 (prologue, `limm pa3,#1.0`, `limm pa2,#0 [end]`), phase 1 `[wait=2]` copies pa0-3 to o0-3 | GEN.md "The API" / "Vertex programs end with an extra phase"; ISA.md `limm`, `phas.i` | partial |
| mkusse.py:120-132 `fs_uniform_sec`, `fs_uniform` | secondary packs FP16 pa9/pa10 to BGRA8 in pa9 (`pck.u8.f16 dmask=6/9 scale`); main `mov o0, sa9 [skipinv end mask=1]` | NOTES.md "Findings" (FP16 uniform pairs); ISA.md "pck" (dmask lanes), "Program structure" (secondary pa = main sa), "What the draw-time builder emits" (probe's `mov o0, sa9 [end]`) | covered |
| sgx545_powervr_sgx.c:49-61 `TA_KICKS`, `heap_ids[]` | a scene = 2 TA kicks (stream, then terminate words); uAPI heap index → heap id | UAPI.md "SUBMIT_JOBS" (two elements, G9), §5; docs/uapi/powervr_sgx_drm.h | covered |
| sgx545_powervr_sgx.c:198-316 `va_alloc`, `create_bo`, `psgx_bo_alloc` | BO_CODE for USSE heaps (CPU copy, no mapping), ALLOW_CPU_ACCESS + GET_BO_MMAP_OFFSET otherwise; page-rounded, ≥0x1000-aligned VM_MAP; texture → general when there is no texture heap | UAPI.md "CREATE_BO, GET_BO_MMAP_OFFSET", "VM contexts, VM_MAP, VM_UNMAP", §5 (texture heap is convention) | covered |
| sgx545_powervr_sgx.c:371-496 import / export / KMS handle / wait | imported dma-buf in the general heap, mapped through the dma-buf; export refuses code objects; CPU wait = timeline point + dma-buf poll | UAPI.md "CREATE_BO, GET_BO_MMAP_OFFSET" (imports, PRIME), "SUBMIT_JOBS" (fences); WINSYS.md "What each backend does" | covered |
| sgx545_powervr_sgx.c:498-531 `psgx_code_upload`, `psgx_code_reset` | UPLOAD_CODE of [off, off+size), multiples of 8, `bad_offset`; append-only while mapped, so a reset is a new object at the same address | UAPI.md "UPLOAD_CODE", "D12. Code objects and the upload ioctl" | covered |
| sgx545_powervr_sgx.c:535-596 contexts, RT datasets | CREATE_CONTEXT render/normal in the VM; CREATE_RT_DATASET w, h, samples = 1 | UAPI.md "CREATE_CONTEXT", "CREATE_RT_DATASET" | covered |
| sgx545_powervr_sgx.c:618-703 `psgx_kick` | CMD_RENDER: pixel event addr/rows, bg secondary/primary addr = PDS base + (word & 0x3ffffff)<<4, rows = word>>26, bg_depth 1.0; CMD_TA_KICK vdm_base per kick; one timeline signal op; BO refs with WRITE; rt_dataset_handle | UAPI.md "SUBMIT_JOBS", §3.2 stream table, §3.3, §3.4 (cmd+0x08/+0x10 packing) | covered |
| sgx545_powervr_sgx.c:754-841 `sgxws_powervr_sgx_dev_create` | timeline syncobj cap; DEV_QUERY GPU_INFO / HEAP_INFO; heaps above 4 GiB skipped; first page of each heap unused; `usse_base_index` from HEAP_INFO; required heaps | UAPI.md "DEV_QUERY", §5 ("usse_base_index", below 4 GiB, rings 0x100 in) | covered |
| sgx545_pvr.c:27-30 CCB constants | ring 0x10000, CCB buffer 0x10420, TA command 0x2c0 | STATE.md §6 HW render context word 3 ("0x10420 bytes = 0x10000 + 0x420 of control"); §1 header +0x00 (0x2c0) | partial |
| sgx545_pvr.c (all) + meson.build:20-62 services bridge ABI | libpvr calls: ALLOC_DEVICEMEM, map, WRAP_EXT_MEMORY, SGX_REGISTER/UNREGISTER_HW_RENDER_CONTEXT, SGX_DOKICK with `SGX_CCB_KICK`, SGX_FLUSH_HW_RENDER_TARGET; `PVRSRV_SYNC_DATA` layout; heap info `ui32HeapID`, `sDevVAddrBase`; the kernel defines the headers need (`SGX_CORE_REV=1013`, `SUPPORT_SGX545`, `SUPPORT_PERCONTEXT_PB`, `SUPPORT_SGX_NEW_STATUS_VALS`, `TRANSFER_QUEUE`, `PVR_SECURE_HANDLES`, ...) | none (NOTES.md "Blob-free replay" names the calls; SRVINIT.md "The bridge sequence" covers srvinit's calls only; DRIVER-UPSTREAM.md §4 lists it as "— (services)") | missing |
| sgx545_pvr.c:118-196 sync data, `token_wait`, `pvr_bo_wait` | a render target's sync data mapped once; a kick adds one pending write on the target; done when `ui32WriteOpsComplete - value >= 0` | TEX.md §5 "HW sync lists"; UKERNEL.md §5 (complete set to the pending value); NOTES.md "Blob-free replay" (poll the back buffer's sync data); TEX.md §10 (sync object mmapped once) | partial |
| sgx545_pvr.c:209-249 `ctx_pb` | pages = (bytes + 0x22000)>>12; free list `prev<<16\|next`, 0xffff ends, ≥0x40000 bytes indexed from the heap base; block list {pages, first\|last<<16, descriptor, 0}; descriptor words 1-4, 6-7, 10-14 (TA = pages+0x10, ZLS = global = pages-0x22, PDS = min-0x100) | STATE.md §6 "PB descriptor", free list; GEN.md "Smaller things" (block list word 2, free list size) | covered |
| sgx545_pvr.c:251-303 `pvr_ctx_init` | allocation order/heap/attribs/size/align: PB desc (kdata 0xf 0x3c/0x20), block list, PB (3D 0xb), free list, CCB (kdata 0x4f 0x10420/0x1000), CCB ctl (8/0x20), render-context sync word (sync 0x3, 4/0x10), scratch (3D 0xb 0x40/0x40), a 0x80-byte buffer (sync 0x4b), ×2 transfer-context dummies (kdata 0x4f 0x104c8/0x1000, 8/0x20, sync 4, sync 4 = the 3D sync for i = 0, kdata 4/4); HW render context words 0 = 0x21, 3, 4, 6, 7, 16 = pid, PD at byte 8 | STATE.md §6 "HW render context"; GEN.md "Layout: the microkernel reads the previous client's render"; TEX.md §10 setup table (transfer CCB 0x10000 + 0x4c8, control 8); UAPI.md "CREATE_CONTEXT" | partial |
| sgx545_pvr.c:319-353 `pvr_rt_init` allocations | 3 × {render details 0x1ec, rd sync (sync 8), kdata 8, kdata 4, HW sync list 0x28, its access word (sync 8)}; RT word, RT data 2·0x38+0x18; tail pointers; TA blocks 0x40, 0x50, 2 × 0x88; background block 0x200; 2 × region headers; 2 entry words | STATE.md §6 "RT data", "Region headers ...", "Render details" (0x1ec), §1 +0x3c (0x28 block); GEN.md "Layout" (three render-details sets) | partial |
| sgx545_pvr.c:355-378 RT data contents | header [0] = RT word, [2] = padded tiles X, [5] = 2; per entry HW ctx, (mx·my)>>4, ·0xc, bg block, TA blocks, tail pointers + size, 1, entry word, 0x10; RT word starts at 1 | STATE.md §6 "RT data" | partial |
| sgx545_pvr.c:380-396 background object block | [5] = [8] = 1.0, [10] = 0x202, [15] = 0x100000, [16] = 0x1000, [17] = 0x3f80, [18] = ((w+0x1000)·0x100)<<8\|0x10, [19] = 1.0, [20] = (h+0x1000)·0x100, [21] = 0x1000, [22] = 0x3f80 | STATE.md §6 "Background object block"; TEX.md §10 ("Block words 3-8 are the texture coordinates ... (0,0), (1,0), (0,1)") | covered |
| sgx545_pvr.c:398-423 render details | +0x00 3D request base, 0x08 0x80, 0x0c 0xf0000, 0x10 b31, 0x38/0x3c 0x80000000, 0x40 1.0, 0x44 0x300, 0x48 bg block word, 0x50 0x88, 0x58 0x30, 0x64 1, 0x68 1, 0xcc 0x100, 0xd0 rd sync, 0xd8 RT data, 0x104 1, 0x110 kdata 8, 0x114 kdata 4, 0x13c 0x07c00000; HW sync list [0] = access address, access = 1, rd sync = 1 | STATE.md §6 "Render details" table; UAPI.md §4.2; TEX.md §5 (sync list layout) | partial |
| sgx545_pvr.c:461-529 `ta_command` | header: size 0x2c0, TA flags 0x63 \| 0x10 on the first frame, bg PDS words, 0x50000000 ×2, 3D flags 0x400 \| f<<14, +0x20 = f, PB desc, rd, HW sync list, RT data, entry; tail: TE AA, MTILE1/2, TE screen, tiles, +0x24c = 0x400000, VDM base, +0x254 0xa2, region headers, tail pointers, 1e-6 ×2, screen size, PB base, +0x2a4 0x88; rd+0x20/+0x28 alternate; rd+0xdc = entry; RT word = f+1, rd sync = 1, access = 1 before each kick | STATE.md §1 header and tail tables; §6 render details +0x20/+0x28; GEN.md "Running it" (`-R N`, SGXGEN_ALTERNATE: entry/region alternation, frame 2 values); UAPI.md §4.1 | partial |
| sgx545_pvr.c:531-586 `pvr_kick_frame` | rd+0x5c/+0x60 = pixel event; kdata word +0x114 target = bg primary word; CCB offset = woff + 0x58; `sCommand.ui32Data[1]` = HW ctx; HW sync list mem; one destination sync (the target); first and last in scene in one kick; `h3DSyncInfo` = the context's 3D sync; write offset (woff + 0x2c0) & 0xffff into CCB control word 0 | STATE.md §1 (0x58 before `ui32CCBOffset`), §6 word 4 (CCB control), "Building a frame from scratch" step 4/7; NOTES.md "Blob-free replay"; TEX.md §5 | partial |
| sgx545_pvr.c:433-456 `pvr_rt_fini` | no SGX_FLUSH_HW_RENDER_TARGET by default (times out, then the GPU locks up); kernel-data structures never freed | GEN.md "Layout: the microkernel reads the previous client's render" | covered |
| sgx545_pvr.c:596-603 `pvr_frame_info` | PB descriptor +0x14 (pages in use), RT data [1] | STATE.md §6 PB descriptor 0x14 [G], RT data header | covered (debug only) |
| sgx545_pvr.c:633-664 `sgxws_pvr_dev_create` | heap id = `ui32HeapID & 0xf`; code bases 0-9 pixel, 10-13 vertex | NOTES.md "Findings" (id low byte); SRVINIT.md "Init scripts"; UAPI.md §5 | covered |
| sgx545_frame_test.c | 720×480, stride 736, BGRA8888: PBE format 0, T1 format 0x0c, layout 6; viewport scale/offset with y scale -h/2 | STATE.md §4 "PBE words" (736 px pitch), "Format codes"; §5 T1 table; TEX.md §11; STATE.md §3 "Viewport" | covered |
| sgx545_drm_winsys.c | screen creation glue | | n/a: no hardware facts |
| sgx545_drm_public.h | kmsro entry point | | n/a: no hardware facts |
| meson.build (outside the services block) | build wiring | | n/a: no hardware facts |

Counts: 83 rows; 44 covered, 34 partial, 2 missing, 3 n/a.

#### Facts the docs need

**sgxws.h `sgxws_target` (partial)**
- STATE.md §4 "PBE words": what a stride of 1 encodes (word 2 = 0; the code accepts "even, or 1"), and that the stride must otherwise be even.

**sgxws.h `sgxws_draw` (partial)**
- PDS.md "DOUTD": the required alignment of a DOUTD source address. The code assumes 16 bytes for vertex buffers (`verts_va`) and 32 for its own DMA sources; GEN.md only records the DDK's 32-byte habit.

**sgx545_frame.h heap ids (partial)**
- NOTES.md "Findings" (or UAPI.md §5): the heap-id numbers next to the bases: 0 general, 1 TA data, 3 kernel data, 4 pixel shader, 5 vertex shader, 6 PDS pixel, 7 PDS vertex, 8 sync, 10 3D params, 14 texture. Also that the winsys takes `ui32HeapID & 0xf`. Kernel code (2?) is not used.

**`SGXWS_ATTRIBS_COLOR` 0x10000003 (missing); `sgxws_bo_alloc`/`ring_init` attribs (partial)**
- SRVINIT.md "Allocations" or a new services section: the attribute bits by name: READ 0x1, WRITE 0x2, CACHE_CONSISTENT 0x4, NO_SYNCOBJ 0x8, EDM_PROTECT 0x40. Also what bit 28 (0x10000000) is: the DDK sets it on the colour buffers it renders to.
- Which attributes each client buffer kind needs: colour buffer 0x10000003, texture 0x3, vertex/ring 0x1. A ring written by the CPU has only READ (0x1); say whether that is right.

**Alignments (partial)**
- GEN.md "Smaller things": the code uses `PDS_ALIGN` 0x40 (64 bytes), but GEN.md and UAPI.md §5 say PDS programs go on 32 bytes. State the value used, and the hardware minimum once tested (G10).

**`sgxws_usse_upload` phas.i relocation (partial)**
- ISA.md "phase / 0xfe-0x100": what a `phas.i` pc counts from when the task's DOUTU names code base n > 0. ISA.md's builder table says "relative to the code heap". UAPI.md "UPLOAD_CODE" and ISA.md `ba` say "from the code base the task runs from". The code uses the heap base, so a pixel program past the first 512 KB depends on the heap-base reading.

**DOUTD control and chunking (partial)**
- PDS.md "DOUTD": the b22-24 rule the code uses is (dwords - 1) & 3. It fits 12 → 3, 25 → 0, 10 → 1, 11 → 2, 27 → 2. Say whether it is a hardware field or only the DDK's value.
- PDS.md "DOUTD": at most 16 dwords per DOUTD. A 20-dword vertex DMA lost its last dwords (Minecraft arm, EA3). Also that the DDK never DMAs more than 25 dwords at once.

**`sec_wait` (partial)**
- ISA.md "Program structure" or PDS.md: a pixel secondary task that follows a DOUTD must `wdf` before it ends. Pointing the DMA task at the empty task (`phas.i [end]`) locked the GPU (EA3). Give the minimal program: phas, prologue up to `wdf`, `nop [end]`.

**State-emit programs (partial)**
- STATE.md §2 or ISA.md: the program text: phas, prologue, `mov o<r>, pa<r> [rcnt rpt=k]` in chunks of at most 16, `emit.t3 r0,#0,#0`, `op0xd6 #0`, `emit.t1 o0,#0,#0 [end f3_20]`. One program per block length 1-32.
- ISA.md "other": what `emit.t3` / `emit.t1` with `f3_20` send (state to the MTE vs a vertex). Also what `op0xd6 #0` does between them. Both are needed by every state block.

**TA terminate (partial)**
- STATE.md §2 / PDS.md "DOUTA": what the DOUTA values 0x2000 (reg 0) and 0x0016000e (reg 1, three times) mean. Whether the repeats are required.
- STATE.md §2: what the terminate's USSE task must be. The DDK's is 0x3c000000; the code runs `state_emit[2]`, a state emit of the two attributes the DOUTAs wrote.

**End-of-tile program (partial)**
- ISA.md: an `emitpix` form, with the fields behind `0x00200080 0xfb200100` and the sideband variant. Today the words exist only in STATE.md §4.
- STATE.md §4: that the program starts with `phas.i #0,#0 [wait=7]` (the code's word 0, 0xfa440700). Also its DOUTU temporaries (the code gives 4 for r0-r3).
- STATE.md §4 PBE word 2: b15's effect (TEX.md §6 says the PBE writes nothing with it clear). This is already flagged as a hardware-poke candidate.

**Pixel-event program (partial)**
- PDS.md "What the DDK generates": the full program as data plus code. The data words:
  - ds0[0] = the ptoff task's DOUTU w0, ds0[1] = 0;
  - ds1[0] = 0 (the compare word), ds1[1] = EOT DOUTU w0 with temps 4;
  - ds0[2] = pe_emit DOUTU w0 (temps 4), ds0[3] = DOUTU w1 with b22 | b23;
  - ds1[2..4] = 8, 2, 4;
  - ds0[4..8] = 0x100, 0xff8, 0x00800000, 0x003ff000, 0x00400000.
- PDS.md, the EOT task word 1: `(in0<<3 & 0xff8) | 0x00800000 | (in0<<3 & 0x3ff000) | (in1<<20 & 0x400000)`.
- PDS.md: which event each of in1 bits 3, 1, 2, 8 is. Also what in0 and in2 carry (listed as guesses now).
- ISA.md / GEN.md, the two pixel-event tasks:
  - in1 & 8 runs `phas.i #0,#0 [wait=7]; ptoff [end]`;
  - the ds0[2] task runs `phas.i; limm r0,#0; emit.t0 r0,#0,r0 [end f3_20 imm=512]`.
- ISA.md: what `ptoff` and `emit.t0 ... imm=512` do for the pixel pipe.

**Background object (partial)**
- STATE.md §5 "T3": the value 0x00800000 the background load uses. The table only gives 00800800 (tex1) and 01028800 (tex2).
- STATE.md §1 / §3: the meaning of the PDS middle word 0x00030001 for the background and the clear (G12).
- STATE.md §5 T0: b21-26 = 0x3f in 0x07fe0090 is still [G].

**Vertices per task (partial)**
- STATE.md §2: the rule is round4(output dwords) → 128 / that, at most 16. The capture table:
  - 4 or 8 output dwords: 16;
  - 12: 10; 16: 8; 20: 6; 24: 5;
  - input dwords don't count.
- STATE.md §2: 64 / output dwords hung the GPU at 12 output dwords (EA3). GEN.md's [G] list says "64 / output dwords" and needs correcting.

**VDM draw (partial)**
- STATE.md §2 draw word 2: what "batch" is, and why the value is 16 (`f0000000`).
- STATE.md §2 draw word 5: what n is in b22-23 = min(n, 3). The code uses vertices per task - 1, which gives 3 for both captured draws.
- PDS.md "What the DDK generates" (vertex fetch): split a vertex into DOUTDs of at most 16 dwords. The DDK DMAs per stream.

**`sgxws_draw` TA block (partial)**
- STATE.md §3 "PDS words", word 1:
  - b0-15 = the fragment program's primary-attribute registers in units of 4 dwords (minimum 1);
  - b25-28 = 12 / units, capped at 0xb. Captures with 0-5 vec4 varyings give units 1 1 1 2 2 3 and b25-28 12 12 6 6 4; 0xb with one unit;
  - b16-17 = 3;
  - a program that uses more attributes than the word declares locks the GPU (two F32 vec4 varyings, EA3).
- STATE.md §3: the DMA kind for the 27-dword two-sided block is 2. It follows from the (n-1)&3 rule above, which STATE.md doesn't state.

**Vertex secondary attributes (partial)**
- STATE.md §2 table row 0x30401120:
  - the slot layout: sa0 = the block's own address, sa1 = 0, sa2-sa5 = the driver constants, sa6-sa9 = 0, shader constants from sa10;
  - the constants as f32 2/255, 1/255, 2/65535, 1/65535. STATE.md says 2⁻¹⁵ and 2⁻¹⁶, which GEN.md says is wrong.

**`sgxws_upload_vs` and `vs_xy`/`vs_xyz` (partial)**
- ISA.md "Phases" (a vertex subsection) or GEN.md, the vertex program's phases and their exact words:
  - phase 0 = `phas.i #p1, #temps/4` (no wait, 0xfa440000 | t), then the prologue, then `nop [end]` or attribute fills with `end`;
  - phase 1 = `phas.i #p2 [wait=2]`, the shader, `emit.t3 r0,#0,#0 [end f12 f10]`;
  - then the epilogue.
- ISA.md "Phases": that pa registers written in phase 0 (`limm pa2/pa3`) are what phase 1 reads.
- ISA.md "Phases": that the temporaries in phase 0's `phas.i` are the count phase 1 runs with.

**Constant-colour blending (partial)**
- STATE.md §3 "Constant-colour blending": what happens with subtract and reverse subtract. The code trades the sop2 sources for reverse subtract, and with `atrade` puts the two alpha products in the other register after both whole-colour products. STATE.md says both are untested.

**Discard program (partial)**
- ISA.md "What the draw-time builder emits":
  - the phase-0 `phas.i` the code uses is `[wait=1]` (0xfa440100 | t). The table lists `fa440800 | t` (h11). Say which wait/rate is required;
  - phase 1 must start on an even instruction (16-byte aligned); the code pads with `nop` (0, 0xf8000140);
  - the phase-1 colour out without end is `mov o0, pa<c> [skipinv rcnt]` (0xa0000000 | c<<7, 0x28a10001).

**Single-phase fragment colour out (partial)**
- ISA.md "Moves" or "Phases": the final `mov o0, pa<c> [end]` needs mask=1 (h12; 0x28851001). Without it the GPU locks up (EA3). The same holds for the builtin `mov o0, sa9 [skipinv end mask=1]`.

**PROLOGUE (partial)**
- ISA.md "Program structure": that every vertex and state task (not only secondary programs) starts with `nop [nosched]; test.and.ne p0,#0,#0 [skipinv]; (p0) ld.a.d pa0,pa0,#0 [skipinv rpt=4]; wdf`. Also whether the hardware needs it (DRIVER-UPSTREAM.md §3 asks for a test).

**`clear_vs` (partial)**
- STATE.md §3 or GEN.md, the clear's vertex program and output order:
  - o0-o2 = x, y, z; o3 = c3 (1.0);
  - o4-o7 = R, G, B, A as floats from the ARGB8888 word's bytes 2, 1, 0, 3 (`pck.f32.u8 [scale]`);
  - 8 output dwords to match output select 0x00081800.

**`copy_fs` (partial)**
- PDS.md "DOUTI": that the colour iterator (source 0x20, 4 components) leaves the colour packed as BGRA8 in pa0. The same for the background sample. That lets `mov o0, pa0` write the pixel unchanged.

**CCB (partial)**
- STATE.md §6 HW render context word 3: whether the 0x420 bytes after the 64 KB ring are control or room for a command that runs past the ring's end (the code assumes the latter).
- STATE.md §6: the write offset wraps at 0x10000 (`(woff + 0x2c0) & 0xffff`), and the client writes it to CCB control word 0 before the kick.

**Services bridge ABI (missing)**
- A new services section (SRVINIT.md or a client-side companion) giving the client's bridge calls and their argument layouts as libpvr uses them:
  - ALLOC_DEVICEMEM (heap, attribs, size, align), mapping, WRAP_EXT_MEMORY;
  - SGX_REGISTER_HW_RENDER_CONTEXT (buffer, size, PD offset 8);
  - SGX_DOKICK with the `SGX_CCB_KICK` fields set: hDevMemContext, hCCBKernelMemInfo, ui32CCBOffset, sCommand.ui32Data[1], hKernelHWSyncListMemInfo, ui32NumDstSyncObjects / pahDstSyncHandles, bFirstKickOrResume, bLastInScene, h3DSyncInfo;
  - SGX_FLUSH_HW_RENDER_TARGET;
  - the `PVRSRV_SYNC_DATA` fields read (ui32WriteOpsPending, ui32WriteOpsComplete);
  - the heap-info fields (ui32HeapID, sDevVAddrBase);
  - the kernel configuration the bridge layout depends on (meson.build: `SGX_CORE_REV=1013`, `SUPPORT_SGX545`, `SUPPORT_PERCONTEXT_PB`, `SUPPORT_SGX_NEW_STATUS_VALS`, `TRANSFER_QUEUE`, `PVR_SECURE_HANDLES`, `SUPPORT_HW_RECOVERY`, `SUPPORT_SGX_EVENT_OBJECT`, `SUPPORT_SGX_HWPERF`, `SUPPORT_SRVINIT`, `SERVICES4`, `SUPPORT_DRI_DRM`, ...).

**Sync data (partial)**
- TEX.md §5 or UKERNEL.md §5: SGX_DOKICK adds one to the destination sync's ui32WriteOpsPending. A render is done when ui32WriteOpsComplete reaches that value (wrapping compare).

**Context allocation order (partial)**
- GEN.md "Layout": the full DDK order with heap, attribs, size and alignment:
  1. PB descriptor (kdata, 0xf, 0x3c, 0x20) and block list (kdata, 0xf, 0x10, 4);
  2. PB (3D params, 0xb) and free list (3D params, 0xb, ≥0x40000, 0x10);
  3. CCB (kdata, 0x4f, 0x10420, 0x1000) and CCB control (kdata, 0x4f, 8, 0x20);
  4. SGXCreateRenderContext's own sync word (sync, 0x3, 4, 0x10);
  5. the scratch block (3D params, 0xb, 0x40, 0x40);
  6. a 0x80-byte buffer for GL (sync, 0x4b, 0x20);
  7. per transfer context, twice: kdata 0x4f 0x104c8/0x1000, kdata 0x4f 8/0x20, sync 0x3 4/0x10, sync 0x3 4/0x10 (the first context's is the 3D sync used in the kick), kdata 0x4f 4/4.

**RT allocations and RT data (partial)**
- STATE.md §6, each of the three render-details sets with its companions:
  - a render-details sync word (sync, 0xf, 8);
  - kdata words of 8 and 4 bytes (0xf, align 0x20);
  - the HW sync list (kdata, 0x28);
  - its access word (sync, 8).
  Then the RT word, RT data, TA-data blocks, background block (3D params, 0x4b, 0x200, 0x1000), region headers and entry words, in that order with attribs 0x4f / 0xb.
- STATE.md §6 "RT data": header [1] is given as 1, but the client leaves it 0 (the microkernel may write it; the debug line reads it as "RT frames"). Say who writes it.
- STATE.md §6 "RT data": the frame counter word [0]. Its value & 1 picks the entry that renders. The client writes f + 1 before every kick. Left at 1, odd frames retire the wrong entry, their PB pages are never freed, and the 3D locks up on the fourth frame (EA3, 2026-10-28).

**Render details (partial)**
- STATE.md §6 "Render details" table, the words it doesn't list:
  - +0xd0 = the render-details sync word's address;
  - +0xd8 = RT data;
  - +0xdc = the RT data entry this render retires, which must equal cmd+0x44 (the same fourth-frame lockup otherwise);
  - +0x104 = 1 (GEN.md lists it under render details, STATE.md under the TA command; G12);
  - +0x110 = an 8-byte kdata word;
  - +0x114 = a 4-byte kdata word that the client sets to the background PDS primary word before each kick.
- TEX.md §5: the HW sync list's word 0 = the access word's address. The client rewrites the access word (1) and the render-details sync word (1) before every kick, because the microkernel clears them.

**TA command (partial)**
- STATE.md §1, header +0x04: the bit table STATE.md refers to (G12). Also the single-kick values the code uses: 0x63, with 0x10 added on a target's first frame (0x73). STATE.md and UAPI.md give 0x71 / 0x62 for the two-kick scene; GEN.md gives 0x61 for frame 2.
- STATE.md §1 +0x1c: b14 = the target's frame number (GEN.md shows 0x4400 at frame 2 only).
- STATE.md §1 +0x20: the target's frame number (STATE.md has "kick +0x00 [G]").
- STATE.md §1 tail +0x24c: 0x400000 for a kick that renders (STATE.md gives 0 / 0x2400000).
- STATE.md §1 / UAPI.md G9: that one kick with both bFirstKickOrResume and bLastInScene works (the services backend does it on every frame). UAPI.md lists it as untested.

#### Doc conflicts found on the way

- Tail-pointer size: STATE.md §6 writes `max(mx,my)² × 4`, which gives 0x100 at 720×480, but its own value is 0x1000. UAPI.md "CREATE_RT_DATASET" and the code use padded tiles (G14).
- Vertex secondary constants: STATE.md §2 has 2⁻¹⁵, 2⁻¹⁶. GEN.md and the code have 2/65535, 1/65535.
- Vertices per task: GEN.md's [G] list says 64 / output dwords. The code and DRIVER-UPSTREAM.md say 128 / output dwords; 64 hung.
- PDS alignment: GEN.md and UAPI.md say 32 bytes. The code uses 0x40 and its comment says 32.
- Colour-mask sopwm: STATE.md §3 and ISA.md "Phases" write it as `0x90b00601 | mask<<11`, but ISA.md "sopwm" stores mask x y z w in h12 h13 h14 h11. The code uses ISA.md's order on 0x90b40601 (end set).
- Background block words 3-8: STATE.md §6 calls them a rotation matrix; TEX.md §10 measures them as texture coordinates.
- CCB 0x10420: STATE.md §6 reads the 0x420 as control; the code comment reads it as room past the ring.
- RT data header [1]: STATE.md §6 says 1; the client leaves it 0.
- phas.i target origin: ISA.md's builder table says "relative to the code heap". UAPI.md and ISA.md `ba` say "the code base the task runs from".

### compiler: `mesa/src/sgx545/compiler`

Bit-level encoding is covered by ISA.md through the generated encoder. The
slot capabilities in `sgx_forms.c` are checked against ISA.md's class
sections.

#### Table

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `meson.build` | build only | - | n/a |
| `sgx_priv.h` (IR, `SGX_F_*`, `sgx_ins`, ctx) | IR structures; PRED3 codes 0/1/2/5/6 (`sgx_ins.pred`); `SGX_ITER_FRAGCOORD 14` | ISA.md "Common fields (all ALU classes)" (pred), "Lists and blocks" (list A: gl_FragCoord iterator 14) | covered |
| `sgx_forms.h` (forms, fields) | form and field enum; operand roles | ISA.md class sections (names only) | n/a (encoding) |
| `sgx_forms.c:16-23` slot file sets | generic dst = r o pa ix sa g idx i; src1/src2 = r o pa sa ix c g # i; src0 = r pa (no ext) or r pa o sa (smp/ld ext h18); one-bit dst bank r/pa (smp, ld) | ISA.md "Operand slots and bank codes", "Operands (generic encoder FUN_00107830)", "hw 1c: smp", "hw 1d: ld" | covered (see notes a, b) |
| `sgx_forms.c:32-45, 47-85` per-form slots and modifiers | FLOAT3 src0 abs only + fmt, src1/src2 neg+abs+fmt; FLOAT2 (fsubflr fdp fdsx fdsy) dst/src1/src2; hw1 single source in src1 with neg/abs, no fmt; fmad16 modifiers as hw0, no fmt; mov in src1; movc src0 r/pa; pck/bitwise no src0, no modifiers; test/testmask fmt, no modifiers, test dst = predicate; sop3/sop3l src0 r/pa; sopwm dst/src1/src2; ima16 src2 negate; imae; smp coord/state/lod; ld base/off; limm dst+imm; br target | ISA.md "Float classes" (slot table, modifiers), "Moves (hw 5)", "hw 08: pck", "hw 09 test, hw 0f testmask", "hw 0a-0e: bitwise", "sop3 family", "sopwm", "ima16", "imae", "hw 1c: smp", "hw 1d: ld", "other (h20-21 = 2)" (limm), "flow control" (br) | covered (notes c-e) |
| `sgx_forms.c:102-117` `sgx_form_init` | default mask 1 (15 for sopwm); pck dmask 15; smp/ld src2 = unused pattern #0 | ISA.md "Unused slots and fixed patterns", "Text notation and defaults" (defaults), "Repeat or mask" | partial (notes f, g) |
| `sgx_compile.c:52` `NUM_TEMPS 124` | allocatable r0-r123 (r124-127 = i0-i3) | ISA.md "Register files and operand limits", "Registers" | covered |
| `sgx_compile.c:166-188` `form_class`, `max_rpt` | float 3-src/dot/minmax/deriv repeat up to 4 with per-source inc or moe; hw1/mov/movc up to 16 with every operand stepping; testmask up to 4 as a mask | ISA.md "Repeats", "Float classes" (Repeat paragraph, hw1 rcnt/mask hardware), "Moves" ("Repeat: h21 rcnt..."), "hw 09 test, hw 0f testmask" (mask row) | partial |
| `sgx_compile.c:230-267` `hw_const` | c4-c13 = 2..1024, c14-c27 = 2^-1..2^-14, c28-c39 (e, 1/e, sqrt2, 1/sqrt2, pi/4, pi/2, pi, 1/pi, 2/pi, 4/pi, 2pi, 2^-16) as correctly rounded f32, c48 = 0.0, c52 = 1.0, c56 = 0xffffffff | ISA.md "Hardware constants (c<n>)" | covered |
| `sgx_compile.c:342-419` `legalize` (constants) | only c48-51 / c52-55 can step under a repeat; negated constant through the slot's negate; constant not allowed in src0 -> register; anything else `limm` | ISA.md "Hardware constants (c<n>)" (four 0.0 / four 1.0 for vec4 repeats), "Register files and operand limits" (src0 r/pa only; limm any 32-bit value), "Float classes" (modifiers) | covered |
| `sgx_compile.c:342-419` `legalize` (F16 halves) | an F16 half is legal in a format-select slot only for r/pa/sa0-31; scalar class F16 source; pck 16-bit sources; fmad16 whole registers; testmask never takes F16 operands | ISA.md "Format select and format control", "Float classes" (h22 fmt, hw1 source format, fmad16), "hw 09 test" (fmt row: "destination's numbering under fmt (testmask) was not drawn"), "hw 08: pck" (lane row) | covered |
| `sgx_compile.c:287-340` `materialize`; `:1384-1409` mov/fneg/fabs | modifier or F16->f32 move as `fsubflr x, c48` (a - floor(0)); mov has no format select | ISA.md "Float classes" (fsubflr(a, b) = a - floor(b), hardware; format select classes hw 0, 2, 3, 4), "Format select and format control" | covered |
| `sgx_compile.c:551-644` `emit_runs`, `:3369-3395` `merge_runs` | a repeat writes components in order (a later iteration reads an earlier write); runs of consecutive registers; derivatives never repeated | ISA.md "Repeats", "Float classes" (Repeat: destination steps every iteration), "Derivatives" (never repeated) | partial |
| `sgx_compile.c:688-725` `f32_to_h` | f32 -> F16 as `fmad16 t, x[swz0=f32], c45, c48` (round to nearest, both lanes) | ISA.md "Float classes" (fmad16 swizzle 3 = f32 converted, rounds to nearest), "Hardware constants" (c45 = F16 (1,1), c48 = 0) | covered |
| `sgx_compile.c:746-923` `pair_src`, `pair_reg`, `fmad16` | F16 constant pairs: 0 -> c48, (1,1) -> c45, (-1,-1) -> -c45, else limm / literal SA slot; lane swizzles lohi/ll/hh/f32; src0 r/pa without negate (fold into src1); pairs packed with `pck.f16.f16 d, lo, hi` | ISA.md "Float classes" (fmad16 swizzles 0-3, modifiers as hw0, src0 negate folded), "Hardware constants", "hw 08: pck" (two-source packing, lanes) | partial (pck.f16.f16 half placement) |
| `sgx_compile.c:1012-1235` `emit_alu16` | mediump ALU in fmad16; min/max/dot/sat/sqrt/sin/csel in f32 over F16 halves then rounded back; hw1 F16 source + `f16d` result lands in the source's half, other half zero; floor via fsubflr + fmad16; `pck.f16.f32` truncation for f2f16 of an f32 uniform; `movc.nz.i32` on whole F16 pairs | ISA.md "Float classes" (hw1 source format: h7, h3, **h2 = F16 destination**, other half zeroed), "hw 08: pck" (f32 -> f16 truncates), "Moves" (movc i32 whole word), "Rounding" bullet in "Register files and operand limits" | covered |
| `sgx_compile.c:1238-1601` `emit_alu` (f32) | fmad for add/mul (mul: + -0.0), fminmax (a,a,b) = min / fmaxmin (a,b,a) = max / clamp (x,0,1); fdp result only in the last iteration's destination; fsubflr = fract; floor = x - fract; trunc via `movc.ltz.f32`; compares via `testmask.fsub.<cond>.dword` (ALU 0x0e, cond 0x19 lt / 0x06 ge / 0x14 eq / 0x18 ne, type 2) + `movc.nz.i32 d, m, 1.0, 0.0`; fcsel `movc.nz.f32` | ISA.md "Float classes" (fmad, fminmax/fmaxmin hardware semantics, fdp hardware), "Moves" (movc conds 1 nz / 2 ltz, f32 denormal = 0, ltz only for negative normals), "hw 09 test, hw 0f testmask" (ALU op table, conditions table, testmask type dword), "Comparisons and conversions" | covered |
| `sgx_compile.c:1622-1672` `emit_test`, `discard` | keep predicate p1 = the inverse comparison (`test.fsub.<inv> p1`); else a 1.0/0.0 keep value updated by `movc.nz.f32` and tested with `test.fadd.ne p1, keep, c48` | ISA.md "Conditions, and what GLSL produces" (dc_* corpus, `test.fadd.ne`), "What a compiler must emit to discard" (step 1), "Program structure and calling conventions" (discard) | covered |
| `sgx_compile.c:1674-1737` `front_face`, `cond_f32` | gl_FrontFacing = `xor t, g16, sa[front_sa]; and t, t, #1`, then `movc.nz.i32 f, t, c52, c48`; integer 0/1 not used as a movc.f32 condition (denormal reads as zero) | ISA.md "Fragment shaders" (g16 bit 0 = clockwise in memory, capture; our sequence stated), "Moves" (f32 z true for a denormal) | covered (capture-level: one draw shape, other g16 bits not examined, sequence not yet drawn) |
| `sgx_compile.c:1764-1815` store_reg of F16 halves | `pck.f16.f16` with dmask 3 (low half) / 0xc (high half), first enabled half takes src1 | ISA.md "hw 08: pck" (dmask row: 16-bit dst only 3, 0xc, 0xf; byte-order hardware for u8 only) | partial |
| `sgx_compile.c:1833-1881` dynamically indexed uniform in SA | `pck.s16.f32 t, x [dmask=3]`; `ima16.sgn idx1, t, #1, #<abs SA number> [f1=u8 f2=u8]`; `mov d, sa[idx1 + j]` | ISA.md "What GLSL integer code compiles to" (a[i] row), "ima16", "Registers" (indexed operands), "The number field" (indexed number layout), "hw 08: pck" (f32 -> s16 rounding) | partial (index register value semantics) |
| `sgx_compile.c:1888-1916` lowp C10 uniform read as F16 | `pck.f16.c10 [scale]`, C10 constant k at channel 2, k+1 at channel 1, k+2 at channel 0 of one SA word | ISA.md "hw 08: pck" (lane row: c10 picks the 10-bit channel), "Lists and blocks" (list C kind 3 = C10 triple), "Float classes" (C10 read as clamp(v/255, -2, 2), for hw1) | partial |
| `sgx_compile.c:1921-1955` input loads | PA register per component; an F16-iterated set's component j in half 2*pa + j | ISA.md "Lists and blocks" (list A format 2, registers per entry), PDS.md "DOUTI" (iterations fill PA in program order), ISA.md "Float classes" (F16 numbers count halves, lo first) | covered |
| `sgx_compile.c:1991-2010` gl_PointSize | f32 output after the sets, `fminmax o, x, x, 511.0` (constant folded when constant) | STATE.md "Lines and points" (size as f32, clamped to 511.0), ISA.md "Vertex shaders" (gl_PointSize, output buffer) | covered |
| `sgx_compile.c:2011-2021` vertex outputs | `mov o<n>` per written component of its set | ISA.md "Vertex shaders" (output buffer, "What a compiler must produce") | covered |
| `sgx_compile.c:2023-2053` derivatives | `fdsx/fdsy d, x, x` per component, f32 (F16 source format-selected, result rounded back) | ISA.md "Program structure and calling conventions" (Derivatives) | covered |
| `sgx_compile.c:2384-2426` `emit_cf_list`, `:2074-2097` labels/branches | if: `test.fadd.eq p0, cond, c48` + `(p0) br else`; loops: head label, unconditional back `br`, break/continue = `br`; real branches instead of if-conversion | ISA.md "flow control" (br relative, predicated br falls through, hardware), "How the corpus builds control flow", "Program structure and calling conventions" (Branches, Loops) | partial (syncend) |
| `sgx_compile.c:2101-2185` `find_projected` | `tex(v.xy * rcp(v.w))` of a whole vec4 varying -> non-dependent projected sample (PDS divides x, y by w) | ISA.md "Block 5: SAMPLE" (flags b1 projected; texture2DProj(s, v4) non-dependent), PDS.md "DOUTI" (0x400 projected) | partial |
| `sgx_compile.c:2431-2500` `emit_tex` (non-dependent choice) | untouched varying (whole, or its first components; F16- or f32-iterated) with no bias -> PDS sample of that set; 2D or cube | ISA.md "Block 5: SAMPLE" (what is non-dependent), "The texture-sample sequence a compiler must emit" (non-dependent read), PDS.md "DOUTI" (sample half: set + 1, 2-D 0x200, projected 0x400) | partial (cube) |
| `sgx_compile.c:2501-2513` non-dependent sample PA | the raw texel in one PA register after the plain iterations, in iteration order | ISA.md "The texture-sample sequence..." (texel lands in the next PA registers, raw), "The finalizer" (only the first chunk from the PDS; one chunk for <= 32-bit texels), PDS.md "DOUTI" | covered |
| `sgx_compile.c:2514-2603` dependent sample | coordinate in consecutive registers of one bank (r/pa/sa); F16 coordinates two per register with smp `.f16`; projective = `frcp` + multiply first; bias in src2 (any f32 source, hardware constant allowed) | ISA.md "hw 1c: smp", "The texture-sample sequence a compiler must emit" (dependent 2D, bias, cube), "Open questions" (coordinate register count not in the word) | partial (F16 coordinate layout) |
| `sgx_compile.c:2605-2638` texel unpack | raw RGBA8, channel n in byte n; f32: `pck.f32.u8 [scale] .l<n>` = b/255; F16: `pck.f16.u8 [scale]` two channels per register | ISA.md "hw 08: pck" (u8 -> f32: .l<n> is byte n, scale = b/255, hardware), "The texture-sample sequence" (step 4), TEX.md "Texel formats" (0x0d RGBA8888 = R, G, B, A bytes) | partial (u8 -> f16 rule; non-8888 formats) |
| `sgx_compile.c:2644-3188` lowp C10 colour path | leaves = lowp vec4 uniforms (xyz C10 triple + w alone); `pck.c10.c10 i, xyz, w [scale]` into i0-i2; sopwm [c1=one c2=one cop/aop add, sub, min, max]; sop3 [cop=sub c1=one c2=s0 a=one aop=sub] = x - y*z; sop3l [c1=s0 c2=zero a1=zero a2=s2a] = x*y; sop3l [c1=1-s0 c2=s0 a1=s1a a2=s2a] = mix; rcp/log2/exp2 per channel with hw1 C10 source + chan + f16d, then `pck.c10.c10 [dmask=6]` and `[dmask=9]`; floor via `pck.f16.c10 [scale]`, fsubflr on halves, `pck.c10.f32 [scale]`; at most three internal registers; the root writes the U8 colour into pa0 | ISA.md "sop2" (formula and C10 -> U8 hardware), "sopwm", "sop3 family" (encoding), "Fields shared by hw 10-19" (INTSRCSEL, complement), "Format select and format control" (C10 enable on at start), "Float classes" (hw1 C10 source read), "hw 08: pck", "What GLSL integer code compiles to" (lowp row: corpus forms) | partial / missing (see list) |
| `sgx_compile.c:3226-3232` nosched over the C10 sequence | nosched on all but the last two instructions because internal registers don't survive a deschedule | COMPILER-UPSTREAM.md "2.3 Hardware semantics" (mentions the loss on a deschedule), ISA.md "Common fields" (nosched bit) | partial |
| `sgx_compile.c:3196-3216` `emit_feedback` | after the colour: p1 = keep; gl_FragDepth clamped to [0, 1] into `pa<max(n_pa_inputs, colour_pa + 1)>`; one `nop` feedback point (driver replaces with atst8 / `(p1) depthf`) | ISA.md "What a compiler must emit to discard", "What the draw-time builder emits", "Program structure and calling conventions" (discard, gl_FragDepth: guess) | partial |
| `sgx_compile.c:3218-3338` `emit_color` | packed BGRA8 (byte 0 B, 1 G, 2 R, 3 A); f32: `pck.u8.f32 D, g, r [dmask=6]` then `D, b, a [dmask=9]` with scale; F16: `pck.u8.f16 D, r, a [dmask=12]` then `D, b, g [dmask=3]`; D = pa0 unless both read pa0, then the first PA after the inputs; c45/c48 lane 0 for constant channels | ISA.md "Fragment shaders" (colour in pa0, byte order, hardware), "hw 08: pck" (dmask byte order src1/src2 alternation, f32 -> u8 scale rounding, hardware), "Header" (0x66-0x6c colour type/register/format) | partial (pck.u8.f16 rule) |
| `sgx_compile.c:3397-3479` `mark_helpers` | everything feeding a derivative or a dependent sample coordinate runs with skipinv clear; branch predicates in such programs too | ISA.md "Program structure and calling conventions" (Derivatives: skipinv clear on feeding math, tx_dep's coordinate math) | partial (branch predicates) |
| `sgx_compile.c:3497-3613` `allocate` | F16 vregs in r0-r31; f32 operands beside an F16 one in r0-r63; repeated-instruction and derivative destinations live while sources are read; sample destination live from issue | ISA.md "Register files and operand limits" (format select: r0-r31 / 6-bit numbers), "Float classes" (h22 fmt), "Derivatives" (d must not be x) | partial (repeat read/write order) |
| `sgx_compile.c:3618-3655` `uniform_layout` | f32 one per slot; F16 pairs (2m low, 2m+1 high); C10 three per slot, constant k of a 4-aligned group at bits 20 - 10*(k%4); a lowp w alone in channel 0; literals after | ISA.md "Lists and blocks" (list C kinds 1-4), DRIVER-UPSTREAM.md "2. The driver" row (upload rounding) | partial |
| `sgx_compile.c:3674-3743` `place_constants` | constants from sa_base; a fragment program's dependent texture state, 4 SA each, after them; all must end at sa127; spill to a memory block with one base SA per 128 dwords (address + 512*j), word = slot + 1 | ISA.md "Register files and operand limits" (Constants past sa127), "hw 1d: ld" (Address: #i reads base + 4(i + 1)), "The finalizer" (texture state 4 dwords after the program's SAs), PIGLIT.md "Uniforms past sa127" (constants past sa127 locked the 3D; spill proven) | covered |
| `sgx_compile.c:3745-3857` `mem_index` | indexed array in memory: clamp with `fminmax x, x, c48, max`, `pck.s16.f32`, `imae.sgn a, t, #4, sa<base> [t2=u32]`, `ld.a.d d, a, #i`, `wdf` | ISA.md "Register files and operand limits" (Constants past sa127), "imae", "hw 1d: ld", PIGLIT.md "Uniforms past sa127" (hardware) | covered |
| `sgx_compile.c:3859-3903` `mem_uniforms` | per spilled slot `ld.a.d t, sa<base>, #i` (drc 0), one `wdf` before the user | ISA.md "Constants past sa127", "Loads and samples" bullet, "other (h20-21 = 2)" (wdf) | covered |
| `sgx_compile.c:3905-3944` `far_f16_uniforms` | F16 constant past sa31 copied to r with `pck.f16.f16` (whole-register sources with lane) | ISA.md "Register files and operand limits" (F16 sources in r0-r31 / pa0-pa31; same limit for sa), "hw 08: pck" (lane row) | covered |
| `sgx_compile.c:3946-4003` `phys_opd`, `:4005-4082` `sgx_encode_ins` | i<n> in a slot without the i bank = r(124+n), +60 under format control; pck 16-bit sources lane = 2*half; scalar F16 source -> h7 + chan; skipinv on every ALU instruction unless helper-needed; moe when all sources step else inc bits; hw1/mov/movc rcnt or mask 1; testmask mask (1<<n)-1; fmad16 rpt 1 | ISA.md "The number field" (i<n> alias, 60 + n under format control), "hw 08: pck" (lane row), "Float classes" (hw1 source format; Repeat paragraph), "Repeats", "hw 09 test" (testmask mask) | covered (note h) |
| `sgx_compile.c:4101-4102` labels / sa_base | labels 3 start, 4 feedback, 5 phase 1; constants from sa9 (fragment) / sa10 (vertex) | ISA.md "Header" (0x3c constant SA base, 0x74/0x76/0x78 labels) | covered |
| `sgx_program.c:26-63` `smp_template` | dependent smp: dst r0, state `#0`, src2 = bias, dim 1 (2D) / 2 (cube), lod 1 bias, cfmt F16, skipinv set, drc 0 | ISA.md "hw 1c: smp", "The smp instruction, as libusc's FUN_0010a3f0 writes it", "Block 5: SAMPLE" (template), "The texture-sample sequence" | partial (skipinv on smp) |
| `sgx_program.c:154-167` branch fix-up | `br` target = label index - this index | ISA.md "flow control" (br, hardware), "Program structure" (Branches) | covered |
| `sgx_program.c:169-186` fragment `n_pa` | iterations + colour + depth; one extra PA register when nothing but gl_PointCoord is iterated (the DDK driver's placeholder colour iteration) | COMPILER-UPSTREAM.md "3. DDK-compatibility code" (names the placeholder), ISA.md "Header" (0x44 PA count) | partial |
| `sgx_program.c:188-204` vertex input mask | PA registers the program reads | ISA.md "Vertex shaders" (vertex bitmask = live pa), "Lists and blocks" item 6 | covered |
| `sgx_program.c:206-239` const map | slot, memory word (slot + 1), shift, format (1 f32, 2 F16, 3 C10, 0x10 F16 of a C10 value, 0x11 literal) | ISA.md "Lists and blocks" (list C), "Constants past sa127", DRIVER-UPSTREAM.md "2. The driver" (F16 round-to-nearest-even, C10 clamp(lrint(v*255)) +-2.0, kinds 0x10/0x11), COMPILER-UPSTREAM.md "3." (kinds 0x10/0x11 ours) | partial |
| `sgx_program.c:240-253` iterations | per iteration: set, comps, first PA, registers (F16: (comps+1)/2; sample: 1), sample/proj/sampler, pointcoord | ISA.md "Lists and blocks" (list A), PDS.md "DOUTI" | covered |
| `sgx_compiler.h:6-26` conventions | uniforms one constant per component in sa[base + k] (9 / 10); fragment varyings iterated in list-A order, iterator = set + 1; colour packed BGRA8 in pa; vertex attribute slot k at pa 4k..4k+3; outputs o0-o3 then sets then point size | ISA.md "Header" (0x3c), "Fragment shaders", "Vertex shaders", PDS.md "DOUTD" (destination attribute register is the compiler's choice) | covered |
| `sgx_compiler.h:62-79` options | `front_sa` (see front_face), `driver_consts` (kinds 0x10/0x11), `f16` | ISA.md "Fragment shaders", COMPILER-UPSTREAM.md "3." | covered (0x10's rounding: see const map row) |
| `sgx_compiler.h:96-205` driver interface structs | const map rules (fmt 3 = round(v*255) signed 10 bits; 0x10 = c/255 to nearest F16 as `pck.f16.c10 [scale]` gives it); sample info (smp before code[at], state words as src1); mem block bases; depth_pa | as the rows above | partial (as above) |
| `sgx_nir.c:24-50` NIR options | fmad not fused (product rounded); fmad16 fused shape; no integers (GLES2 ints as floats) | ISA.md "Register files and operand limits" (Rounding), "Comparisons and conversions" (GLSL ES ints are floats) | covered |
| `sgx_nir.c:58-231` `sgx_assign_varyings`, `sgx_pack_varyings` | 10 sets x 4 components; order mat4, mat2, vec4, mat3, vec3, vec2, float, larger arrays first; one set per row; set 0 reserved (2 comps) when gl_PointSize is written or gl_PointCoord read; past 10 sets pack rows into free components | ISA.md "Vertex shaders" (Varyings to sets, gl_PointSize, Matching the fragment shader), STATE.md "Lines and points" (coordinate set tc0, only needed when gl_PointCoord is read) | covered |
| `sgx_nir.c:233-264` `lower_sincos` | t = 4 * fract(x / 2pi + c) - 2 (c = 0.5 sin, 0.75 cos), `fsin(t)` = sin(t * pi/2) | ISA.md "Register files and operand limits" (Scalar functions: fsin quarter turns, period 4, ~16 bits) | partial |
| `sgx_nir.c:266-321, 1065-1088` sampler / uniform numbering | samplers numbered in order of first use; uniforms vec4-aligned in declaration order unless the caller places them | COMPILER-UPSTREAM.md "3." (glslcompiler numbering, first-use sampler order), ISA.md "Block 5" (sampler 0..15) | n/a (DDK compatibility, not hardware) |
| `sgx_nir.c:323-334` `front_true_instr` | gl_FrontFacing = true when the driver gives no front_sa | - | n/a |
| `sgx_nir.c:336-512, 1091-1235` int16, cmp widening, copysign, pipeline | NIR lowering | - | n/a (algorithm) |
| `sgx_nir.c:516-662, 705-800` `lower_uniforms`, `uniform_formats` | mediump uniforms as F16 constants; a lowp vec4 (4-aligned, read for xyz) as a C10 triple + w; highp f32, converted with truncating `pck.f16.f32` | ISA.md "Lists and blocks" (list C kinds 1/2/3: highp / mediump / lowp in the corpus), "hw 08: pck" (f32 -> f16 truncates) | partial |
| `sgx_nir.c:915-938` vertex attributes | attribute slot a at pa 4a..4a+3 (matrix columns consecutive) | PDS.md "DOUTD" (word 1 bits 8-21 destination register, the compiler's choice), ISA.md "Vertex shaders" | covered |
| `sgx_nir.c:939-954` vertex outputs | o0-o3 position, enabled sets in order with their sizes, then the point size | ISA.md "Vertex shaders" (The output buffer) | covered |
| `sgx_nir.c:955-1063` fragment iterations | gl_FragCoord first (iterator 14, 4 comps, f32); then one iteration per used set in set order (all of the set's used components); F16 when every varying of the set is mediump/lowp and single-row; gl_PointCoord = set 0 (F16 under f16) | ISA.md "Lists and blocks" (list A: iterator 14, set + 1, format, registers), PDS.md "DOUTI" (source 0 = position: x, y, z, 1/w; bits 27/28 precision; w1 b4 sprite tc0 x2 f16), STATE.md "Lines and points" (sprite tc0) | partial (order; F16 precision bit is a guess in PDS.md) |

Notes on `sgx_forms.c` against ISA.md:

- (a) `DST_FILES` includes `idx` for every form using the generic
  destination. ISA.md "hw 08: pck" lists the pck destination as "r o pa sa g
  i ix" (no idx). The compiler only writes idx with ima16, so nothing wrong
  is emitted, but the slot sets are wider than the class tables.
- (b) movc's src0 kinds (r/pa only) follow from the generic table ("none in
  the float classes"; the classes with an extension are listed), but
  "Moves (hw 5)" doesn't say it for movc itself.
- (c) TEST/TESTMASK src1/src2 carry `fmt`, but `legalize` keeps F16 halves
  out of testmask (ISA.md: testmask's destination numbering under fmt not
  drawn). Consistent.
- (d) The sop/sopwm forms don't record that format control (C10 on at
  program start) makes every r/o/pa/sa/i number 6-bit. ISA.md states it
  ("Format control": "those instructions can only name registers 0-63").
  The compiler never checks it: a C10 uniform (`c10_uref`) lands at
  `sa_base + slot`, and nothing keeps that below sa64. A gap in the code, not
  the docs.
- (e) `sgx_encode_ins:4019-4024` turns i<n> in a slot without the i bank into
  r(60 + n). That is the format-control form with the C10 flag (ISA.md "The
  number field"); it is right only because every such operand is a C10
  `iref(..., true)`. A non-C10 i operand there would need r(124 + n).
- (f) `sgx_form_init` sets the unused-source pattern `#0` in src2 for smp
  and ld only. ISA.md "Unused slots and fixed patterns" says mov (and rlp)
  also write it. Settled: the XML's mov leaf fixes src2 to `#0` itself, and
  the compiler's switch to the generated encoder was byte-identical on the
  corpus, mov included.
- (g) `sgx_form_init` sets mask = 1 for every form except sopwm (15). ISA.md
  "Text notation and defaults" gives mask default 1 only for smp, pck,
  test, testmask and the bitwise classes, "0 elsewhere", while "Float
  classes" (hw1, hardware) says mask 0 writes nothing. `sgx_encode_ins`
  sets mask 1 itself for one-iteration hw1/mov/movc, which matches the
  hardware statement. The words are settled (byte-identical on the corpus
  with the encoder switch); ISA.md's "0 elsewhere" should say which classes
  have a mask field at all.
- (h) The compiler never emits `end`, `phas` or the vertex epilogue; the
  driver adds them. Some forms the compiler emits have no end bit (fmad16:
  "so fmad16 has no end bit"; test "rejects it"), so the driver must append
  `nop [end]` when one is last.

#### Facts the docs need

Each bullet is a fact the code relies on that a spec section has to state.
The target doc is named.

**Repeats** (`form_class`, `max_rpt`, `emit_runs`, `allocate`, `legalize`):
- ISA.md "Moves (hw 5)": with `rcnt`, every operand of mov/movc steps by one
  register per iteration (no per-source hold), up to 16, and whether that
  stepping uses the MOE increments; with h21 clear, mask = 1 writes one
  iteration (mask 0 writes nothing, as for hw1).
- ISA.md "hw 09 test, hw 0f testmask": in testmask's mask mode (rpt n ->
  mask (1<<n)-1), the destination and both sources step per channel, and a
  constant source steps too (c48-c51 / c52-c55).
- ISA.md "Repeats" or "Float classes": the order of reads and writes inside
  a repeat: does iteration n read its sources after iteration n-1 wrote its
  destination (what `emit_runs` assumes), and may a destination overlap a
  later iteration's source? `allocate` treats the destination as live while
  the sources are read; the doc should say which is true.

**Packing conversions** (`hw 08: pck`; all in ISA.md "hw 08: pck" "Notes"):
- 16-bit destinations (f16, s16, u16): dmask 3 writes the low half and 0xc
  the high half, the single enabled half takes src1, and dmask 0xf takes
  src1 low and src2 high (only s16.f32 is stated now). Used by every
  `pck.f16.f16`.
- `pck.f16.c10 [scale]`: result = c / 255 for the signed 10-bit channel the
  lane picks (clamped to +-2?), rounded to nearest F16. The compiler (load
  of a lowp uniform, C10 floor) and `driver_consts` kind 0x10 depend on it.
  COMPILER-UPSTREAM.md's table names a tests/c10 rounding check, but ISA.md
  doesn't give the rule.
- `pck.c10.f32 [scale]`: f32 -> C10 = round(v * 255) clamped to the 10-bit
  signed range (or to +-2.0), and the rounding mode.
- `pck.c10.c10` (with and without scale): for a C10 destination, dmask bits
  are channels (bit 0 channel 0 ... bit 3 alpha); the enabled channels take
  src1, src2, src1, src2 in order; what `.l<n>` picks on a C10 source,
  including `.l3` (alpha); and what dmask 0xf with no lanes does
  (`pck.c10.c10 i, xyz, w [scale]` puts the xyz triple in channels 0-2 and the
  w slot's channel 0 in the alpha).
- `pck.f16.u8 [scale]` and `pck.u8.f16 [scale]`: the same conversion rules as
  the f32 ones (b/255, round to nearest F16; x*255 rounded to nearest and
  clamped). Only the f32 forms are hardware-stated.

**C10 colour classes** (ISA.md "sopwm", "sop3 family"):
- sopwm semantics with C10 operands and C10 / U8 destinations (the sop2
  hardware results cover sop2 only): `dst = (src1*c1) cop (src2*c2)` per
  channel, alpha by `aop`, saturation/clamping, and that c1 = c2 = `one`
  (select zero + complement) gives a plain add/sub/min/max.
- sop3 and sop3l formulas: which source each colour select multiplies, what
  `cop`/`aop` combine, and the alpha computation. The compiler relies on:
  `sop3 d, z, x, y [cop=sub c1=one c2=s0 a=one aop=sub]` = x - y*z;
  `sop3l d, y, x, x.w [cop=add c1=s0 c2=zero a1=zero a2=s2a]` = x*y (alpha
  y.a*x.a); `sop3l d, t, a, b [c1=1-s0 c2=s0 a1=s1a a2=s2a]` = mix(a, b, t).
- A U8 destination of sopwm/sop3/sop3l from C10 sources: bytes 0-2 from
  channels 0-2, alpha byte from what (internal register: full alpha; 32-bit
  register: channel 0).
- ISA.md "Registers": internal registers hold four full C10 channels (the
  40-bit claim is "presumably" now), so a C10 alpha survives in i0-i2.
- ISA.md "Registers" or "Common fields": internal registers are lost when
  the task is descheduled; the rule for `nosched` (which instructions of a
  sequence that keeps values in i0-i3 must carry it; the compiler leaves the
  last two unmarked). COMPILER-UPSTREAM.md only mentions it.
- ISA.md "Float classes" (hw1 source format): hw1 with a C10 source (h8) and
  h2 (`f16d`) set: the result format (C10?) and which channel of the
  destination it lands in (the compiler reads it back at `chan`'s channel),
  and whether the other channels are zeroed.

**Constants and uniforms** (`uniform_layout`, `load_uniform`, const map):
- ISA.md "Lists and blocks" (list C) or a driver-interface section: the bit
  layout of a C10 triple in an SA word (constant k of a 4-aligned group at
  bits 20 - 10*(k%4): x in channel 2, y in 1, z in 0), a lone lowp w as
  channel 0 of its own word, the F16 pair rule (2m low half, 2m+1 high half),
  and the upload conversions (F16 round to nearest even, C10
  clamp(lrint(v*255)) +-2.0; now only in DRIVER-UPSTREAM.md).
- ISA.md "Registers" (indexed operands): the value an index register holds
  for `bank[idxN + off]` is an absolute register number in that bank (the
  compiler loads `t + sa_base + k`), and the offset is added in registers.
- ISA.md "Register files and operand limits": F16 operands under format
  select reach sa0-sa31 only (stated for r/pa; the compiler applies it to
  sa, `far_f16_uniforms`).

**Textures** (`emit_tex`, `smp_template`, PDS DOUTI):
- PDS.md "DOUTI": the sample-half encoding of a non-dependent cube read
  (3-component coordinate), which the compiler produces for `textureCube`
  of an untouched varying; only 2-D (0x200) and projected (0x400) are stated.
- PDS.md "DOUTI": a projected sample divides x and y by the set's w
  (component 3), on hardware (now "captures only show 2-D").
- PDS.md "DOUTI": a non-dependent sample reads its set's iterated value at
  full precision whatever the plain iteration of the same set uses (F16 or
  f32), and a 2D sample of a set with more than 2 components uses x, y.
- ISA.md "hw 1c: smp": F16 coordinates (h3) are packed two per register
  (x low half, y high half; a cube's z in the next register's low half),
  starting at a whole register.
- ISA.md "hw 1c: smp" or "The texture-sample sequence": whether the smp
  itself must have skipinv clear for its LOD (libusc's `tx_dep` template
  has h23 clear, `tx_dep_if` / `vtx_lod` set); the compiler sets it.
- ISA.md "The texture-sample sequence" / TEX.md: what raw data the sampler
  returns for formats other than RGBA8888 (565, 4444, 5551, L8, A8, LA88):
  the compiler always unpacks one dword as R, G, B, A bytes.

**Discard, depth and phases** (`emit_feedback`, `n_pa`):
- ISA.md "What a compiler must emit to discard": whether the feedback point
  may come after the colour packing (phase 1 empty, as the compiler emits
  it) rather than before it (the DDK's layout), given that atst8 reads and
  writes the colour register.
- ISA.md "Phases, visibility feedback and discard": which PA registers the
  driver's extra feedback iterations (`pcoeff`'s pa<n>.., the points path's
  second one) occupy relative to the program's own PA use, so that
  `depth_pa = max(n_pa_inputs, colour_pa + 1)` doesn't overlap them; and
  the hardware meaning of `(p1) depthf pa<z>, sa3, sa4` (now a guess).
- ISA.md "Lists and blocks" / "Header" (0x44): the rule that a fragment
  program with no iterated input other than gl_PointCoord gets one placeholder
  colour iteration (one more PA register), and whether the hardware needs at
  least one iteration or only the DDK's driver does.
- ISA.md "Program structure and calling conventions": the list of forms
  without an end bit (fmad16, efo, fdual, test) and the vertex epilogue
  (`phas.i #0, #0 [wait=7]; emit.t1 r0, #0, #0 [end f12 f10 f3_20]`, TA locks
  up without it; now only in GEN.md and NOTES.md), so the driver's
  finalisation of compiler output is specified in one place.

**Control flow** (`emit_cf_list`, `mark_helpers`):
- ISA.md "Program structure and calling conventions": when a branch needs
  `syncend` (the corpus's `tx_dep_if` has `(!p0) br [syncend]` around a block
  with derivatives and a sample; the compiler never sets it).
- ISA.md "Derivatives": that predicates of branches in a program with
  derivatives or dependent samples must be computed on helper instances too
  (skipinv clear), so all instances of a quad take the same path.

**sin/cos** (`lower_sincos`):
- ISA.md "Scalar functions": fsin's accuracy over its input range (the code
  assumes it is only usable on [-2, 2)), and the reduction the compiler
  uses (4 * fract(x/2pi + 0.5) - 2 for sin, + 0.75 for cos through fsin);
  the current text describes reduction "then * c36", which isn't what the
  code does.

**Fragment iteration order** (`assign_io`):
- ISA.md "Lists and blocks" (list A): gl_FragCoord's iteration comes first
  in PA order, then one iteration per set in set order covering all of the
  set's used components (a convention shared with the driver's DOUTI
  builder, not hardware).
- PDS.md "DOUTI": bits 27/28 -> F16 iteration is a guess now; the compiler's
  F16 PA layout depends on it.

### instruction sets: `mesa/src/sgx545/isa`, `mesa/src/sgx545/pds`

#### USSE instruction set: `mesa/src/sgx545/isa`

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `sgx545.xml` (4261): classes hw 00-1f, every leaf's fields and fixed patterns | the 64-bit USSE instruction words | ISA.md "Encoding conventions" (operand slots, bank codes, number field, format select/control, unused slots), "Instruction classes", each class section ("Float classes", "Moves", "Dual issue", "Pack/unpack, test, testmask, bitwise", "Integer, fixed-point and multi-unit ops", "smp, ld/st, and the special class") | covered |
| `sgx545.xml` displays, `sgx545_isa_asm*.c` | the text notation | ISA.md "Text notation and defaults" | covered (the assembler doesn't take all of it yet: efo `i=`, dot `cs=`/`as=`, `firh.s8`, st `am3` `.q`, rlp `unused2=`, fdual `sd=`/`sb=`/`sc=`; a code gap, not a doc gap) |
| `sgx545_isa_encode.c`: `sgx_isa_opnd_num`, `sgx_isa_noext`, `SGX_TDST` | g<n> = n + 0x40; i<n> in a slot without the i bank = r(124 + n); test's dst field 0x280 without a destination | ISA.md "The number field"; "hw 09 test, hw 0f testmask" (l20 row) | covered |
| `sgx545_isa_encode.c`, `sgx545_isa_disasm.c`: format control through `ISA_GPU_ID()`; `is_setfc` | setfc's arguments (a0 efo F16, a1 C10), program-start state C10 on / F16 off | ISA.md "Format select and format control"; "Fields shared by hw 10-19 and 1a"; "MOE control" | covered |
| `sgx545_isa.h` `struct sgx545_isa_fc` | one operand format per slot | ISA.md "hw 1" source format (h7 F16 / h8 C10) | covered; the struct can't hold h7 and h8 set together (an fcos with both encodes h7 wrong): a code gap |
| `sgx545_isa_enums.py`, `meson.build`, `isa.h`, `tests/*` | build glue, tests | - | n/a: no hardware facts |

#### PDS programs: `mesa/src/sgx545/pds`

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `sgx545_pds.xml` `#instruction` leaves | MOVS, ADD/MUL16/OR/AND/XOR and the unnamed s=1 forms, TST, BRA, SHL/SHR, HALT; U/S1/S2 operand numberings | PDS.md "Instruction encoding" and its subsections; "Bits no field names" | covered |
| `sgx545_pds.xml` data-word roots | DOUTU w0/w1, DOUTD control, DOUTI w0/w1, DOUTA control | PDS.md "DOUT word layouts" (DOUTU, DOUTD, DOUTI, DOUTA); "Bits no field names" | covered (meanings of DOUTD b22-25, DOUTU w1 b2/b22/b23, DOUTI b25-26 and the precision values are open questions in PDS.md; the values used are stated) |
| displays, `sgx545_pds_asm.c`, `sgx545_pds_disasm.c` | the text notation | PDS.md "Text notation" | covered |
| `sgx545_pds.h` `sgx545_pds_ds0_at`/`ds1_at` | data-segment interleave of DS0/DS1 | PDS.md "Program layout" | covered |
| `isa.h`, `meson.build`, `tests/*` | build glue, tests | - | n/a: no hardware facts |

### kernel driver: `drm/`

`drm/powervr_sgx/powervr_sgx_stream*.{c,h}` and `powervr_sgx_cmd_checks.c`
are byte-identical to `docs/uapi/stream/`, which is generated from
`docs/uapi/powervr_sgx_kmd_stream.xml`, so UAPI.md §3 covers them.
`include/uapi/drm/powervr_sgx_drm.h` is `docs/uapi/powervr_sgx_drm.h`.

#### Table

##### Registers (`pvrsgx_regs.h`, used across the driver)

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `pvrsgx_regs.h:23-24` CORE_ID / CORE_REVISION | offsets 0x1c, 0x20; safe to read on a core in any state | DRM-DRIVER.md "3. Bringing the GPU up" (step 1: names + safe); UAPI.md "DEV_QUERY", "7.2" (values 0x01150000, 0x1000e) | partial: offsets 0x1c/0x20 not stated |
| `pvrsgx_regs.h:20-22` clock gating | CLKGATECTL 0x0000, CLKGATECTL2 0x0004, CLKGATESTATUS 0x0008 | SRVINIT.md "SGX_BRIDGE_INIT_INFO" (ClkGateStatusReg 0x8, mask 0xffffffdf; ClkGateCtl values); DRM-DRIVER.md "3." step 6 | partial: offsets of CLKGATECTL (0x0) and CLKGATECTL2 (0x4) not stated |
| `pvrsgx_regs.h:26-28` SOFT_RESET | offset 0x80; bit 0 = BIF; 0x3e = DPM, TA, USE, ISP, TSP | UKERNEL.md "5a" (name only) | missing: offset and every bit |
| `pvrsgx_regs.h:30-40` event registers | HOST_ENABLE2 0x110, HOST_CLEAR2 0x114, STATUS2 0x118, STATUS 0x12c, HOST_ENABLE 0x130, HOST_CLEAR 0x134; STATUS b31 MASTER_INTERRUPT, b29 TIMER, b14 SW_EVENT; HOST_CLEAR b31, b14 | SRVINIT.md "Init scripts" (0x110, 0x130); UKERNEL.md "2. The event dispatcher" (SW_EVENT 0x4000, TIMER 0x20000000, ack via `str 0x134`), "1." (master-interrupt bit by name) | partial: 0x114, 0x118, 0x12c offsets and MASTER_INTERRUPT = b31 (status and clear) not stated |
| `pvrsgx_regs.h:41` TIMER | 0x144 (defined, unused) | UAPI.md "7.2" (name only) | missing: offset |
| `pvrsgx_regs.h:43-47` code bases, kicker, event timer | USE_CODE_BASE(n) = 0xa0c+4n, PDS_EXEC_BASE 0xab8, EVENT_KICKER 0xac4, EVENT_TIMER 0xacc | SRVINIT.md "Init scripts" | covered |
| `pvrsgx_regs.h:46-49` doorbells | EVENT_KICK 0xac8, EVENT_KICK2 0xae8, KICK_NOW = bit 0 | UKERNEL.md "1." and "2." (names, roles); SRVINIT.md "Init scripts" (name); DRM-DRIVER.md "3." step 8 (KICK2) | partial: offsets 0xac8/0xae8 and the written value (1) not stated |
| `pvrsgx_regs.h:51-54` BIF_CTRL | 0xc00; b1 PAUSE (unused), b3 INVALDC, b4 CLEAR_FAULT (unused) | UKERNEL.md "6. Register sequences" (BIF_CTRL bit 3 INVALDC) | partial: offset 0xc00, PAUSE b1, CLEAR_FAULT b4 not stated |
| `pvrsgx_regs.h:55-58` fault registers | BIF_INT_STAT 0xc04 (b0-15 faulting requestors, b16-18 fault type); BIF_FAULT 0xc08 (faulting address in b12-31) | DRM-DRIVER.md "4." and "6." E1 (BIF_FAULT named, example values 0x3000b021, 0x37021002) | partial: both offsets, INT_STAT fields, BIF_FAULT address mask and low-bit meaning not stated (UAPI.md G3) |
| `pvrsgx_regs.h:59-84` directory lists, banks | DIR_LIST_BASE0 0xc84, BASE1..15 = 0xc38+4(n-1) (15 = 0xc70), 16 lists, BANK_SET 0xc74, BANK0 0xc78: EDM b0-3, TA b4-7, HOST b8-11, 3D b12-15, 2D b16-19; EDM uses list 15 | UKERNEL.md "6." (0xc84, BANK0 0xc78, EDM b0-3, TA b4-7, value 0x0f); DRM-DRIVER.md "4. Address translation" (16 lists, EDM list 15) | partial: BASE1..15 offsets, BANK_SET 0xc74, BANK0 HOST/3D/2D fields not stated (UAPI.md G2); value format of a list base (UKERNEL.md 6 says "shifted"; code writes the raw PD physical address) |
| `pvrsgx_regs.h:70-71` BIF_MEM_REQ_STAT | 0xca8, READS b0-10 (defined, unused) | none | missing |
| `pvrsgx_regs.h:86-100`, `pvrsgx_mmu.c` (`pte_get`, `pte_set`) | MMU: 2 levels, 1024-entry PD and PT, 4 KiB pages, 4 MiB per PDE, 32-bit entries, address b12-31; PDE valid b0, 4K page-size field 0; PTE valid b0, WRITEONLY b1, READONLY b2, CACHECONSISTENT b3, EDMPROTECT b4; PD physical address goes to a directory list | DRM-DRIVER.md "4." (32-bit physical entries, 4 MiB directory entries), "6." rule 4 + E1 (READONLY enforced for `st` and PBE); UAPI.md "5." (4 KiB pages) | partial: the entry formats, bit positions and page-size field are not stated (UAPI.md G3) |
| `pvrsgx_regs.h:15-18` comment | only CLKGATECTL, CLKGATESTATUS, SOFT_RESET, EVENT_STATUS, EVENT_HOST_ENABLE, BIF_CTRL, CORE_ID, CORE_REVISION are safe on an uninitialised core; USE_CODE_BASE_0 hangs the bus until reset | DRM-DRIVER.md "3." step 1 (CORE_ID/REVISION only); "5." (a read of the gated core froze the SoC) | partial: the safe list and the USE_CODE_BASE_0 hang not stated |

##### Device bring-up (`pvrsgx_device.c`, `pvrsgx_drv.c`)

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `pvrsgx_drv.c:212-247` `pvrsgx_probe`, `:271` id table; Kconfig | PCI 8086:089b; BAR0 at least 0x4000 bytes; 32-bit DMA mask; shared INTx | DRM-DRIVER.md intro (01:02.0 8086:089b), "1. Status" (BAR0, INTx), "4." (32-bit DMA mask) | partial: BAR0 size (0x4000 minimum) not stated |
| `pvrsgx_drv.c:101-117` LIMITS_GET | rt_max 2048x2048, samples 1, PB 0x100 / 0x400 / 0x8000 pages, max_ta_kicks 64 | UAPI.md "DEV_QUERY" (default 0x400), "CREATE_CONTEXT" (3D params under 0x10000 pages), G14 | partial: RT limits are G14; PB min 0x100 and max 0x8000 not stated; max_ta_kicks 64 has no source and the driver accepts at most 2 kicks (`PVRSGX_MAX_TA_KICKS`, -EOPNOTSUPP) |
| `pvrsgx_device.c:59-111` `pvrsgx_kmem_alloc` | kernel memory below 4 GiB (DMA32), write-combined | DRM-DRIVER.md "4." (DMA32, write-combine, G19) | covered |
| `pvrsgx_device.c:30-34` `pvrsgx_kernel_heaps` | sync 0x2d000000-0x2e000000, kernel code 0x30000000-0x30400000, kernel data 0x32000000-0x37000000, each whole 4 MiB PDEs | UAPI.md "5. Heap layout"; DRM-DRIVER.md "4." | covered |
| `pvrsgx_device.c:114-192` `pvrsgx_reset`, `soft_reset`, `inval_dc`, `reset_sleep` | reset sequence: SOFT_RESET = 0x3f; BIF_CTRL, BANK_SET, BANK0, all 16 list bases = 0; list 0 = scratch PD; SOFT_RESET = 0x3e (BIF out, cores held); INVALDC pulse; up to 32 rounds of: read BIF_INT_STAT, BIF_FAULT, re-reset with BIF, map scratch page at the fault, write STATUS/STATUS2 back to HOST_CLEAR/CLEAR2, release, INVALDC; then BANK0 EDM = 15, list 15 = firmware PD, list 0 = idle PD, INVALDC, SOFT_RESET = 0; settle = a SOFT_RESET read + 1 us | DRM-DRIVER.md "4." ("Reset (`pvrsgx_reset`) follows the DDK's recovery path...") | partial: sequence described in one sentence; register values, order, the fault-drain loop's reads/acks/bound, the settle delay and the BIF-out/cores-in intermediate state not stated |
| `pvrsgx_device.c:195-227` `pvrsgx_irq`, `pvrsgx_irq_thread` | interrupt = EVENT_STATUS & HOST_ENABLE has SW_EVENT; ack SW_EVENT + MASTER_INTERRUPT via HOST_CLEAR; host control INTERRUPT_FLAGS (word 8) HWR b0 set with INTERRUPT_CLEAR (word 9) HWR clear = lockup report; lockup count word 3 | UKERNEL.md "2." (SW_EVENT raised by the microkernel, host ISR clears it, reads ui32InterruptFlags), "4." (word 8 b1 ACTIVE_POWER; ClearFlags b0 HWR) | partial: ack register/bits, word indexes 3 and 9, and the "HWR flag set, clear-flag not set" lockup reading not stated |
| `pvrsgx_device.c:317-388` runtime PM | no PCI PM capability (stays D0); suspend = microkernel power-off; resume = full start without the quiesce (a read of the gated core froze the SoC); 500 ms autosuspend | DRM-DRIVER.md "5. Jobs" (Runtime PM bullet) | covered |
| `pvrsgx_device.c:243-255` | kernel heaps' page tables preallocated so their PDEs never change (needed for sharing) | DRM-DRIVER.md "4." (page tables shared, not copied) | covered |

##### Firmware (`pvrsgx_fw.c`, `pvrsgx_fw.h`)

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `pvrsgx_fw.c:40-41, 62, 708-723` names, `ddk_load` | file names; DDK image is exactly 0xcf68 bytes (libsrv_init .rodata 0x2000..0xef68) | DRM-DRIVER.md "3." step 2; SRVINIT.md "The firmware file"; UKERNEL.md "4a" | covered |
| `pvrsgx_fw.c:66-100, 613-706` `ukfw_header`, `ukfw_parse`, `ukfw_table`, `ukfw_load` | open microkernel file: magic, header fields and offsets, header 0x60 min and multiple of 8, image multiple of 8, CRC-32 IEEE, interface major 1, feature bits 0/1 only, entry program 0..4, entry offset < image and 8-aligned, patch offset <= image-8 and 8-aligned, patch value 1..5, program 0 required, core_rev mismatch only warns; tables at most 64 pairs at 4-aligned offsets; image at most CODE_ARENA - 0x200 - 0x10 = 0xddf0 bytes | UKERNEL.md "4a. The firmware file" | partial: not stated: the 64-pair table limit, 4-byte table alignment, 8-byte alignment of entry and patch offsets, the image size limit 0xddf0, that a missing program-0 entry is refused, that core_rev mismatch is a warning not a refusal |
| `pvrsgx_fw.c:214-227` `limm` | placeholder: word 0 low 21 bits 0xdbeef; DDK word 1 = 0xfc237150; open image: word 1 bits 4-8 and 12-17 clear; split b0-20 -> w0, b21-25 -> w1 b4-8, b26-31 -> w1 b12-17 | SRVINIT.md "Buffer contents" (Microkernel); UKERNEL.md "4a" (0x000dbeef in low 21 bits) | partial: the open image's word-1 check (bits 4-8, 12-17 zero) not stated |
| `pvrsgx_fw.c:464-494` `fw_build_open` | five PDS programs, each a DOUTU (word 1 = 2) to its listed entry; programs not listed enter at program 0's offset; patches filled from value 1..5; host-kick table = identity | UKERNEL.md "4a" (program 0 bare DOUTU bit 27, values 1..5, identity); SRVINIT.md "PDS programs" (word 1 = 2) | partial: the fallback of unlisted programs 1-4 to program 0's offset not stated; SRVINIT.md "Loading openHC's own microkernel" says the `-o` event program also DMAs the TA3D block (DOUTD 0x3c0003f), which conflicts with UKERNEL.md 4a's bare DOUTU that the driver builds |
| `pvrsgx_fw.c:107-121` `struct_sizes` | 13 SGX_MISCINFO_STRUCT_SIZES values in report order | SRVINIT.md "SGX_BRIDGE_INIT_INFO" | covered |
| `pvrsgx_fw.c:124-133` `host_kick` | entries 0x11d, 0x103, 0xd4, 0xeb, 0x13a, 0x173, 0x156, [9] 0x157 | SRVINIT.md "SGX_BRIDGE_INIT_INFO" | covered |
| `pvrsgx_fw.c:139-164` `layout`, `:741-783` `pvrsgx_fw_init` | kernel-data offsets/sizes of the 9 buffers, image at kernel-code 0, PDS programs at image end aligned 0x10 (+0, 0x20, 0x60, 0x90, 0x190), arenas 0xe000 each; data PTEs CACHECONSISTENT+EDMPROTECT, code READONLY | SRVINIT.md "Allocations" (addresses, sizes); DRM-DRIVER.md "3." step 3 (14 pages) | partial: covered for the DDK image; PTE flag bits are G3 (see MMU row); SRVINIT's attribute flags (0x4f / 9) are services flags, the mapping to PTE bits is not stated |
| `pvrsgx_fw.c:166-172`, `fw_build:497-512` signature buffers | count, zero word, register list; TA 6 regs, 3D 32 regs | SRVINIT.md "Buffer contents" | covered |
| `pvrsgx_fw.c:202-207` `doutu` | DOUTU word 0 for a kernel-code address: base 15 + segment, bit 27, offset/8 << 4 | SRVINIT.md "Buffer contents" (PDS programs, `doutu(e)`); PDS.md "DOUTU" | covered |
| `pvrsgx_fw.c:229-320` PDS encoder | U numbering (DS0 0-0x3f, DS1 0x40-0x7f, inputs 0x80+), data rows DS0 DS0 DS1 DS1; MOVS layout and DOUT dests 3/5/6; ALU AND (c,s1) / XOR (d,s0); TST with sel b8; BRA 0x9; HALT 0xaf000000; cc 7/0 | PDS.md "Instruction encoding" (MOVS, Arithmetic and logic, TST, BRA, HALT) | covered |
| `pvrsgx_fw.c:322-435` `pds_entry`, `pds_ta3d`, `pds_events`, `pds_events2` | pds0 (0xd60), pds1 (DOUTD 0x3c0003f if in0==0, then 0xdf0 with no bit 27), pds2 (0xe20), pds3 (masks 0x100 / 0x400090 / 0x200 / 0x300400 -> 0x30 / 0x7050 / 0x2cd0 / 0x5670), pds4 (0x4000f / 0x120080 -> 0x73a0 / 0x2ea0 / 0x5740) | SRVINIT.md "Buffer contents" (PDS programs table); PDS.md "What the DDK generates" | covered (behaviour and every constant; the instruction-by-instruction order of pds3/pds4 is not written out, the code matches SRVINIT's byte-identity claim) |
| `pvrsgx_fw.c:514-534` DDK patch list | nine limm sites and values | SRVINIT.md "Buffer contents" (Microkernel table) | covered |
| `pvrsgx_fw.c:442-458` `ta3dctl_fill` | TA3D block words 0x00, 0x01, 0x02, 0x13, 0x16, 0x17 (firmware PD), 0x2b, 0x2c, 0x2d; zeroed at every start | SRVINIT.md "Buffer contents"; UKERNEL.md "1." table, "5a" (discard) | covered |
| `pvrsgx_fw.h:33-47` host control words | 0 init status, 1 power status, 2 cleanup status, 3 uk lockups, 4 host lockups, 5 HWR sample rate, 6 uk timer clock, 7 APM sample rate, 8 interrupt flags, 9 interrupt clear, 13 host clock, 14 assert fail, 16 cleanup seq (v2) | UKERNEL.md "1." (field names, no offsets), "4." (word 8; first word = init status), "Cleanup v2" (+0x40 word 16); SRVINIT.md "Allocations" (0x44 size) | partial: word indexes 1-7, 9, 13, 14 not stated |
| `pvrsgx_fw.h:53-66` host control bits, cleanup types | INIT_COMPLETE b0; CLEANUP COMPLETE b0 / BUSY b1 / DONE b2; cleanup types RT 1, RC 2, TC 3, PB 5; POWMAN_POWEROFF_COMPLETE b3; POWMAN_NO_WORK b5 (unused); INTERRUPT HWR b0, ACTIVE_POWER b1 | UKERNEL.md "4." (INIT_COMPLETE, HWR b0, ACTIVE_POWER b1), "Cleanup" table (RT 1, RC 2, PB 5), "Cleanup v2" (bits 0-2), "Power (type 3)" (names only) | partial: TC = 3, POWEROFF_COMPLETE = b3, NO_WORK = b5 not stated |
| `pvrsgx_fw.c:855-885` `fw_check` | GETMISCINFO: data[1] = misc buffer; misc[0] request 0x2 (GET_STRUCT_SIZES), READY = b0, 500 ms; misc[1] core rev, [2] core id, [3] DDK version, [4] build, [7] build options, [8..20] struct sizes | UKERNEL.md "Get misc info (type 6)"; DRM-DRIVER.md "3." step 10; SRVINIT.md "Allocations" (0x54 bytes) | partial: the misc-info word layout, the request value 0x2 and READY bit value not stated |
| `pvrsgx_fw.c:538-611` `cleanup_v2`, `pvrsgx_fw_cleanup` | v2: data[2] nonzero seq, status cleared first, COMPLETE + echo at word 16, DONE vs BUSY, 1 ms retry, 1 s give-up; v1: COMPLETE polled 5 s, then cleared by the host; data[0] type, data[1] address; afterwards the next command requests a data-cache invalidate (0x8) | UKERNEL.md "Cleanup (type 5)", "What a kernel driver needs from it", "Cleanup v2" | partial: the host clearing COMPLETE after v1, the per-poll 5 s bound in v2, and the post-cleanup INVAL_DATA request not stated |
| `pvrsgx_fw.c:803-838` `init_script_part2` | part-2 register writes and the code-base formulas | SRVINIT.md "Init scripts" (Part 2) | covered |
| `pvrsgx_fw.c:841-852` `deinit_script` | zero 0xa58, 0xa68, 0xa6c, 0xa70, USE_CODE_BASE_15, both host-enable regs, both clock-gate regs | SRVINIT.md "Init scripts" (Deinit); UKERNEL.md "6." | covered (clock-gate offsets: see CLKGATECTL row) |
| `pvrsgx_fw.c:891-977` `pvrsgx_fw_start` | host control: word 5 = 1000/100 = 10, word 6 = core_clock/1000 (core_clock default 400 MHz), word 7 = 0, word 1 = 0, word 13 = host time in us, word 0 = 0; first command's cache control = PD invalidate (0x2); CLKGATECTL 0xa6aa, CLKGATECTL2 0xaaaaaa, 0xa58 = 0, EVENT_TIMER = 0; reset; kicker = 0; part 2; wmb; KICK2; poll INIT_COMPLETE 500 ms; restart: deinit, SOFT_RESET 0x3f, bus mastering off 10 ms | DRM-DRIVER.md "3." steps 5-9, "5." (restart_quiesce); SRVINIT.md "Init scripts" (part 1) | partial: core clock value (400 MHz), the host-clock word, the first-command PD invalidate (SRVINIT's `ui32CacheControl` 2 is not tied to it), and word indexes (see host control row) not stated |
| `pvrsgx_fw.c:980-1004` `pvrsgx_fw_stop` | POWER command data[1] = 1 (POWEROFF); poll POWEROFF_COMPLETE (b3) 500 ms; wait until CLKGATESTATUS & 0xffffffdf == 0 (500 x 100-200 us); deinit; SOFT_RESET 0x3f held | DRM-DRIVER.md "3." (Stopping); UKERNEL.md "Power (type 3)"; SRVINIT.md "SGX_BRIDGE_INIT_INFO" (status reg/mask) | partial: POWEROFF value 1, POWEROFF_COMPLETE bit, "masked status reads 0 when gated", and the SOFT_RESET hold value not stated |

##### Kernel CCB (`pvrsgx_ccb.c`, `pvrsgx_ccb.h`)

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `pvrsgx_ccb.h:12-24` command types | TA 0, TRANSFER 1, 2D 2, POWER 3, CONTEXTSUSPEND 4, CLEANUP 5, GETMISCINFO 6, PROCESS_QUEUES 7, SETHWPERFSTATUS 9, FLUSHPDCACHE 10 | UKERNEL.md "3." (type headings) | covered |
| `pvrsgx_ccb.h:26-30` cache control bits | INVAL_BIF_PT 0x1, INVAL_BIF_PD 0x2, INVAL_BIF_SL 0x4, INVAL_DATA 0x8 | UKERNEL.md "3." step 3 (data/PT/PD/SLC by name) | partial: bit values not stated (UAPI.md G19) |
| `pvrsgx_ccb.h:32-34`, `pvrsgx_ccb.c:22-37` | 256 slots x 8 words (service address, cache control, 6 data); control word 0 = write offset, word 1 = read offset, modulo 256 | UKERNEL.md "3."; STATE.md "6." (CCB control: write, read) | covered |
| `pvrsgx_ccb.c:53-96` `pvrsgx_ccb_send` | full when wo+1 == ro; order: command, wmb, write offset, kicker word += 1 (mod 256), wmb, EVENT_KICK = 1 | UKERNEL.md "2." (doorbell, WO != RO detection); SRVINIT.md "Allocations" (event kicker buffer) | partial: the event kicker's per-command increment (& 0xff) and the doorbell offset/value not stated |
| `pvrsgx_ccb.c:40-51` `pvrsgx_ccb_consumed` | a command is done once the read offset has passed it | DRM-DRIVER.md "5." | covered |

##### Address spaces (`pvrsgx_mmu.c`, `pvrsgx_vm.c`, `pvrsgx_kobj.c`, `pvrsgx_gem.c`)

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `pvrsgx_mmu.c` (whole) | entry formats (see MMU row above); tables write-combined; `pvrsgx_mmu_share` copies PDEs so page tables are shared | DRM-DRIVER.md "4." | partial (entry formats, G3) |
| `pvrsgx_vm.c:56-91` `heaps` | client heaps: general 0x3e000000-0xf0000000, PDS pixel 0x2e000000-0x30000000, PDS vertex 0x30400000-0x32000000, USSE pixel 0x37000000+0x500000 (base index 0), USSE vertex 0x3c000000+0x200000 (index 10), texture 0xf0000000-2^32, 4 KiB pages, 512 KiB windows | UAPI.md "5. Heap layout" | covered |
| `pvrsgx_vm.c:43-53, 209-213` `share_kernel_heaps` | every VM maps the kernel heaps unless the image declares feature bit 1 (secure) | DRM-DRIVER.md "4." (the DDK microkernel moves the EDM to list 1 with BIF_BANK0 0x11); UKERNEL.md "4a" (bit 1) | partial: DRM-DRIVER.md 4 says the default shares "only when the DDK microkernel is loaded"; the code also shares for an open microkernel without bit 1 |
| `pvrsgx_vm.c:365-461` `VM_MAP`; `:463-502` `VM_UNMAP` | page-aligned, inside one heap, below 4 GiB, import pages not bounced; PT+PD invalidate requested on the next command after a map and after an unmap; pages kept until earlier jobs finish | UAPI.md "VM contexts, VM_MAP, VM_UNMAP", "CREATE_BO"; DRM-DRIVER.md "4." (unmap) | partial: the invalidate after VM_MAP (the BIF may have cached a walk of the empty range) and the PT+PD bit pair are not stated |
| `pvrsgx_kobj.c:34-46, 57-108` ranges | kernel data from 0x32010000, kernel code from 0x30010000 (past the firmware arenas), sync, per-VM 3D params, TA data; PTE flags: kdata CACHECONSISTENT+EDMPROTECT, kcode and sync CACHECONSISTENT, 3D params / TA data 0, VM copies 0; PT+PD invalidate after each create | UAPI.md "5.", "7.3" (which structure in which range); DRM-DRIVER.md "4." | partial: PTE flag choice per range has no doc (G3); the invalidate-after-create not stated |
| `pvrsgx_kobj.c:4-16, 125-152` graveyard | released structures kept until restart: the microkernel reads the previous render's structures | GEN.md "Layout: the microkernel reads the previous client's render"; UAPI.md "7.5" | covered |
| `pvrsgx_gem.c` | shmem with DMA32 gfp, write-combined, CPU access flag, code objects not exportable, max 1 GiB | DRM-DRIVER.md "4.", "6." rule 1; UAPI.md "CREATE_BO" | covered (the 1 GiB cap is a driver choice) |

##### Render path (`pvrsgx_render.c`, `pvrsgx_render.h`)

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `pvrsgx_render.c:39-47` constants | PB default 0x400 + 0x22 pages, free list 0x40000, context CCB 0x10000 ring / 0x10420 alloc, TA command 0x2c0, shared at +0x58, tail at +0x238, RT max 2048 | STATE.md "6." (HW render context, PB descriptor, Free list), "1." (offsets); UAPI.md "CREATE_CONTEXT" | partial: RT max 2048 has no source (G14) |
| `pvrsgx_render.c:70-101` `pb_init` | free list `prev<<16 \| next` from heap-base page index, 0xffff ends; block list {pages, first \| last<<16, desc}; descriptor +0x04 pages, +0x08 head, +0x0c (last-1)<<16, +0x10 free list, +0x18/+0x1c copies, +0x28 TA = pages+0x10, +0x2c ZLS = pages-0x22, +0x30 global, +0x34 PDS = min-0x100, +0x38 block list | STATE.md "PB descriptor", "Free list"; GEN.md "Smaller things" (block list word 2) | covered |
| `pvrsgx_render.c:122-152, 175-192, 653-659` `pb_create`, `vm_pb_get`, `pb_rearm` | one PB per VM at the 3D-params heap base, base = PB & 0xfff00000, free list in 3D params; descriptor and block list in kdata; rebuilt on the first job after a restart | DRM-DRIVER.md "4." (per-VM PB), "5." (PB re-set after reset); UAPI.md "5." (request base, free-list index) | covered |
| `pvrsgx_render.c:194-234` `pvrsgx_render_ctx_init` | HW render context 0x44 bytes in kernel code: w0 0x21, w2 PD, w3 CCB, w4 CCB control, w6 PB descriptor, w7 0x40-byte scratch in 3D params, w16 pid; 0x18-byte 3D sync in the sync range | STATE.md "HW render context"; UAPI.md "CREATE_CONTEXT", "7.3" | covered (the 3D sync's use is in the CMDTA_SHARED row) |
| `pvrsgx_render.c:256-284` `rt_numbers` | xt, yt; mode by w*h < 0x4b000; mx, my; MTILE1/2; TE screen; tiles per macrotile; padded tiles = mx*2 or mx*4; region bytes = padx*pady*12; tail pointers = (next power of two >= max(padx, pady))^2 * 4 | STATE.md "Render target", "RT data"; UAPI.md "CREATE_RT_DATASET" | partial: tail-pointer size differs: UAPI.md squares then rounds to a power of two; the code rounds the side up first (equal at 720x480, larger for e.g. 40 padded tiles); mode 0 padded = mx*2 only implied |
| `pvrsgx_render.c:322-344, 398-444` RT data | 2*0x38+0x18 bytes in kdata; header [0] -> 4-byte frame word, [2] padx, [5] 2; entry [0] HW ctx, [2] mt>>4, [3] mt*0xc, [4] bg block, [5] own 0x88 block, [6] shared 0x40 block, [7] shared 0x50 block, [8] tail pointers, [9] size, [10] 1, [12] 4-byte kdata word, [13] 0x10; TA-data blocks, region arrays, tail pointers in the TA data range mapped in the VM | STATE.md "RT data", "Region headers and tail pointers"; UAPI.md "7.3" | partial: STATE.md header [1] = 1 is not written by the code; that each entry has its own 0x88 block while the 0x40/0x50 blocks are shared is not stated; entry [1] (render in flight) and [11] left 0 is not stated |
| `pvrsgx_render.c:357-369` background block | [5] and [8] 1.0, [10] 0x202, [15] 0x100000, [16] 0x1000, [17] 0x3f80, [18], [19] 1.0, [20], [21] 0x1000, [22] 0x3f80 | STATE.md "Background object block"; TEX.md "10." (words 3-8 = texture coordinates (0,0),(1,0),(0,1)) | covered |
| `pvrsgx_render.c:371-392` render details, static words | +0x00 PB base, +0x08 0x80, +0x0c 0x000f0000, +0x10 0x80000000, +0x38/+0x3c b31, +0x40 1.0, +0x44 0x300, +0x48 bg block word, +0x50 0x88, +0x58 0x30, +0x64 1, +0x68 1, +0xcc 0x100, +0xd0 -> 8-byte sync word (=1), +0xd8 -> RT data, +0x104 = 1, +0x110 -> 8-byte kdata, +0x114 -> 4-byte kdata, +0x13c 0x07c00000; HW sync list [0] -> access word (=1) | STATE.md "Render details"; UAPI.md "4.2"; GEN.md "What a frame is made of" (+0x104 named as [G] constant) | partial: +0xd0, +0xd8, +0x104 value, +0x110, +0x114 and the objects they point at are not stated |
| `pvrsgx_render.c:690-720` render details, per job | +0x04 ZLS base, +0x14/+0x18 bias, +0x20/+0x28 region arrays, +0x2c zls_control \| (xt-1)<<3, +0x30/+0x34 depth offset, +0x38/+0x3c b31 \| stencil offset, +0x40, +0x5c/+0x60, +0xdc -> RT-data entry; word at +0x114 = background PDS primary word; frame word = f+1; sync and access words = 1 | STATE.md "Render details"; UAPI.md "3.4", "4.2" | partial: +0xdc, the +0x114 word's contents and the per-scene resets of the frame/sync/access words not stated |
| `pvrsgx_render.c:681-688, 641-643` frame alternation | frame f: odd frames use entry 0 / region array 0, even frames entry 1 / array 1; single_entry: always entry 1, f = 0 | GEN.md "Running it" (`-R`, SGXGEN_ALTERNATE: frame 2 values) | partial: GEN.md gives the frame-2 example (and says that mode ran only on fakepvr); the general rule is not stated in STATE.md or UAPI.md |
| `pvrsgx_render.c:722-743` TA command header | +0x00 0x2c0; +0x04 TA flags: one kick 0x63, two kicks 0x61 / 0x62 (0x60 middle), +0x10 on the dataset's first render; +0x08/+0x10 PDS words, +0x0c 0x00030001, +0x14/+0x18 0x50000000; +0x1c last kick only: flags_3d \| b10 \| f<<14; +0x20 = f; +0x24 PB desc, +0x38 render details, +0x3c HW sync list, +0x40 RT data, +0x44 entry | STATE.md "1. Header"; UAPI.md "4.1", "3.4"; GEN.md "Running it" (0x61, cmd+0x20 = 1, 3D flags 0x4400 on frame 2); DRIVER-UPSTREAM.md "4." (0x63/0x10 named) | partial: 0x63 and the bit meanings of TA flags (b4 = first render) not stated (G12); 0x60 middle-kick has no source; cmd+0x1c f<<14 and cmd+0x20 = f contradict UAPI.md 4.1 (cmd+0x20 K-const 0) and STATE.md (kick +0x00, [G]); 3D flags 0 on non-last kicks only shown by STATE.md's ref values |
| `pvrsgx_render.c:745-766` CMDTA_SHARED | +0x58 READY = 1 every kick; last kick: +0x74 3D sync write-pending, +0x78 write-complete address, +0x7c read-pending (post-incremented), +0x80 read-complete address | UAPI.md "4.1" (ready flag, sync updates, no offsets); STATE.md "CMDTA_SHARED" (kernel-filled) | missing: every offset and value in CMDTA_SHARED |
| `pvrsgx_render.c:754-765` HW device sync list (cmd+0x3c, 0x28 bytes) | [0] access-word address, [1] count = 1, [2] read pending, [3] read-complete address, [4] write pending (post-incremented), [5] write-complete address; done when write-complete == pending+1 | TEX.md "5." (access address, count, per-sync addresses and pending values); UKERNEL.md "5." (write-ops-complete = pending+1); STATE.md "1." (cmd+0x3c dependency block, 0x28, [G]) | partial: exact word order and size, and that cmd+0x3c is the HW device sync list, not stated (G15) |
| `pvrsgx_render.c:768-783` TA command tail | TE AA 0x20000000, MTILE1/2, TE screen, tiles/mt, ta_flags2 \| b22 on the last kick, VDM base, 0xa2, region array, tail pointers, 2 x 0x358637bd, `(h-1)<<16 \| (w-1)`, +0x274 PB base, +0x2a4 0x88 | STATE.md "1. Tail"; UAPI.md "4.1", "3.3" | partial: the kernel's b22 on the last kick ("this kick renders") not stated; STATE.md derives b22-23 from kick flag 0xa00 |
| `pvrsgx_render.c:785-797` context ring | command written at the write offset, offset += 0x2c0 mod 0x10000, slack past the ring end, write offset = control word 0; kick data[1] = HW render context | STATE.md "HW render context" (0x10000 + 0x420); UKERNEL.md "TA kick (type 0)" | partial: how the microkernel finds the command (at the control's read offset; STATE.md 1 places it 0x58 before `ui32CCBOffset`) and the wrap rule (commands written contiguously into the slack) not stated |
| `pvrsgx_render.c:559-612, 620-623` stream decode, `pds_word` | tile limits xt-1 / yt-1, pad = xt*32 x yt*16, PDS words `rows<<26 \| (addr - 0x2e000000)>>4`, max 2 kicks | UAPI.md "3.1"-"3.4", "5." | covered (2-kick limit: G12) |
| `pvrsgx_render.c:817-853` transfer context | 0xb4 bytes in kernel code: w0 1, w2 PD, w3 CCB, w4 control, w0x2a pid; ring 0x10000 + 0x4c8; control 8 bytes | TEX.md "10."; UAPI.md "CREATE_CONTEXT", "7.3" | covered |
| `pvrsgx_render.c:902-949` transfer command | words 0-15, +0xa4 0x1c0, +0xa8 8/0, +0x194 0x07c00000; shared at +0xac: +0x130 NumDstSyncs 1, +0x134 read pending, +0x138 read-complete address, +0x13c write pending, +0x140 write-complete address; done = pending+1; data[1] = HW transfer context | UAPI.md "3.5"; TEX.md "10."; UKERNEL.md "10." (dst sync offsets); DRM-DRIVER.md "5." | covered |
| `pvrsgx_render.c:821-822` transfer tile limits | first/last tile up to 127 x 255 | UAPI.md "3.2" (limits = largest RT), G14 | missing: the 127/255 values (4096 px) have no source and differ from UAPI.md 3.2's rule |
| `pvrsgx_render.c:154-166, 236-254, 286-313, 855-869` cleanups | PB cleanup at PB release (after the contexts'), RC at context destroy, RT at dataset destroy, TC at transfer-context destroy | UKERNEL.md "What a kernel driver needs from it", "Cleanup v2" | partial: TC type value (3) not stated (see host-control-bits row) |
| `pvrsgx_render.c:951-1072` dumps, `pvrsgx_dump.c`, `pvrsgx_debugfs.c` | text dumps of the structures above; register list read when powered (incl. EDM task regs 0xb14/0xb20) | DRM-DRIVER.md "5." (devcoredump contents); SRVINIT.md (0xb14/0xb20) | partial: that the listed registers are safe to read while clocked is not stated (offsets as in the register rows) |

##### Jobs, validator, tools, build

| file (lines) / area | what it encodes | doc section(s) | status |
|---|---|---|---|
| `pvrsgx_job.c:85-247` completion, `run_job` | one job at a time; NULL job = PROCESS_QUEUES done when consumed; render/transfer done when write-complete reaches pending+1; poll every jiffy; imports flushed before and after | DRM-DRIVER.md "5."; UAPI.md "CREATE_BO", "SUBMIT_JOBS" | partial: DRM-DRIVER.md 5 still says render jobs "will complete on the 3D's end-of-render instead"; the code's render completion on the HW sync list's write-complete word is documented only for transfers (DRM-DRIVER.md 5, UKERNEL.md 10) and in general by UKERNEL.md 5 |
| `pvrsgx_job.c:250-290` timeout | 2 s; guilty/innocent, -ETIME/-ECANCELED; restart without power-off; lost -> -EIO | DRM-DRIVER.md "5."; UAPI.md "8."; UKERNEL.md "5a" | covered |
| `pvrsgx_job.c:352-408` CREATE_CONTEXT | PB request 0 or 0x100..0x8000 pages | UAPI.md "CREATE_CONTEXT" | partial: bounds 0x100 / 0x8000 not stated |
| `pvrsgx_validate.c`, `pvrsgx_usse_table.h` | rules: unknown form, ldr, str, setl, savl refused; br target (signed l0-17) inside the object; code objects only in USSE heaps; READONLY PTE; append-only while mapped | DRM-DRIVER.md "6."; UAPI.md "UPLOAD_CODE", "D12"; ISA.md "hw 1f: the special class" (br field) | covered (PTE bit value: MMU row) |
| `drm/tools/gen_usse_table.py` | one (mask, match) per isaspec leaf, overlap refused | DRM-DRIVER.md "6." rule 3; UAPI.md "UPLOAD_CODE"; encodings: ISA.md | covered |
| `drm/tools/mkukfw.py` | builds/checks the 4a file: header struct `<2I10I4I32s` (0x60), header padded to 8, zlib CRC-32, patches_off 0 when none, default entry 0:0, the driver's checks incl. MAX_IMAGE 0xddf0, 64-pair table limit, word-1 placeholder mask 0x3f1f0 | UKERNEL.md "4a" | partial: same unstated limits as the `ukfw_parse` row; patches_off = 0 with no patches not stated |
| `drm/tools/gen_usse_corpus.py` | test corpus extraction (drops setl/savl) | n/a | n/a |
| `Makefile`, `Kconfig`, `MAINTAINERS.fragment`, `ci/*`, `Documentation/gpu/powervr_sgx/*` | build, CI kernel, RAM boot (`run.sh` extracts .rodata 0x2000 + 0xcf68), docs | DRM-DRIVER.md "7. Testing"; SRVINIT.md "The firmware file" | n/a (the 0x2000/0xcf68 extraction is covered) |
| `powervr_sgx_stream*.{c,h}`, `powervr_sgx_cmd_checks.c` | stream framing, per-field checks, cross-field rules (identical to `docs/uapi/stream/`) | UAPI.md "3.1"-"3.6" (generated table) | covered |

##### Tests (one row each)

| file | what it checks |
|---|---|
| `drm/tests/powervr_sgx_test.c` | 39 IGT-style uAPI subtests: dev-query (sizes, heaps, limits, quirks), BO create/mmap, VM create/map/overlap/kernel ranges/handles, contexts, NULL jobs (binary, timeline, chains, 600 jobs, BO refs, destroyed VM), invalid submits and streams (render, transfer), RT datasets, reserved bits, code BOs, PRIME, UPLOAD_CODE corpus/reject/invalid/mapped, cross-file and cross-VM handles |
| `drm/tests/usse_corpus.h` | generated list of valid USSE words (minus setl/savl) that UPLOAD_CODE must accept |
| `drm/tests/powervr_sgx_frame.c` | renders sgxgen frames (1 or 2 kicks, several sizes, contexts, RT datasets) through SUBMIT_JOBS and checks every pixel |
| `drm/tests/powervr_sgx_blit.c` | transfer jobs from sgxtex: copy, upload (twiddle), readback, box-filtered mip; every destination byte checked |
| `drm/tests/powervr_sgx_hang.c` | mixed render+transfer submit, forced TA fault -> timeout, reset, guilty refusal, devcoredump, other contexts still working; runtime-PM resume; `-E` the PBE write into a read-only code object (E1) |
| `drm/tests/powervr_sgx_prime.c` | PRIME both ways with simpledrm: rendered pixels in the buffer and in scanout, explicit and implicit sync, import cache flushing |
| `drm/powervr_sgx/tests/pvrsgx_fw_test.c` | KUnit: 5 cases on the 4a file checks (good file, not ours, CRC, header fields, tables) |

#### Facts the docs need

##### Register map (DRM-DRIVER.md "3."/"4.", or a register table in UKERNEL.md "6.")
- CORE_ID = 0x001c, CORE_REVISION = 0x0020.
- CLKGATECTL = 0x0000, CLKGATECTL2 = 0x0004 (CLKGATESTATUS 0x0008 is in SRVINIT.md); that `CLKGATESTATUS & 0xffffffdf` reads 0 once the clocks are gated (the stop sequence waits for it).
- SOFT_RESET = 0x0080: bit 0 BIF; bits 1-5 (0x3e) DPM, TA, USE, ISP, TSP; "all" = 0x3f; 0 releases; the driver's hold state after stop is 0x3f.
- EVENT_HOST_CLEAR2 = 0x0114, EVENT_STATUS2 = 0x0118, EVENT_STATUS = 0x012c, EVENT_HOST_CLEAR = 0x0134 (an explicit statement, not only the `str 0x134` aside); MASTER_INTERRUPT = bit 31 in STATUS and in HOST_CLEAR; SW_EVENT bit 14 in HOST_CLEAR.
- TIMER = 0x0144.
- EVENT_KICK = 0x0ac8, EVENT_KICK2 = 0x0ae8; the value written is 1 (bit 0); which one the host uses for the first kick versus every CCB command.
- BIF_CTRL = 0x0c00: PAUSE bit 1, INVALDC bit 3 (pulse 1 then 0), CLEAR_FAULT bit 4.
- BIF_INT_STAT = 0x0c04: bits 0-15 faulting requestors, bits 16-18 fault type; BIF_FAULT = 0x0c08: faulting page address in bits 12-31, and what the low bits hold (the examples 0x...021, 0x...002).
- BIF_DIR_LIST_BASE1..15 = 0x0c38 + 4(n-1) (list 15 = 0x0c70), list 0 = 0x0c84 apart; the value format (UKERNEL.md 6 says "shifted"; the driver writes the PD's raw page-aligned physical address, and UAPI.md 7.2 read back 0x01d6b000).
- BIF_BANK_SET = 0x0c74 (what it does; the driver zeroes it); BIF_BANK0 fields HOST bits 8-11, 3D bits 12-15, 2D bits 16-19 (EDM/TA already in UKERNEL.md 6) — UAPI.md G2.
- BIF_MEM_REQ_STAT = 0x0ca8, READS bits 0-10.
- Which registers are safe to read on an uninitialised core (CLKGATECTL, CLKGATESTATUS, SOFT_RESET, EVENT_STATUS, EVENT_HOST_ENABLE, BIF_CTRL, CORE_ID, CORE_REVISION), and that USE_CODE_BASE_0 hangs the bus before the reset sequence; which registers the dump may read while clocked.
- BAR0 size (the driver requires at least 0x4000 bytes).

##### MMU (DRM-DRIVER.md "4.", new subsection; UAPI.md G3 points at UKERNEL.md)
- Two levels: 1024-entry page directory, 1024-entry page tables, 4 KiB pages, 4 MiB per directory entry, 32-bit entries holding a physical address in bits 12-31.
- PDE: valid bit 0; page-size field (0 = 4 KiB) and its position.
- PTE: valid bit 0, WRITEONLY bit 1, READONLY bit 2, CACHECONSISTENT bit 3, EDMPROTECT bit 4; which are enforced (READONLY is, E1) and for which requestors.
- The mapping from SRVINIT.md's services attribute flags (0x4f, 9) to PTE bits as the driver uses them: kernel data CACHECONSISTENT+EDMPROTECT, kernel code READONLY (firmware arena) or CACHECONSISTENT (kernel objects), sync CACHECONSISTENT, client and VM-mapped kernel objects 0.
- That an empty-range walk may be cached, so a PT+PD invalidate is needed after creating a mapping too (the driver requests one after every VM_MAP and kernel-object create).

##### Reset (DRM-DRIVER.md "4.", "Reset" paragraph)
- The sequence in order: SOFT_RESET 0x3f, settle (one SOFT_RESET read + 1 us); BIF_CTRL, BIF_BANK_SET, BIF_BANK0 and all 16 list bases = 0; list 0 = scratch PD; SOFT_RESET 0x3e (BIF out, cores in); INVALDC pulse; fault drain (below); BANK0 EDM = 15; list 15 = firmware PD; list 0 = idle PD; INVALDC; SOFT_RESET 0.
- Fault drain: while BIF_INT_STAT bits 0-15 are nonzero (bound 32): read BIF_FAULT, SOFT_RESET 0x3f, map the scratch page at the fault, write STATUS and STATUS2 to HOST_CLEAR and HOST_CLEAR2, barrier, release to 0x3e, INVALDC, unmap.

##### Host control block and firmware commands (UKERNEL.md "1." and "3.")
- Host control word indexes: 0 init status, 1 power status, 2 cleanup status, 3 microkernel lockup count, 4 host lockup count, 5 HWR sample rate, 6 microkernel timer clock, 7 APM sample rate, 8 interrupt flags, 9 interrupt clear flags, 13 host clock (the host writes microseconds at start), 14 assert-fail word, 16 cleanup v2 sequence.
- The lockup report: interrupt flags HWR (b0) set while the clear flags' HWR is clear.
- Power: POWER command data[1] values (POWEROFF = 1; IDLE, RESUME), POWEROFF_COMPLETE = bit 3 of power status, NO_WORK = bit 5.
- Cleanup type TC = 3 (and 2DC's value); for the pre-v2 contract, that the host clears COMPLETE after reading it; in v2, the 5 s bound per poll; that after a cleanup the host requests a data-cache invalidate on the next command.
- ui32CacheControl bit values: BIF PT 0x1, BIF PD 0x2, BIF SLC 0x4, data 0x8 (UAPI.md G19); that the first command after a start carries the PD invalidate (0x2) (and whether that is what SRVINIT.md's `ui32CacheControl` 2 means).
- Event kicker: the host increments the 32-bit kicker word by one (mod 256) for each command before ringing EVENT_KICK, and zeroes it at start (SRVINIT.md says only the latter).
- GETMISCINFO buffer layout: word 0 request/status (request GET_STRUCT_SIZES = 0x2, READY = bit 0), word 1 core revision, 2 core id, 3 DDK version, 4 build, 7 build options, 8-20 the 13 struct sizes (order as SRVINIT.md).
- Core clock of the CE5300 SGX (the driver assumes 400 MHz for the timer-clock word).

##### Firmware file (UKERNEL.md "4a")
- Entry and patch offsets are multiples of 8; table offsets multiples of 4; each table at most 64 pairs; patches_off may be 0 when n_patches is 0.
- Maximum image size the driver accepts: 0xe000 - 0x200 - 0x10 = 0xddf0 bytes (kernel-code arena minus PDS space), and that the five PDS programs follow the image, 16-byte aligned, at +0, 0x20, 0x60, 0x90, 0x190.
- A file without a program-0 entry is refused; a `core_rev` mismatch is only a warning.
- Placeholder check for the open image: word 0 low 21 bits = 0xdbeef and word-1 bits 4-8 and 12-17 = 0.
- Programs 1-4 that the table doesn't list are built as DOUTU (word 1 = 2) to program 0's offset.
- Resolve the conflict with SRVINIT.md "Loading openHC's own microkernel" (event program = DOUTD of the TA3D block + DOUTU) versus 4a (bare DOUTU), which the driver builds.

##### TA command and render structures (STATE.md "1." and "6."; UAPI.md "4.1", "4.2")
- TA flags (cmd+0x04): 0x63 for a one-kick scene; 0x61 first / 0x62 last for two kicks; 0x60 middle (no source; G12); bit 4 (0x10) on the RT dataset's first render; the bit meanings.
- cmd+0x1c: written only on the last kick; bits 14+ = the dataset's frame index (UAPI.md 4.1 lists only b10).
- cmd+0x20 = the dataset's frame index (UAPI.md 4.1 says K-const 0; STATE.md says kick +0x00 [G]; GEN.md shows 1 on frame 2).
- cmd+0x24c: the kernel ORs bit 22 on the kick that renders (last kick).
- CMDTA_SHARED offsets (from cmd+0x58): +0x00 ctrl flags, READY = 1, set on every kick; +0x1c 3D-sync write-ops pending, +0x20 its write-complete address, +0x24 read-ops pending (post-increment), +0x28 read-complete address, on the last kick.
- cmd+0x3c is the HW device sync list, 0x28 bytes: [0] access-word address (the word holds 1), [1] count, then per sync [2] read pending, [3] read-complete address, [4] write pending, [5] write-complete address; the microkernel writes pending+1 to write-complete at 3D end (TEX.md 5 has the members but not the order).
- Render-details words: +0xd0 address of an 8-byte sync word (set to 1 per scene), +0xd8 RT data address, +0xdc RT-data entry address of this render, +0x104 = 1, +0x110 address of an 8-byte kdata word, +0x114 address of a word holding the background PDS primary word (`rows<<26 | offset>>4`).
- RT data: header word 0 points at a frame word set to frame index + 1 per scene; whether header word 1 must be 1 (STATE.md says 1; the driver leaves it 0); entries alternate per frame (odd frames entry 0 / region array 0, even entry 1 / array 1; GEN.md has only the frame-2 example); each entry has its own 0x88 TA-data block while the 0x40 and 0x50 blocks are shared; entry words 1 and 11 are left 0.
- Tail-pointer array size: settle the formula — the driver uses (next power of two >= max(padx, pady))^2 * 4, UAPI.md "CREATE_RT_DATASET" says (padded)^2 * 4 rounded up; mode-0 padded tiles = mx*2.
- The render context's command ring: the command is written at the control's write offset and read by the microkernel at its read offset; commands are written contiguously past the 0x10000 ring end into the 0x420 (render) / 0x4c8 (transfer) slack.

##### Limits (UAPI.md "DEV_QUERY", "CREATE_CONTEXT", G14)
- PB request bounds 0x100 and 0x8000 pages (and their reason).
- RT maximum 2048 x 2048 (placeholder) and the transfer tile limits 127 x 255 (4096 px), which differ from UAPI.md 3.2's "largest render target" rule.
- `max_ta_kicks` = 64 reported, while the driver accepts at most 2 kicks per scene (middle-kick flags, G12).

##### Other stale or conflicting text
- DRM-DRIVER.md "4.": `share_kernel_heaps` -1 shares unless the image declares feature bit 1, not "only when the DDK microkernel is loaded".
- DRM-DRIVER.md "5.": render jobs already complete on the HW sync list's write-complete word, not "will complete on the 3D's end-of-render".
- DRM-DRIVER.md "1. Status" mentions "the TA-flag 0x10 change"; STATE.md/UAPI.md don't define bit 0x10 (see TA flags above).
