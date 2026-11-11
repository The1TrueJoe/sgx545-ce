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

## How each fact is established

Every fact carries one of these tags:

- **hardware**: confirmed on a CE5300 board. Most come from a blob-free
  replay (`tools/decode/mkreplay.py --poke`) that changed one field, with the
  pixels changing as predicted; the runs are listed under "Hardware proof"
  at the end. Others name the run in the text (a lockup, a dEQP run).
  **hardware (open driver)** means the open Mesa driver in this repo
  (`mesa/src/gallium/drivers/sgx`, its winsys in
  `mesa/src/gallium/winsys/sgx`, words packed from
  `mesa/src/sgx/csbgen/*.xml`) programs the field from GL state and renders
  correctly on the board. That backs the values the driver writes, not the
  values it never uses.
- **capture**: seen in the vendor driver's output: the command streams and
  state words of a capture, a diff of two one-change probe captures, or the
  vendor microkernel's memory trace over a frame.
- **unconfirmed**: stated, but no hardware run or capture backs it yet.
  Each one is listed in "Gaps" at the end with the test that would confirm
  it. Shipping code must not rely on one.
- **guess**: a name or meaning that fits but has no other backing.

A table's tag column uses the same words. Where a fact has parts with
different backing, each tag names its part.

## Where the knowledge comes from

- **Captures.** `tools/probe/probe.c` (ES2) and `tools/probe/probe1.c` (ES1)
  have a knob for each piece of state (see their headers). The DDK ran them
  on a CE5300 board under pvrtrace. The one-change captures of 2026-10-28
  are listed under "Captures"; later sets (2026-11-02, 2026-11-08) are named
  where they are used.
- **Microkernel traces.** The vendor microkernel's memory reads and writes
  over a generated frame (`tools/ukernel/run.py`).
- **Replays.** A capture replayed with no DDK loaded, one word poked per
  run ("Hardware proof").
- **The open driver**, and frames built from scratch by `tools/gen`
  (`docs/GEN.md`), on the same board.

The GPL kernel's `sgx545defs.h` names only the BIF, USE code-base, PDS
exec/invalidate and event registers. It has no ISP, TSP, TE, MTE or PBE
registers, so the other names used here are descriptive. A name with no
other backing is tagged guess.

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
`ui32CCBOffset`. The client part is 0x2a8 bytes, followed by the deferred
updates.

### Header, cmd+0x00 to +0x58

| off | ref.bin | field | tag |
|---|---|---|---|
| 0x00 | 0x2c0 | command size, rounded up to 64 bytes (`0x2b0 + 8·n` deferred updates) | capture; hardware (open driver) |
| 0x04 | 0x71 / 0x62 | TA flags; see "TA flags" below | capture |
| 0x08 | 04001800 | background object, PDS secondary word | capture; hardware (open driver) |
| 0x0c | 00030001 | background object, PDS middle word | capture; hardware (open driver) |
| 0x10 | 0c00060e | background object, PDS primary (0x2e0060e0: samples the render target itself, i.e. "load the old contents") | capture; hardware (open driver) |
| 0x14 | 0x50000000 | `0x40000000 + 0x10000000` in every capture. The DDK can put `n<<27` there instead, under a render-context setting no capture has | capture (0x50000000); unconfirmed (`n<<27`) |
| 0x18 | 0x50000000 | constant | capture |
| 0x1c | 0 / 0x400 / 0x580 | 3D flags. b0, b1, b3, b4 and b12 follow client kick flags. b7 \| b8 are needed for a ZLS store or load to happen (`docs/TEX.md` section 7). b10 is set on the kick that renders. b14 and up = the RT dataset's frame index on the kick that renders (0x4400 on frame 2, GEN.md). The microkernel copies the word to render details +0xd4 at every kick and acts on it there (section 6, "Render details") | capture; hardware (open driver: b7, b8, b10, b14 and up); unconfirmed (b0, b1, b3, b4, b12) |
| 0x20 | 0 | the RT dataset's frame index (1 on frame 2, GEN.md). The microkernel copies it to TA3D +0xa4 and render details +0x140 at every kick | capture; hardware (open driver) |
| 0x24 | 3200e000 | **PB descriptor** (kdata) | capture; hardware (open driver) |
| 0x38 | 3200e560 | **render details** (kdata, 0x1ec) | capture; hardware (open driver) |
| 0x3c | 3200e7a0 | **HW device sync list** (kdata, 0x28; `SGXMKIF_HWDEVICE_SYNC_LIST`), below. The microkernel copies it to render details +0xe0 at every kick | capture; hardware (open driver) |
| 0x40 | 3200aac0 | **RT data** (kdata) | capture; hardware (open driver) |
| 0x44 | 3200ab10 | RT data entry for this render (`+0x18 + idx·0x38`) | capture; hardware (open driver) |
| 0x48 | 0 | a client value, under a client kick flag | guess |
| 0x4c, 0x50 | 0 | multi-RT values | guess |
| 0x54 | 0 / 6 | number of deferred render-details updates at +0x2a8 | capture |

### HW device sync list (cmd+0x3c, kdata, 0x28 bytes)

`SGXMKIF_HWDEVICE_SYNC_LIST` (`sgx_mkif_km.h`), filled by the kernel at kick
time; 0x28 bytes hold two objects.

| word | field |
|---|---|
| 0 | `sAccessDevAddr`: address of a sync-heap word that holds 1 while the render is pending |
| 1 | `ui32NumSyncObjects` |
| 2 + 4n | object n: `ui32ReadOpsPendingVal` |
| 3 + 4n | `sReadOpsCompleteDevVAddr` |
| 4 + 4n | `ui32WriteOpsPendingVal` |
| 5 + 4n | `sWriteOpsCompleteDevVAddr` |

At the end of the render the microkernel writes `ui32WriteOpsPendingVal + 1`
to object n's `sWriteOpsCompleteDevVAddr`, n being render details +0x108,
and zeroes the access word [capture: microkernel trace]. A render is done
for the host when the write-complete word reaches the pending value plus
one. The host compares the two as wrapping 32-bit counters.

### CMDTA_SHARED, cmd+0x58 to +0x238

The kernel fills it at kick time; the client leaves it zero. The layout is
`SGXMKIF_CMDTA_SHARED` (`sgx_mkif_km.h`), offsets from cmd+0x58:

| off | field | what the microkernel does with it |
|---|---|---|
| 0x00 | `ui32CtrlFlags`: READY = 1, set on every kick | read at the kick |
| 0x04, 0x08 | `ui32NumTAStatusVals`, `ui32Num3DStatusVals` | the counts of the status lists at the end of the struct |
| 0x0c-0x18 | the TA TQ sync: write-ops pending, write-complete address, read-ops pending, read-complete address | read at TA start |
| 0x1c-0x28 | the 3D TQ sync, in the same order | copied to render details +0xe4..+0xf0 at every kick. At the 3D end the read-complete address gets read-ops pending + 1. The write-complete address gets write-ops pending + 1 only when render details +0xd4 b9 (0x200) is set |
| then | the source sync list, `sTA3DDependency`, `sCtlTAStatusInfo[]`, `sCtl3DStatusInfo[]` | the TA-finished handler writes each TA status value to its address. The 3D end writes the 3D list from its render-details copy (count at +0x144, pairs from +0x148) |

All [capture]: the layout is the GPL kernel header's, and what the
microkernel does is from its memory trace.

### TA flags (cmd+0x04)

The values the clients use:
- 0x63 for a scene in one kick, 0x73 on a render target's first frame
  [capture];
- 0x71 and 0x62 for the first and last of two kicks [capture];
- 0x61 for the first kick after frame 1 (GEN.md) [capture];
- 0x60 for a middle kick [unconfirmed: no capture].

Bits 5 and 6 are set on every kick [capture]. What the microkernel does
with each bit:

| bit | what the microkernel does | tag |
|---|---|---|
| 0 (0x1) | first kick of the scene: pulses SOFT_RESET's TA bit (0x80 = 4, then 0) before the PB check. Also tested together with b2 (mask 0x5) at TA finish, at scene setup and in one other place | capture (set on every first kick); unconfirmed (the reset) |
| 1 (0x2) | last kick of the scene: the TA-finished handler starts the 3D | capture (set on the kick that renders); hardware (the open microkernel's `do_3d` gates on it, 2026-11-03) |
| 2 (0x4) | only ever tested together with b0 (mask 0x5); no client sets it | guess (resume) |
| 3 (0x8) | tested together with b0 (mask 0x9) before the stored-scene DPM load; no client sets it | guess |
| 4 (0x10) | writes 0xa08 = 1, once per kick: the USE code-cache invalidate | unconfirmed |
| 5 (0x20) | invalidates the TA-side caches: 0xad0 = 1 and 0xae0 = 1, then waits for 0x138 & 0x27 and acks 0x140 = 0x27 | unconfirmed |
| 6 (0x40) | sets a bit in 0x804, then runs the MADD (texture) cache invalidate: waits for EVENT_STATUS b26 (`MADD_CACHE_INVALCOMPLETE`), acks it, then a USE data-cache flush (`op0xff #3`). The 0x04000000 edge shows in the DDK's register sample at every render kick | capture (the register sample); unconfirmed (the rest) |
| 10 (0x400) | compares a sync value named by the command (+0x10c) before going on | guess |
| 12 (0x1000), 13 (0x2000) | tested; no client sets them | guess |

One kick with both b0 and b1 set (0x63) renders: the services backend and
powervr_sgx send every frame that way, and m2's smoke tests pass with it
(10/10 frames on a CE5300 board, 2026-11-04) [hardware; not a replay poke].

### Tail, cmd+0x238 to +0x2a8

Most of these come from the render target; see section 6 for the
formulas.

| off | ref.bin | field | tag |
|---|---|---|---|
| 0x238 | 20000000 | TE AA: `0x20000000`, b31 = x2 multisample in X, b30 = in Y | capture (0x20000000); unconfirmed (b30, b31) |
| 0x23c | 82010018 | TE MTILE1: b31 = 4×4 macrotile mode, macrotile X boundaries (in 32-px tiles) at b22 / b12 / b0 = 8, 16, 24 | capture; hardware (open driver) |
| 0x240 | 02010018 | TE MTILE2: macrotile Y boundaries (in 16-px tiles) = 8, 16, 24 | capture; hardware (open driver) |
| 0x244 | 0001d016 | TE screen: last tile X (b0-11) = 22, last tile Y (b12-) = 29 | capture; hardware (open driver) |
| 0x248 | 0x40 | tiles per macrotile (8 × 8) | capture; hardware (open driver) |
| 0x24c | 0 / 0x2400000 | TA flags 2: b22-23 and b25 follow client kick flags, and b19. The microkernel loads it into the TA register 0x218 at TA start (with +0x238..+0x248 into 0x204-0x214). b22 (0x400000) is set on the kick that renders: the DDK's kick 2 carries 0x400000, kick 1 0 | capture (b22, the register loads); hardware (open driver: b22); unconfirmed (b19, b23, b25) |
| 0x250 | 3e518000 | **VDM control stream base** for this kick | hardware |
| 0x254 | 0xa2 | a client value, 0xa2 in every capture | guess |
| 0x258 | 20006000 | region header array of this render (TE PSG region base) | capture; hardware (open driver) |
| 0x25c | 20000000 | tail pointer array (TE TPC base) | capture; hardware (open driver) |
| 0x260, 0x264 | 358637bd | two 1e-6 floats, constants | capture |
| 0x268 | 01df02cf | screen size `(h-1)<<16 \| (w-1)` | capture; hardware (open driver) |
| 0x26c, 0x270 | 0 | client values | guess |
| 0x274 | 10000000 | 3D params heap base (PB base & 0xfff00000) | capture; hardware (open driver) |
| 0x278 | 0 | a client value << 24 | guess |
| 0x27c-0x298 | 0 | client values (multisample / output values) | guess |
| 0x29c | 0 | number of RT data entries - 1 | capture |
| 0x2a0 | 0 | a render-target value << 12 | capture (0); unconfirmed (the formula) |
| 0x2a4 | 0x88 | a render-target value: 0x88 with no AA | capture (0x88); unconfirmed (other values) |
| 0x2a8.. | | deferred render-details updates: (device address, value) pairs, `count` at +0x54 | capture |

The deferred updates exist because the render details are written in full
only when the client starts a new render. Later kicks into the same render
send only the words that changed. In the probe frame with a depth buffer,
kick 2 sends six [capture]:

- ZLS control;
- the ZLS request base;
- the depth and stencil store and load addresses.

## 2. The VDM control stream

The stream is 32-bit words in the general heap. Each kick's TA command
points at the part that kick runs.

| words | meaning | tag |
|---|---|---|
| `4 \| addr>>4`, info | **state program** (PDS vertex heap): info b26-31 = data rows of 16 B; low bits `0x2022400 \| buffer index (0-3, rotating) + ((attribute dwords+3)>>2)` for a TA state block, `0xe100 \| ((n+3)>>2)` for a plain state/secondary DMA | capture; hardware (open driver) (rows); guess (low bits) |
| `6 \| addr>>4`, info | state program, second form (the TA terminate: 4 × DOUTA + DOUTU) | capture |
| `8…`, idx, w2, w3, `n \| addr>>4`, info | **draw**: see below | capture; hardware (open driver) |
| `c0000000` | terminate | capture; hardware (open driver) |

The draw is six words:

| word | ref.bin | meaning |
|---|---|---|
| 0 | 80c00003 | `0x80000000 \| 0x00c00000 \| prim<<26 \| index count`. prim: triangles 0, lines 1, points 2, strip 3, fan 4 (the bits are `0x4000000·k`) [capture; hardware, open driver] |
| 1 | 3e517004 | index buffer address; bit 0 is cleared for 16-bit indices [capture; hardware, open driver], bits 0-1 for 32-bit [unconfirmed] |
| 2 | f0000000 | `(batch-1)<<28` [capture], \| 0x1000000 for 32-bit indices [unconfirmed], \| index offset [unconfirmed]. The batch is 16 in every DDK draw: triangles, lines, strips and fans, 3 to 192 indices, 1 to 8 draws a frame (2026-11-08 captures). What it batches is [guess] |
| 3 | 003fffff | max index (constant) [capture] |
| 4 | f3040d82 | vertex PDS program `addr>>4`, top nibble = (vertices per USSE task - 1). **It is not a type nibble**: the VDM knows from the draw header that this word follows [capture]. For compiled shaders the DDK's count is `min(16, 128 / round4(output dwords))`, and input dwords don't count: 4 or 8 output dwords give 16, 12 → 10, 16 → 8, 20 → 6, 24 → 5 [capture: 4-24 output dwords, log `2026-10-28-1530-gl4es-ctest`; the e1tex capture's 12-output shader runs 10]. `64 / output dwords` hung the GPU at 12 output dwords (2026-10-28) [hardware; not a replay poke]. The DDK's built-in clear programs don't follow the rule: they run 8 per task at 8 and at 4 output dwords (`0x3c008080` and `0x3c008100` in the base4 and vp5 captures), while a 4-output compiled shader in the same captures runs 16 [capture]. Why the clear uses 8 is [guess] |
| 5 | 01c00082 | b0-6 = PDS data rows, b7-21 = ceil(input attribute bytes/16), b22-23 = min(n,3), b24-31 = ceil(output dwords/4) [capture; hardware, open driver]. b22-23 is 3 in every DDK draw, over the same range as word 2; what n is stays [guess] |

`glDrawArrays` draws through a static 16-bit identity index buffer.

### The state programs and what they DMA

| program | DMA | USSE task | what |
|---|---|---|---|
| 0x3040e000 (`43040e00 0e022403`) | 12 dw from 0x30401000, secondary attributes, kind 3 | 0x3c008700 | the clear's TA state block |
| 0x3040e080 (`43040e08 0e022405`) | 25 dw from 0x30401080, kind 0 | 0x3c008a00 | the triangle's TA state block |
| 0x30401120 (`43040112 0800e103`) | 10 dw from 0x304010e0, kind 1 | 0x3c000080 | vertex secondary attributes: sa0 = the block's own address, sa1 = 0, sa2-sa5 = the driver constants 2/255, 1/255, 2/65535, 1/65535 (`3c008081 3b808081 38000080 37800080`), sa6-sa9 = 0, then the vertex shader's uniforms [capture: every DDK capture on disk, 24 of them, has this 10-dword block; none has vertex uniforms, so where they start is a guess] |
| 0x30400060 (`43040006 0400e100`) | none | 0x3c000080 | empty secondary update (the end of the stream) |
| 0x30400000 (`63040000 0e022401`) | DOUTA (0x2000,0), 3 × (0x16000e, 0x100) | 0x3c000000 | TA terminate |

- The static state program at 0x3040e000 and the 399 copies after it is
  `tst; bra; doutd ds0[0..1]; tst; bra; doutd ds0[2..3]; doutu ds0[4..5]`.
  That gives two optional DMAs. The rotating buffer index in the info word
  picks one of four copies.
- **The state-emit USSE programs are just `emit` instructions.** They copy
  the DMA'd attributes to the MTE (macro tiling engine). The block is the
  state; the program only forwards it. The DDK's, from the base4 capture
  (`0x3c008700` for 12 dwords, `0x3c008a00` for 18):

      phas.i #0, #0 [wait=7]                         fa440700 00000000
      nop [nosched]                                  f8000940 00000000
      test.and.ne p0, #0, #0 [skipinv]               48831280 a00c0000
      (p0) ld.a.d pa0, pa0, #0 [skipinv rpt=4]       e9833084 a0000000
      wdf                                            f9200000 00000000
      mov o0, pa0 [skipinv rcnt rpt=n]               rpt at most 16 per mov: 18 dwords
                                                     are `rpt=16` then `mov o16, pa16 rpt=2`
      emit.t3 r0, #0, #0                             fb23c000 a0000000
      op0xd6 #0                                      ff200000 00000000
      emit.t1 o0, #0, #0 [end f3_20]                 fb2f4000 a0200000

  [capture]. The triangle's block is DMAed as 25 dwords, but its program
  forwards 18 (pa0-pa17). The clear's DMA and program both cover 12.
- **The TA terminate's USSE task (`0x3c000000`) is the same program for two
  dwords** (`mov o0, pa0 [rcnt rpt=2]`). It forwards the two attributes the
  terminate's DOUTAs wrote: pa0 = 0x2000 (ctrl 0) and pa1 = 0x0016000e
  (ctrl 0x100, written three times). What those two words mean, and whether
  the three repeats are needed, is [guess].
- **The vertex secondary task (`0x3c000080`) is one instruction,**
  `phas.i #0, #0 [end wait=7]` (`fa4c0700 00000000`): the DMA itself loads
  the secondary attributes, and nothing is forwarded.
- **DOUTD control word** (the DMA's second word), over every DOUTD in the
  three captures:
  - b25 is set for a DMA into secondary attributes, clear for a vertex
    stream;
  - with b25 set, b22-23 = (dwords - 1) & 3. This holds for all 14 such DMAs
    (9, 10, 11, 12, 15, 25 and 27 dwords). This is the "kind" in the table above;
  - with b25 clear it is 0;
  - b24 was set once, on a 15-dword TA state block in vp5 (`0x0380000e`);
    its meaning is a guess. [capture]

## 3. The TA state block (ISP/TSP/MTE)

The block is the first word of a state-program DMA. The header word says
which groups follow, in this order:

| header bit | words | group | tag |
|---|---|---|---|
| 0x1 | 1 | ISPA, object type bits 15-18 replaced per draw | capture; hardware (open driver) |
| 0x2 | 1 | ISPB (present when ISPA b9) | capture; hardware (open driver) |
| 0x4 | 1 | ISPC, front stencil (present when ISPA b8) | capture; hardware (open driver) |
| 0x8, 0x10, 0x20 | 3 | second-face ISPA, ISPB, ISPC (two-sided stencil). Each face has its own ISPA reference. In a y-flipped window the first group (0x1, 0x2, 0x4) holds GL's **back** face's words and this one GL's front: the first group is the faces wound CCW in the target's memory (`stencil2`, header `df7f`, 2026-11-02) | capture; hardware (open driver) |
| 0x40 | 3 | PDS: secondary, middle word, primary (from the fragment program) | capture; hardware (open driver) |
| 0x80 | 2 | region clip, sticky until the next one (clears, driver objects, draws without a clear before them): `b31 \| xmin<<16 \| xmax`, `ymin<<16 \| ymax`, in 32×16-px tiles; the target's tiles or the scissor's | hardware |
| 0x100 | 6 | viewport | hardware |
| 0x200 | 1 | always 0 | capture |
| 0x400 | 1 | output select (from the vertex program) | capture; hardware (open driver) |
| 0x800 | 1 | W clamp, `0x3727c5ac` = 1e-5 | capture |
| 0x1000 | 1 | MTE control | capture; hardware (open driver) |
| 0x4000 (+0x8000) | 2 | TSP texcoord sizes, texcoord precision mask (from the program) | capture; hardware (open driver) |

- The clear's header is `d4c3` and the triangle's `df43`.
- The type-4 VDM info word's low bits follow this block's length:
  `(dwords+3)>>2` (12 → 3, 13 → 4, 18 → 5) [capture], ORed with a 2-bit
  counter the DDK keeps [unconfirmed: never seen nonzero, see "Field
  layouts for packing"].
- The DMA reads 25 dwords for the triangle's 18-word block. The extra words
  are whatever follows; the header alone says where the block ends.
- The two-sided block (22 words, header `df7f`) is read with 27 dwords
  (`stencil2`), and the VDM info word gives it 7, not `(22+3)>>2` = 6. The
  DMA length is the part that matters: at 25 or 24 dwords the two-sided
  draws come out blank or with wrong channels, differently from run to run;
  at 27 or 28 they draw, whatever the info word says (dEQP
  `fragment_ops.random`, 2026-11-02) [hardware]. The driver reads
  `max(25, words + 5)`.

### ISPA

| bits | field | tag |
|---|---|---|
| 0-7 | stencil reference (single-sided; front/back in ISPC otherwise) | capture (`s1`..`s6`); hardware (open driver) |
| 8 | ISPC follows (stencil enabled with a stencil buffer) | capture; hardware (open driver) |
| 9 | ISPB follows: always set | capture |
| 11 | two-sided stencil: set in both the front and the back group's ISPA | capture (`stencil2`, 2026-11-02); hardware (open driver) |
| 15-18 | object type, set per draw: 0 triangles, 1 lines, 3 points, 4 point sprites (generates gl_PointCoord), 5 a triangle's edges, 6 a triangle's vertices; 7 locks up the 3D core; see "Lines and points" and "Polygon mode and edge flags" | hardware (1, 4, 5, 6, 7); capture (0, 3); hardware (open driver) |
| 20 | depth write disable | capture (`dmask0`); hardware (open driver) |
| 21 | no color write: color mask 0, or a blend that keeps the destination (ZERO, ONE) | capture (`cmask0`); hardware (open driver) |
| 22-24 | depth compare, `GL enum - 0x200`: never, less, equal, lequal, greater, notequal, gequal, always. Depth test off, or no depth buffer, gives always + no write (`0x01d00000`) | hardware (greater); capture (`dless`, `dnever`, `dgreater`); hardware (open driver) |
| 25-27 | pass type: 0 opaque, 1 translucent (any glBlendFunc; `b1`/`b2`/`b3` captures), 2 translucent + discard, **3 viewport/scissor generate** (`viewport`/`scissor` captures), 4 punch-through (discard/alpha test), 5 depth-only (stencil/discard special case) | capture (0, 1, 3); hardware (4: run a5; 0, 1, 3, 4: open driver); unconfirmed (2, 5) |

### ISPB

| bits | field | tag |
|---|---|---|
| 4-7 | color write mask: R = 8, G = 4, B = 2, A = 1 | capture (`cmask`); hardware (open driver) |
| 8-15 | line width × 16 (1.0 → 0x10, 9.5 → 0x98); points keep 0x10, their size is a vertex output | hardware |
| 23 | depth bias (POLYGON_OFFSET_FILL) | capture (`polyoff`); hardware (open driver) |

### ISPC: stencil

The front face's word and the back face's word.

| bits | field | tag |
|---|---|---|
| 0-7 | write mask (glStencilMask) | capture; hardware (open driver) |
| 8-15 | compare mask (glStencilFunc mask) | capture; hardware (open driver) |
| 16-18 | zpass op | capture; hardware (open driver) |
| 19-21 | zfail op | capture; hardware (open driver) |
| 22-24 | sfail op | capture; hardware (open driver) |
| 25-27 | compare, `GL enum - 0x200`; 7 (`0xe000000`) when stencil is off | capture; hardware (open driver) |

The op codes are 0 keep, 1 zero, 2 replace, 3 incr, 4 decr, 5 invert,
6 incr_wrap and 7 decr_wrap.

Seen field by field in the `s1`..`s6` captures:

- `0400f0ff` = EQUAL, compare mask 0xf0, write mask 0xff;
- `0453f0ff` = ops ZERO/REPLACE/INCR;
- the reference lands in ISPA b0-7 (`01d00355`).

**glClearStencil** is the clear object getting ISPC `0e0200ff` (always,
zpass = replace, write mask 0xff) with the clear value as its ISPA
reference (`cstencil`) [capture].

### PDS words

- **Word 0 (secondary) and word 2 (primary):**
  `(data bytes/16)<<26 | (addr - 0x2e000000)>>4` [capture; hardware, open
  driver].
  - The secondary program DMAs the fragment uniforms and runs the secondary
    USSE program.
  - The primary program runs the fragment shader: DOUTU, plus DOUTI and
    DOUTT per input.
- **Word 1:** from the fragment program.
  - b18-24 = (secondary attribute dwords + 3)>>2 [capture; hardware, open
    driver].
  - b0-15 = the primary attributes in units of 4 dwords, at least 1. A
    program that reads more attributes than the word declares locks the GPU:
    two F32 vec4 varyings under a constant `…0001` (2026-10-28, log
    `2026-10-28-1530-gl4es-ctest`) [hardware].
  - b16-17 = 3 in every capture [capture].
  - b25-28 = `12 / units` (integer) when the program has no secondary
    attributes (b18-24 = 0), and 0xb when it has some. The DDK's built-in
    clear program has 0. From the 2026-11-08 captures, with n vec4 varyings
    fed straight to gl_FragColor:

    | varyings | word 1 | units | b18-24 | b25-28 |
    |---|---|---|---|---|
    | 0 (uniform color) | `160f0001` | 1 | 3 | 0xb |
    | 1, 2 mediump (or lowp) | `18030001` | 1 | 0 | 12 |
    | 3, 4 mediump | `0c030002` | 2 | 0 | 6 |
    | 5 mediump | `08030003` | 3 | 0 | 4 |
    | 2 highp | `0c030002` | 2 | 0 | 6 |

    A mediump or lowp vec4 varying takes 2 dwords (F16) and a highp one 4,
    so units = ceil(attribute dwords / 4). [capture]. The rule with both
    secondaries and more than one unit is [unconfirmed].

### The DDK's fragment programs for a flat color and one sample

From the DDK running `sgx_perfmicro` (a 2026-11-08 capture; 720x480, eight
full-screen quads a frame):

**Flat color with blending** (`gl_FragColor = color`, a mediump vec4
uniform; SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
- **Per draw: the secondary program packs the color once.** The PDS
  secondary DMAs 11 dwords of uniforms and runs it:

      pck.u8.f16 pa9, pa9.l2, pa9  [skipinv dmask=6 scale]
      pck.u8.f16 pa9, pa10, pa10.l2  [skipinv end dmask=9 scale]

  This leaves the color as one packed U8 dword in sa9.
- **Per pixel: two instructions, 0 temporaries:**

      phas.i #0, #0 [wait=7]
      mov pa0, sa9  [skipinv mask=1]
      sop2 o0, pa0, o0  [skipinv end c1=s1a c2=1-s1a cop=add a1=s1a a2=1-s1a aop=add]

- **The words:** ISPA `03d00200` (pass type 1, translucent), PDS word 1
  `160f0001`, and a primary program that is one DOUTU (temps 0, no
  iterations, no samples). Without blending the per-pixel program is the
  `mov` alone.

**One texture sample, output unchanged** (`gl_FragColor = texture2D(t, uv)`,
256x256 RGBA8888, LINEAR, REPEAT).
- **The PDS primary:**
  - DOUTU (temps 0, w1 `02000004`: one sample);
  - DOUTI `46012201` (sample tc0, 2D);
  - DOUTT `07fe1400 0d0ff0ff f0001000 00800800` (twiddled, layout 0).
- **The sample lands in pa0 already packed as U8**, and the per-pixel program
  is one instruction:

      phas.i #0, #0 [wait=7]
      mov o0, pa0  [skipinv end rcnt]

- **The words:** PDS word 1 `18030001` (one unit, no secondaries) and ISPA
  `01d00200`.

So for these two cases the DDK does no per-pixel format conversion. The
color is packed per draw, and the texel stays in the U8 format the sampler
delivers. [capture]

### Viewport (6 floats)

The words are, in order: x offset, x scale, y offset, y scale, z offset and
z scale.

- x offset = x + w/2, and x scale = w/2.
- On a window, y scale = -h/2 and y offset = H - (y + h/2). On a pbuffer
  (EGL type 4), y scale = +h/2 and y offset = y + h/2.
- z offset = (n+f)/2, and z scale = (f-n)/2.
- Status: x offset [hardware: p1]; the rest [capture: `viewport`, `drange`
  and `pb` captures; hardware, open driver].

### MTE control

- **b0-1, cull:** 0 = none, 1 = cull CW-in-screen, 2 = cull CCW-in-screen.
  - GL CCW front with cull BACK gives 1; cull FRONT gives 2.
  - With CW front the two swap.
  - FRONT_AND_BACK is 0 here; GL drops those draws itself.
  - Status: hardware. On the replay, 2 removes the probe triangle and 1
    keeps it. In the captures, `cullb` gives 1, and `cullf` and `cullcw`
    give 2.
- **The rest is a constant, `0x1555620`** [capture: every draw; any meaning
  is a guess]. The clear object uses `0x40` [capture]. The DDK's ES1 draws
  use `0x15555620`, and `0x3a0` with flat shading ("Flat shading" below)
  [capture].

### Output select and TSP words

- **Output select:** `(vertex output dwords)<<16 | 0x3000`, with `| 0x100`
  when the shader writes the point size.
  - 4 dwords are the position. Each texcoord set adds its size.
  - The clear's own value is `0x00081800` (8 dwords: position and color).
    Its texcoord-size word is 5 (one 4-component set: the color) and its
    MTE control 0x40.
  - Status: capture; hardware (open driver).
- **The clear's vertex program** (the DDK's built-in, `0x3c008080` in the
  base4 capture; 4 input dwords from one vertex stream, 8 output dwords):

      phas.i #<phase 1>, #0 [wait=2]; then the prologue (nop / test / ld pa / wdf)
      mov o0, pa0 [skipinv rcnt rpt=3]                o0-o2 = x, y, z
      mov o3, c3 [skipinv rcnt]                       o3 = 1.0 (w)
      pck.f32.u8 o4, pa3.l2, pa3.l2 [skipinv rcnt scale]   R = byte 2 of the ARGB8888 word
      pck.f32.u8 o5, pa3.l1, pa3.l1 [...]                  G = byte 1
      pck.f32.u8 o6, pa3,    pa3    [...]                  B = byte 0
      pck.f32.u8 o7, pa3.l3, pa3.l3 [...]                  A = byte 3
      emit.t3 r0, #0, #0 [end f12 f10]
      phas.i #0, #0 [wait=7]; emit.t1 r0, #0, #0 [end f12 f10 f3_20]

  So the vertex stream is x, y, z (floats) and the color as one ARGB8888
  word, and the outputs are position (4) then color as 4 floats in 0-1
  [capture].
- **TSP word 0:** 3 bits per texcoord set: 1, 2, 3 components, or 5 for 4
  components [capture; hardware, open driver].
- **TSP word 1:** a bitmask of the sets iterated at F16: mediump and lowp
  sets, not highp [capture].
  - In the 2026-11-08 captures, 1-5 mediump vec4 varyings give 0x1, 0x3, 0x7,
    0xf and 0x1f, two lowp give 0x3, and two highp give 0.
  - TSP word 0 is 5 per set throughout (`2d`, `16d`, `b6d`, `5b6d`).
  - The output select is `(4 + 4n)<<16 | 0x3000` whatever the precision: the
    vertex outputs stay 32-bit. [capture]

### Blending and the color mask are shader code

- **The SGX545 has no fixed-function blender.**
  - glBlendFunc and glBlendEquation change no state word but ISPA's pass
    type (translucent) and b21. The fragment shader gets blend code
    appended. [capture]
  - The clear color and glBlendColor go into the shader's constants
    [capture].
- **The color mask is both shader code and ISPB.** It is ISPB b4-7, plus a
  masked move appended to the shader, plus ISPA b21 when the mask is 0
  [capture]. That the appended move's word is `0x90b00601 | mask<<11` is
  [unconfirmed].

So reproducing GL blending means generating the shader epilogue, not
setting a state word.

- **The captures agree.** `b1` (SRC_ALPHA, 1-SRC_ALPHA), `b2` (ONE, ONE)
  and `b3` (+ FUNC_SUBTRACT) differ from the base only in ISPA pass type
  0 → 1, plus the fragment shader.
- **Color mask 1 0 1 1** gives ISPB mask 0xb and a different clear shader.
- **Color mask 0** sets ISPA b21 and ISPB mask 0. The clear turns into a
  full-screen object that samples the previous buffer instead (a masked
  clear changes nothing).

**Decoded on a CE5300 board** (`b1`, `cm1`-`cm3` captures of probe-st; the Mesa
driver draws them the same way, byte-identical to the DDK) [capture;
hardware, open driver]:

- **The blend** is one `sop2` after the color:
  `sop2 o0, pa0, o0 [skipinv end c1=s1a c2=1-s1a cop=add a1=s1a a2=1-s1a aop=add]`
  for SRC_ALPHA, ONE_MINUS_SRC_ALPHA. s1 is the shader's color, s2 the
  tile's (o0 starts as the tile's color in a translucent pass). GL's
  factors are sop2's selectors: zero, s1, s2, s1a, s2a, asat, each with a
  complement (1-zero = one). Alpha has its own selectors and op, so
  separate blend functions and equations cost nothing.
- **Reverse subtract** works by trading the sources (`sop2 o0, o0, pa0`)
  and renaming the selectors. The DDK has no EXT_blend_minmax: GL_MIN/MAX
  raise INVALID_ENUM there. sop2's min and max ops do them.
- **The color mask** is
  `sopwm o0, pa0, pa0 [skipinv end mask=m c1=zero c2=zero cop=max aop=max]`
  after the color, with m one bit per byte of the packed BGRA8 (B b0, G b1,
  R b2, A b3). It comes with ISPB's mask, and ISPA stays opaque.
- **Blend and mask together.** The DDK's sop2 loses `end` and writes `r0`,
  then the sopwm reads pa0. One more word changes, 0x2e018020 in `cm2`
  (0x00010700 → 0x01010700), perhaps a temporary count for that r0 [guess].
  The Mesa driver writes the blend back to pa<c> instead, with the same
  pixels.

### Constant-color blending

`sgxs_blend_const` in `tools/state/sgxstate.h` builds the five
instructions. They are bit-exact with the DDK's for k1-k5, k7 and k8
[capture].

sop2's selectors can't name a constant. With any CONSTANT_COLOR /
ONE_MINUS_CONSTANT_COLOR / CONSTANT_ALPHA / ONE_MINUS_CONSTANT_ALPHA
factor, the DDK multiplies each side out with sopwm and adds the halves
[capture: k1-k8]:

    sopwm r1, Y, o0      [c1=zero c2=Fd]           dst * Fd (all four bytes)
    sopwm r1, Y, o0      [mask=8 c1=zero c2=Fda]   alpha byte: dst.a * Fda
    sopwm r0, pa<c>, X   [c1=Fs c2=zero]           src * Fs
    sopwm r0, pa<c>, X   [mask=8 c1=Fsa c2=zero]   src.a * Fsa
    sop2  o0, r0, r1     [end c1=one c2=one cop=add a1=one a2=one aop=add]

- X and Y are whichever of the color, the tile (o0) or the constant the
  factor reads.
- **The constant is the blend color, packed BGRA8 like the color, in a
  fragment secondary attribute.** The DDK puts it in sa2 of its 11-dword
  constant DMA: glBlendColor(0.25, 0.5, 0.75, 0.375) = `604080bf` [capture].
- Products round to nearest: 204 × 64/255 = 51.2 gives 51 (`k1`) [capture].
- Turning k1's src-RGB sopwm into 1-s2 or s2a reproduces the DDK's own
  ONE_MINUS_CONSTANT_COLOR and CONSTANT_ALPHA frames [hardware: b2, b3].
- The final sop2 carries the blend equation (2026-11-08 captures, src
  CONSTANT_COLOR, dst ONE_MINUS_CONSTANT_COLOR):
  - FUNC_ADD: `sop2 o0, r0, r1 [... cop=add aop=add]` (`81848805 00000001`);
  - FUNC_SUBTRACT: `sop2 o0, r0, r1 [... cop=sub aop=sub]` (`81848805 00050001`);
  - FUNC_REVERSE_SUBTRACT: the same ops with the sources swapped,
    `sop2 o0, r1, r0` (`81848805 00050080`).

  The four sopwm products are the same in all three. With CONSTANT_ALPHA /
  ONE_MINUS_CONSTANT_ALPHA the factors become `s2a` / `1-s1a`. [capture]

**k18/k19 are not a constant.** In `kb0` (ONE, ZERO) the sop2 was turned
into `c1=k18`, `c2=k19`, `a1=k18` and `a2=k19` (`kk1`-`kk4`, discovery)
[hardware: kk1-kk4]:

- k19 reads 0 in color and alpha.
- k18 as c1 reads a different value per 4×4 block, zero in R and G. The
  values step by the shader color's own factor (102, 82, 66, 53 … =
  102 × (204/255)^n): state left behind by earlier tasks, not a register
  the program sets.
- k18 in a1 gave alpha 255.

So neither can carry a blend color. The sopwm form above is what to use.

### Alpha test (ES1)

The DDK's ES1 driver (`tools/probe/probe1.c` captures `e1*`) needs
no state words for it. The fragment program ends in one visibility-test
instruction instead of its final `mov o0, color`:

    atst8 o0, p0.nw, pa0, sa0, sa1  [end]      f0000001 f934000d   (e1gt5)

with ISPA pass type 1 (`03d00200`). `tools/state/sgxstate.h` has
`sgxs_atst8`, `sgxs_atst_depth_word` and `sgxs_atst_alpha_word`.

**The operands.** The secondary program writes sa0 and sa1 with DOUTA:

| word | value | fields | tag |
|---|---|---|---|
| sa0, depth | `0000e00f` | `0xe000 \| depth func (GL - 0x200, 7 with no depth test) \| 8 when depth writes are off` | capture |
| sa1, alpha | `80008e00` (GREATER 0.5) | `ref8 << 24 \| alpha func (GL - 0x200) << 13 \| 0xe00`, ref8 = round(ref × 255): 0.5 → 128, 0.25 → 64 | hardware |

- From `e1gt5`, changing sa1 to the reference 0.25 reproduces the DDK's
  own `e1gt25` frame. Changing it to LESS or GEQUAL reproduces `e1lt5` and
  `e1ge5`. All 0 pixels off [hardware: a1-a3].
- ALWAYS (`8000ee00`) draws the frame with no alpha test [hardware: a7].
- The Mesa driver's discard `atst8` (`f9340406`, sa3/sa4) carries these
  same two words with the alpha function ALWAYS (`0xee00`). So its discard
  program does an alpha test as soon as sa4 has another function and
  reference [unconfirmed].

**The instruction.** atst8 fields:

- dst l21-27, bank h0-1 (0 r, 1 o, 2 pa);
- color source l14-20, bank h2 (0 r, 1 pa);
- the two sa operands l7-13 and l0-6 (bank l28-31 = 0xf);
- end h18;
- h6 = write p0 (`p0` instead of `p0.nw`);
- h9-10 = the kill predicate (the discard form).

The DDK's ES1 words also set **h3** (`sel36` in dis.py). With h3 cleared
the replay's render stalls (447 ms) and leaves the right part of the frame
unwritten, with no SGX recovery [hardware: a6]. Keep it.

**Pass type.** It must not be opaque. With pass type 0 the failing pixels
are drawn with garbage [hardware: a4]. Pass type 4 (punch-through, the Mesa
driver's discard) works exactly like the DDK's 1 [hardware: a5].

**Variants** [capture]:

- **Textured** (`e1texat`, GL_MODULATE): `sop2 r0, pa0, pa1` modulates,
  then `atst8 o0, p0.nw, r0, sa0, sa1 [end]` (`f9340009`).
- **With a blend** (`e1atbl`): `atst8 r0, p0, pa0, sa0, sa1` (`f930004c`,
  writes p0, no end), then the blend predicated on it:
  `(p0) sop2 o0, r0, o0 [end ...]`.
- **GL_NEVER** (`e1nev`): no atst8. The draw gets ISPA `01f00200` (b20 and
  b21: no depth, no color writes).
- ES1 points and lines draw as in "Lines and points" (`e1pt`, `e1spr`,
  `e1ln`).

### Scissor and partial viewports: pass type 3 objects

There is no scissor rectangle register. The ISP keeps one **visible bit
per pixel**, and every object is drawn only where it is set. Pass type 3
("viewport generate") objects write the bit and draw nothing else. The
**region clip** (group 0x80) limits which tiles the TA bins an object into,
and it is **sticky**: an object without group 0x80 keeps the last one.
`tools/state/sgxstate.h` builds all of it (`sgxs_scissor_objects`).

**The two objects** (TA block header `d485`: ISPA, ISPC, region clip,
output select, MTE, TSP; 9 dwords, DMA kind 0; no PDS group, so no pixel
program runs):

| object | ISPA | ISPC | region clip | geometry (screen space, y down) |
|---|---|---|---|---|
| reset | `07e00100` | `00000000` (b25-27 = 0) | the target, or the old rectangle's tiles | triangle (0,0) (2w,0) (0,2h), VDM prim 0 |
| set | `07e00100` | `02000000` (b25-27 = 1) | the rectangle's tiles | 4-vertex strip TL TR BL BR, VDM prim 3 |

- ISPA `07e00100`: pass type 3, b21 no color write, depth always with
  write, ISPC follows [capture; hardware, open driver].
- ISPC b25-27: 0 resets the bit under the object, 1 sets it [hardware: q1,
  q4].
- The vertices are 2 dwords (x, y). The vertex program is
  `mov o0, pa0 [rpt=2]; mov o2, c3; mov o3, c3` (z = w = 1.0), output
  select `0x00040000`, MTE `0x40` [capture; hardware, open driver]. The
  words are in `sgxstate.h`.
- Pixel centers inside the strip get the bit: moving its left edge from
  x 100 to 150 moved the scissor's edge [hardware: q2].
- The set object only reaches its region clip's tiles [hardware: q3].
- **Region clip words:** `0x80000000 | x0/32 << 16 | (x1-1)/32`,
  `y0/16 << 16 | (y1-1)/16`, for a rectangle `[x0,x1) × [y0,y1)` in target
  pixels. `glScissor(37, 23, 101, 57)` on 720×480 = x 37..138, y 400..457 =
  `80010004 0019001c` [capture: sc2; hardware, open driver].
- **Draws inherit it:** with the reset turned into a set (everything
  visible), the triangle was still cut to the tiles of the clear before it
  (columns 3-9, rows 20-26), 0 pixels off that prediction [hardware: q1].

**What the DDK emits** (captures `sc1`-`sc6`, `vp1`, `nc0`) [capture]:

1. A render that doesn't start with a full-target clear starts with a
   **load object**: a full-target triangle (0,0) (1440,0) (0,960), x y z u v
   per vertex, sampling the color buffer's previous contents (T0
   `00b60e90`, T1 `cc2cf1df`: layout 6, format 0xc). It comes with
   `PROBE_NOCLEAR` and no scissor too (`nc0`), so it is the render's "load",
   not part of the scissor. With a scissor it is what keeps the pixels
   outside it.
2. Scissor (or a viewport) smaller than the target: **reset** over the whole
   target, **set** over the rectangle.
3. glClear inside a scissor is a strip over the rectangle (4 vertices, x y z
   color) with the rectangle's region clip.
4. Draws: unchanged (own viewport transform, no group 0x80) after a clear
   or a set object. With no clear in the render, the draw itself carries
   the region clip (header `dfc3`, `sc4`).
5. The scissor changes between draws (`sc5`): reset with the old
   rectangle's region clip (full-target triangle), then set the new
   rectangle.
6. A zero-area scissor (`sc6`): the reset only; its clear is a degenerate
   strip at (0,480).
7. A scissor covering the whole target (`sc3`) emits nothing: the capture
   equals `base3`.
   Turning the scissor off between draws (`sc7`) is one pass-type-3 object
   over the whole target with ISPA `07e00000` and **no ISPC group** (header
   `d481`, full region clip). The next draw's block then carries only the
   changed groups (header `d483`: ISPA, ISPB, a full region clip), with no
   PDS or viewport group. So every group of the TA state block is sticky,
   not just the region clip [capture].
8. A partial glViewport (`vp1`, no scissor): the clear stays full-target;
   the draw is preceded by reset + set over the viewport rectangle and has
   the viewport transform of section 3.

### Lines and points

`tools/state/sgxstate.h` has the words (`sgxs_prim`, `sgxs_ispa_objtype`,
`sgxs_ispb_width`, `sgxs_point_size_word`, `sgxs_line_list_indices`).

**Primitives.** The VDM index-list word is
`0x80000000 | 0x00c00000 | prim << 26 | count`. prim is 0 triangles,
1 lines, 2 points, 3 triangle strip, and b28 makes prim 0 a fan
(`90c00004`) [capture; hardware, open driver]. There is no line strip or
loop. The DDK's GL_LINE_STRIP of
4 vertices is a prim-1 list of count 6 over the identity indices
(`lstrip`): its 2(n-1) vertices are the strip's n plus n-2 that follow them
in memory, so its frame equals GL_LINES' (`cmp` identical) and with real
data behind the strip it draws the stray lines seen under gl4es. Its
GL_LINE_LOOP is right: count 8 over its own indices 0 1 1 2 2 3 3 0
(`lloop`). A driver expands strips and loops into line lists the way the
loop does.

**ISPA object type** (b15-18, replaced per draw): 0 triangles, 1 lines, 3
points, 4 point sprites [capture].

- A line list with object type 0 draws nothing, whether the VDM prim is
  turned into lines (`tri2`) or the type turned off (`ln1`) [hardware: l3,
  l4].
- Object type 4 is what generates gl_PointCoord: with 3 instead, the same
  points are drawn the same size but the coordinate reads 0 [hardware: p4].

**Line width** is ISPB b8-15 = width × 16, truncated, at most `ff`: 1.0
`10`, 1.06 `10`, 4 `40`, 9.5 `98`, 16 `ff`. The DDK's other draws all carry
`10` [hardware: l1, l2; capture: `ln106`, `ln16`]. glLineWidth clamps to
[1, 16] [unconfirmed].

**Points.** A point draw's vertex program writes one output dword after
the position and the varyings: **the size as an f32**, clamped to 511.0, the
largest point size [capture; no draw of ours has run this yet].

- A computed size: `fminmax o6, pa4, pa4, sa10.f16.lo [fmt]` (corpus `vo_ps`;
  `ptattr`), which is min(x, 511) with sa10's low half = 0x5ffc. The fminmax
  writes f32 whatever format its sources are in.
- gl_PointSize = 8.0: the program is `mov o6, sa10`, and sa10 is loaded with
  `5ffc4800`, the F16 pair (size 8.0, 511.0); 16.0 gives `5ffc4c00`. That word
  is the input of the secondary program, `fminmax pa10, pa10.f16.hi,
  pa10.f16.hi, pa10.f16.lo` (corpus `vo_psk`), which leaves min(size, 511) in
  sa10 as an f32 before the vertex program reads it.
- Changing the loaded word's size from 8.0 to 16.0 draws the DDK's own
  16-pixel frame [hardware: p1]. That replay still runs the secondary
  program, so it doesn't show the output format by itself.
- Output select gets 0x100 (`00073100`) [capture].
- The ES2 driver gives every point draw a two-component texcoord set ahead
  of the size (TSP words 2 and 1). The vertex program doesn't write it
  (o4-o5 stay unwritten), so output select counts 4 + 2 + 1 = 7 [capture].
- The ES1 driver adds that set only for a sprite. `e1pt` has none (output
  select 9 = position 4 + color 4 + size 1). `e1spr` puts it after the
  vertex color (11, TSP word `0x15`) [capture]. So it is only needed when
  gl_PointCoord is read.
- The coordinate set comes first, as tc0. A shader's own varyings follow it
  (a varying vec2 is iterated as tc1), and the size comes last: `ptvar` /
  `sprvar` have output select `00093100`, 4 + 2 + 2 + 1, size in o8
  [capture].
- In a sprite (object type 4) the ISP writes gl_PointCoord there, and the
  pixel program iterates it as tc0 (`DOUTI iterate tc0 x2 f16`, DOUTU w1
  b24) [capture]. The values are `(i + 0.5) / size` across the point, origin
  top left. As `round(255 × value)` this predicts the DDK's 16-pixel sprites
  exactly, and resized ones at 8 and 32 [hardware: p2, p3].
- A point's square is `[c - size/2, c + size/2)` around its center
  [hardware: p1-p3].
- Without gl_PointSize written (`pt1`) the DDK draws nothing [capture].

### Clip planes, shading, polygon and line state: native or lowered

What GL 2.1 asks of rasterization, against what the hardware is known to do.
"Native" means a state word does it; the rest is lowered in the shader or
on the CPU. The driver already gathers vertices on the CPU (`sgx_screen.c`,
vertex buffers), so CPU expansion costs a copy it already makes.

| GL state | how | tag |
|---|---|---|
| cull face, front face | native: MTE control b0-1 | hardware |
| polygon offset (fill) | native: ISPB b23 per state block; units and factor are render details +0x14/+0x18, **one pair per render**, so a change of values splits the render (`depth_bias` in the winsys) | capture (`polyoff`); hardware (open driver) |
| line width | native: ISPB b8-15 = width × 16, at most 16 | hardware |
| point size, sprites | native: a vertex output; ISPA object type 4 generates gl_PointCoord | hardware |
| line strips and loops | lowered: no strip primitive; expanded to line lists | hardware |
| scissor, viewport | native: region clip plus pass-type-3 objects | hardware |
| user clip planes | native: `fdpc` into cp<n> in the vertex program, enabled by output select b0-5 | capture (`cp1`, `cp2`, `cp6`, `cpnone`) |
| flat shading, provoking vertex | native: pixel PDS DOUTI b25-26 = 2 for the iteration; the last vertex provokes | hardware (`fl-douti` replay); capture (`flat`) |
| logic ops | shader code: pass type 1, and the program combines its color with o0, which holds the destination | capture (`lxor`, `lcinv`) |
| polygon mode LINE/POINT, edge flags | native for the mode: ISPA object type 5 draws a triangle's edges, 6 its vertices; edge flags not known | hardware (`ot5`, `ot6` replays) |
| polygon offset for LINE/POINT modes | follows polygon mode: ISPB b23 on an object type 5 or 6 draw is untried | unconfirmed |
| line stipple | no ES state, so no DDK capture; no field known; lowered | — |
| polygon stipple | no ES state, no field known; lowered | — |
| line and point smooth | no hardware antialiasing seen: the DDK draws a smooth line 1 px wide and a smooth point 1 px | capture (`lsmooth`, `psmooth`) |

**User clip planes are native** (**capture**: `probe1` with
`PROBE_CLIPPLANE`, 2026-11-11: one plane `1 0 0 0` (`cp1`), two (`cp2`), six
(`cp6`) and one that cuts nothing, `1 0 0 1` (`cpnone`)). The read-backs
clip exactly: `cp1` keeps the triangle's x ≥ 0 half, `cp2` its x, y ≥ 0
quarter, `cp6` less again, and `cpnone` matches the base pixel for pixel.
Against the base, the DDK changes three things and nothing else (no MTE
control, TA header or ISP word):

- **The vertex program** computes the eye-space position and, for plane n,
  ends with `fdpc o(7+n), cp<n>, pa0, c52 [rpt=2 inc0 inc1]` (as `dis.py`
  prints it). Six planes give `cp0` to `cp5`, one `fdpc` each; no `fddpc`.
  The planes' coefficients arrive as constants in the PDS vertex program's
  data (the values 1, -1, 0.4 and 0.2 of `cp6` are there).
- **Output select** (TA block word, `0x304110b4` in these captures) gets
  one bit per plane in b0-5 and one more output dword per plane:
  `00083800` with none, `00093801` with one, `000a3803` with two,
  `000e383f` with six. The `fdpc` result is that extra output. Which output
  register it lands in (o7+n as printed, or o8+n after the repeat) is
  **unconfirmed**: the count says 8 + n dwords.
- **The VDM draw words** follow the bigger program: the vertex PDS
  program's address and data size.

So the TA clips against the `fdpc` results of the planes whose bit is set.
The driver sets `caps->clip_planes = 0` today, so st/mesa lowers user planes
into the vertex shader (`nir_lower_clip_vs` writes gl_ClipDistance). The
native path is: the compiler emits each distance with an `fdpc` into cp<n>
as an extra output, and the driver sets output select's mask and count. How
the compiler gets a gl_ClipDistance through `fdpc` (a dot product of the
distance with a unit vector, or of the position with the plane) is its
choice (**guess** until the hardware test below). Clipping in the TA works
on lines too, so the discard fallback, which can't run on lines, isn't
needed.

- **Test (open stack):** a replay or `sgx_hwt` frame with one `fdpc` into
  cp0 and output select `00093801`, drawing a triangle and a line across
  the plane, read back. It confirms the output slot and that lines clip.
- **Fallback, if the test fails:** `nir_lower_clip_fs` turns the distances
  into varyings and a discard. That costs the punch-through pass and a
  varying per four planes, and **not on lines:** any program with
  visibility feedback breaks a frame drawn with lines, whether or not the
  discard is taken (ISA.md, "Phases, visibility feedback and discard":
  hardware). Clipped lines would have to be drawn as thin triangles.

**Flat shading.** ES1 has `glShadeModel(GL_FLAT)`, so a `probe1` capture with
`PROBE_SHADEMODEL=0x1D00` against the base (`0x1D01`) (**capture**, `flat`,
2026-11-11). The whole triangle reads back (0, 0, 255), the third vertex's
color: the last vertex provokes, as GL wants. Three words change, each by
itself in its own buffer:

| word | smooth (base) | flat |
|---|---|---|
| pixel PDS DOUTI for the color (`0x2e01802c`) | `27c10000` (b25-26 = 3) | `25c10000` (b25-26 = 2) |
| MTE control (TA block, `0x304110bc`) | `15555620` | `000003a0` |
| VDM draw word 2 (`0x3e519038`) | `f0000000` | `f4000000` (b26) |

**DOUTI b25-26 alone is the switch** (**hardware**: replays of the base on a
CE5300 board, 2026-11-11, one word changed at a time). With `25c10000` the
dump matches the `flat` capture pixel for pixel. The MTE control and VDM
draw word changes, alone, leave the dump byte for byte equal to the base.
PDS.md has b25-26 as 3 on every shader entry and 0 on the clear's and
background's iterations, so the field is the iteration's shading mode,
with 2 flat and 3 smooth (Gouraud). It is set per iterated attribute, so a
GLSL `flat` varying is the same switch on that one DOUTI. What the MTE
control word and VDM b26 change is unknown. Neither selects the provoking
vertex: replays of `flat` with the MTE word set back to `15555620`, with
VDM b26 cleared, and with both, all give the capture's dump byte for byte,
the last vertex's color (**hardware**, 2026-11-11). A first-vertex
convention has no known field; rotating each triangle's indices is the
fallback. The MTE control word was a constant in every ES2
capture (`01555620`); what its other fields mean is still a **guess**.

**Logic ops.** The ES1 has `glLogicOp`. Blending and the color mask are
fragment-shader code here (§3, "Blending and the color mask are shader
code"), and a logic op is too (**capture**: `probe1` `PROBE_LOGICOP=0x1506`
(XOR, `lxor`) and `0x150C` (COPY_INVERTED, `lcinv`), 2026-11-11). Against the
base, two things change: ISPA goes from pass type 0 to 1 (`01d00200` →
`03d00200`), and the pixel program's one instruction, `mov o0, pa0`,
becomes `xor o0, pa0, o0` for XOR and `and.inv o0, c56, pa0` for
COPY_INVERTED (c56 holding all ones). Under pass type 1, o0 starts as the
destination pixel, packed 8888. The read-backs agree: the centre's
(63, 64, 128, 160) over the background (0, 0, 128, 255) gives (63, 64, 0, 95)
for XOR and (192, 191, 127, 95) for COPY_INVERTED. `nir_lower_blend` already
takes a logic op (`logicop_enable`, `logicop_func`). The compiler's side, a bitwise table of
the 16 ops after the color, is in ISA.md "What a compiler needs".

**Polygon mode and edge flags.**
- **The ISP has polygon modes** (**hardware**: replays of the base triangle
  on a CE5300 board, 2026-11-11, with ISPA b15-18 changed):
  - Type 5 draws the triangle's three edges as 1-pixel lines, with the
    vertex colors interpolated along each edge: 840 pixels, two per row
    down the sides and the whole bottom row.
  - Type 6 draws its three vertices as single pixels, in the vertex colors.
  - Types 1 (lines), 2 and 3 (points) draw nothing from triangle data, as a
    line list drawn with type 0 draws nothing (§3, "Lines and points").
  - Type 7 locks up the 3D core (HWR, `lockup_3d`). Never write it.
- So GL_LINE and GL_POINT polygon modes are type 5 and type 6 on a
  triangle draw. Front and back can differ, since ISPA is per side.
  Unknown: the width and size these draw with (1 pixel at line width 1 and
  no point size here), whether ISPB's line width applies, whether a vertex
  output gives type 6 a point size, and how edge flags are given. Polygon
  offset on them is untried (the table above).
- **Lowered, for edge flags:** `u_unfilled_indices` (gallium aux) rewrites
  triangle indices into line or point lists, and the CPU expansion has the
  per-vertex flag to hand.

**Line and polygon stipple.**
- **Line stipple:** the CPU already expands strips and loops, so it can add
  a per-vertex distance along the line, in pixels. The fragment shader then
  discards by the pattern and factor, both uniforms.
- **Polygon stipple:** `util_pstipple` (gallium aux) puts the 32×32 pattern
  in a texture and adds a discard to the fragment shader.
- Both discard, so both pay for the punch-through pass. A discard breaks a
  frame with lines (see "User clip planes" above), so stippled lines have to
  be drawn as thin triangles: two per segment, from the same CPU expansion.

**Line and point smooth.** ES1 has `GL_LINE_SMOOTH` and `GL_POINT_SMOOTH`.
The DDK antialiases neither (**capture**: `lsmooth`/`lsharp`, a width-3
line, and `psmooth`/`psharp`, a 16-px point, 2026-11-11):
- with `GL_LINE_SMOOTH` the only change is ISPB's width, `30f0` → `10f0`
  (3.0 → 1.0), and the line reads back 1 px wide (360 pixels against
  1080);
- with `GL_POINT_SMOOTH` the only change is the point size the vertex
  program writes, 16 → 1, and the point reads back as one pixel.

So no state word for smoothing shows up. GL lets a driver report a smooth
line width range and point size range of 1 to 1, which fits what the DDK
draws (**guess**: the DDK's ranges weren't queried). Mesa's
`nir_lower_poly_line_smooth` gives real coverage in the shader, at the cost
of a blend.

## 4. The render target

### The end-of-tile program (pixel event → EOT USSE program)

- Render details +0x5c points at the pixel event PDS program (0x2e006000,
  the same program in every capture).
- Its ds1[1] is a DOUTU word for the end-of-tile USSE program, here
  0x37004000.
- The program starts with `phas.i #0, #0 [wait=7]` (`fa440700 00000000`).
  The pixel event's DOUTU starts it with w0 = ds1[1] = `0x01008000`
  (w0[31:22] = 4, the temporaries for r0-r3) [capture].
- The program is four LIMMs that load the PBE words into r0-r3, then two
  EMITPIX [capture; hardware, open driver]. The disassembler shows them as
  `emit.t0 r0, r1, r0 [f3_20 imm=2048]` and
  `emit.t0 r2, r3, r0 [end f3_20 imm=1536]`, the second imm being the
  sideband word (0x600):

      LIMM  lo = v & 0x1fffff | reg<<21,  hi = 0xfca00000 | (v>>21 & 0x1f)<<4 | (v>>26)<<12
      EMITPIX lo 0x00200080, hi 0xfb200100                        (the four PBE words)
      EMITPIX lo (side & 0x3f)<<22 | 0x00208180,
              hi 0xfb240000 | (side & 0x3000)<<10 | (side>>3 & 0x1f8)   (the sideband word)

### The pixel event program (render details +0x5c)

The DDK's, from the base4 capture (0x2e006000, data 0x50 bytes, 29
instructions). The data rows:

| word | value | what |
|---|---|---|
| ds0[0], ds0[1] | `00010400`, 0 | DOUTU of the ptoff task (`0x37008200`) |
| ds1[0] | 0 | compare operand for in1's b8 test |
| ds1[1] | `01008000` | DOUTU w0 of the end-of-tile task (`0x37004000`, temps 4) |
| ds0[2], ds0[3] | `01010500`, `00c00000` | DOUTU of the pixel-event emit task (`0x37008280`, temps 4), w1 = b22 \| b23 |
| ds1[2], ds1[3], ds1[4] | 8, 2, 4 | the in1 bit masks |
| ds0[4]..ds0[8] | `00000100 00000ff8 00800000 003ff000 00400000` | masks for building the end-of-tile DOUTU w1 |

The code, by in1 (the event word the hardware passes):

- in1 & 8 → DOUTU the **ptoff** task, then halt. That task is
  `phas.i #0, #0 [wait=7]; ptoff [end]`.
- else in1 & 2 → test `op0xd(in1, ds1[0]) & 0x100`:
  - if set → `douta in2 → attr 0`, then DOUTU the **end-of-tile** task with
    w1 = `(in0<<3 & 0xff8) | 0x00800000 | (in0<<3 & 0x3ff000) | (in1<<20 & 0x400000)`;
  - else → the emit task below.
- else in1 & 4 → DOUTU the **pixel-event emit** task, which is
  `phas.i #0, #0 [wait=7]; limm r0, #0; emit.t0 r0, #0, r0 [end f3_20 imm=512]`.
- else halt.

[capture]. What each in1 bit is (end of tile, end of render, …), and
what in0 and in2 carry, is [guess]. `docs/PDS.md` has the instruction
encodings.

### PBE words

| word | ref.bin | field | tag |
|---|---|---|---|
| 0 | 00000000 | clip min `y0<<16 \| x0` | capture |
| 1 | 01df02cf | clip max `y1<<16 \| x1` (719, 479) | hardware |
| 2 | 016f8000 | b16-27 = (stride in pixels)/2 - 1 (367 → 736 px). The DDK pads a pbuffer's or FBO's pitch to a multiple of 8 pixels, at least 8, and a window's to 32: widths 1, 2, 3 → 8 (field 3), 16 → 16, 17 → 24, 33 → 40, FBO 64 → 64, FBO 65 → 72, window 720 → 736. So the field is always odd, and a pitch of 1 or 2 pixels is never encoded (2026-11-08 captures); b15 = set on every surface the 2026-11-08 captures render to: window, pbuffers and both FBOs, the back buffer *and* fb0 (the swap). With it cleared the PBE writes nothing at the address (replay, `docs/TEX.md` section 6), so it is not a scanout flag; b28-29 = rotation (0x10000000·k), b30-31 = downscale/AA (0x40000000·k) | hardware (stride: p6; b15 cleared: TEX.md section 6); capture (padding, b15 set); unconfirmed (b28-31) |
| 3 | 3e265000 | address, & ~3 (moved for rotated surfaces) | capture; hardware (open driver); unconfirmed (rotated surfaces) |
| side | 0x600 | b0-5 = PBE format code (0 BGRA8888, 1 RGBA8888, 3 RGB565, 4 RGBA5551, 5 RGBA4444, see `docs/TEX.md`); b9 = always; b10 = dither (GL_DITHER); b12 = set by the DDK under a condition not identified, never seen set; **b13 = twiddled destination** (set by the destination's memory layout; the transfer queue's texture uploads set it); for format codes 3-14 a per-surface value is added at b6-11 (0 in every capture) | capture (`dither0`, codes 0, 1, 3, 5); hardware (b13; codes 0, 1, 3); unconfirmed (code 4, b12, the b6-11 add) |

- The back buffer of a window is BGRA8888, top row first, with a pitch of
  720 rounded up to 32 = 736 px: 0x159000 bytes = 736 × 480 × 4.
- glReadPixels returns RGBA, bottom row first.

### Format codes

- Every surface EGL gave the probe was BGRA8888, PBE format code 0:
  windows, and pbuffers whether 5 6 5 or 8 8 8 8 was requested (`pb`,
  `pb8888`, `pbd`).
- So code 0 = BGRA8888 [capture]. The DDK's per-format table gives the
  other codes: 1 RGBA8888, 3 RGB565, 4 RGBA5551, 5 RGBA4444, 0x18 8-bit,
  0x1b LA88 [unconfirmed]. FBO render targets confirm 1, 3 and 5 (sideband
  0x2601, 0x2603, 0x2605: twiddled + dither) [capture], and RGBA8888 and
  RGB565 render-to-texture replays are pixel-exact (`docs/TEX.md` section 4)
  [hardware].

### Screen and tiles

- The TE tile is 32 × 16 pixels. The render target's tile counts are
  `xtiles = (w+31)/32` and `ytiles = (h+15)/16` [capture; hardware, open
  driver].
- **Region clip** (TA block group 0x80):
  - word 0 = `0x80000000 | xmin tile << 16 | xmax tile`;
  - word 1 = `ymin tile << 16 | ymax tile`;
  - the whole 720×480 target is 0..22, 0..29;
  - sticky TA state, see "Scissor and partial viewports" [hardware].
- glScissor is not a hardware rectangle; see "Scissor and partial
  viewports" in section 3.
- **Pbuffers** (`pb` 64×32, `pb2` 128×48): 2×2 macrotile mode
  (MTILE1 = MTILE2 = `01004004`, 16 tiles per macrotile), positive viewport
  y scale, and PBE stride = width rounded up to 32. All of it is
  reproduced by `build.py` [capture].

## 5. Texture state (DOUTT, 4 words per sample)

The pixel PDS program sends DOUTT right after the DOUTI that starts the
sample. Each word combines the texture's state with a part the sample
instruction sets, `(texture state & mask) | sample part` [unconfirmed as a
rule for all four words; T3 below is the case the captures show].

| word | field | tag |
|---|---|---|
| 0 | see the table below: filter (min, mag, mip), wrap S/T, format | capture; hardware (open driver) |
| 1 | b0-11 = height-1, b12-23 = width-1; b24-31: see the table below | capture; hardware (open driver) |
| 2 | texture base address (texture heap 0xf0000000; the render target for the background object) | capture; hardware (open driver) |
| 3 | see the table below | capture |

glTexParameter sets these T0 bits [capture: the `t*` captures below;
hardware, open driver]:

- **wrap S:** b6-8: 0 repeat, 1 mirror (0x40), 2 clamp (0x80);
- **wrap T:** b3-5: 0 repeat, 1 mirror (8), 2 clamp (0x10);
- **min filter:** NEAREST `0x1e0000`, LINEAR `0x1e0400`,
  N_MIP_N `0x1a0000`, L_MIP_N `0x1a0400`, N_MIP_L `0x1a0200`,
  L_MIP_L `0x1a0600`;
- **mag filter:** NEAREST 0, LINEAR `0x1000`.

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

**T0** (the wrap bits, the min filter bits and the mag filter bit, ORed
with a constant):

| bits | field | tag |
|---|---|---|
| 3-5 | wrap T: 0 repeat, 1 mirror, 2 clamp | hardware (mirror: p9); capture; hardware (open driver) |
| 6-8 | wrap S: 0 repeat, 1 mirror, 2 clamp | hardware (mirror: p9); capture; hardware (open driver) |
| 9 | mip filter linear | capture; hardware (open driver) |
| 10 | min filter linear | capture; hardware (open driver) |
| 12 | mag filter linear | hardware |
| 17-20 | 0xf without mipmaps, 0xd with (b18 = mipmapping off) | capture; hardware (open driver) |
| 21-26 | **LOD bias**, one 6-bit field v in 1/8 levels: bias = (v - 63)/8, so the DDK's 0x3f is no bias. Level sampled = floor(λ + bias + 0.5). On a CE5300 board, for 16- to 128-texel chains, the shift is the same at every size: v 63..59 → 0, 58..51 → -1, 47/43 → -2, 39/35 → -3, 19 → -5, 3 → -7. Not a clamp: GL's MAX_LEVEL/MAX_LOD need another mechanism. Whether a positive bias reaches into b27 is untested | hardware |
| 30 | set for L8, A8 and LA88 (channel replication?) | capture; hardware (open driver); guess (the meaning) |

**T1:**

| bits | field | tag |
|---|---|---|
| 0-11 | height - 1 | capture; hardware (open driver) |
| 12-23 | width - 1 | capture; hardware (open driver) |
| 24-28 | format: 0x02 RGBA4444 (stored ARGB4444), 0x04 RGBA5551 (stored ARGB1555), 0x05 RGB565, 0x0c BGRA8888 (render target), 0x0d RGBA8888 (RGB888 is uploaded as 8888), 0x19 A8, 0x1a L8, 0x1b LA88 | capture; hardware (open driver) |
| 29-31 | layout: 2 cube map (`cube`, T1 `4d00f00f`, 2026-11-02), 0 twiddled (power of two), 3 linear (NPOT; no stride field), 6 strided (T0 then holds the pitch: (pitch - 1) low 3 bits in b9-11, the rest in b17-26, `docs/TEX.md` section 3; counted in 32-bit words, the same as texels for 32-bit formats, see §5 background). Memory layouts 0 and 3 are in `docs/TEX.md` section 1 | capture; hardware (open driver) |

**T2:** the base address. The mip levels follow the base level; the chain
length is implied by the size, since no word changed with the level count.

**T3:** the shader's half of the word. It depends on the sample
instruction, not the texture: `tex1` gives `00800800` and `tex2`
`01028800` [capture]. The background object's load of the render target
(its pixel program `0x2e0060e0` in the base4 capture: DOUTU, one DOUTI
sampling tc0, then DOUTT) gives `00800000`, with T0 `00b60e90`, T1
`cc2cf1df` and T2 = the render target's address [capture]. What the 0x800
part that the shader samples add, and the background lacks, does is
[unconfirmed]. Three fields are known from open-stack runs [hardware;
TEX.md §14-15]: b29-31 the shadow compare function, b16-17 the compare
result format, and the low bits (b0-6 at least) a volume's depth - 1.

**The background's T0 and T1 follow the surface**, from the 2026-11-08
pbuffer and FBO captures:
- **T1** = `layout<<29 | format<<24 | (w-1)<<12 | (h-1)`. Window and pbuffer
  surfaces read `cc…` (layout 6 strided, format 0xc). A 64×64 RGBA8888
  FBO texture reads `0d03f03f` (layout 0 twiddled, format 0xd), and a 65×64
  RGB565 one `c504003f`.
- **T0**, strided: `0x90` plus the pitch as (pitch − 1), with the low 3 bits
  in b9-11 and the rest in b17-26:
  - 8 px → `00000e90`, 16 → `00020e90`, 24 → `00040e90`, 40 → `00080e90`,
    736 → `00b60e90`;
  - the 65-px RGB565 FBO (pitch 72 px = 144 bytes) gives `00080690` = 35,
    which is pitch − 1 in 32-bit words, not texels.

  So the pitch field counts 32-bit words; for the 32-bit surfaces the two
  are the same. One 16-bit data point. [capture]

## 6. HW render context, render details, parameter buffer

### HW render context (68 bytes, SGX_REGISTER_HW_RENDER_CONTEXT)

The client builds it. The kernel copies it, writes the PD physical address
at byte offset 8, and returns its device address (here 0x3000e000).

| word | ref.bin | field |
|---|---|---|
| 0 | 0x21 | flags: b0 = 1, b5 = per-context PB [capture; hardware, open driver (0x21); unconfirmed (b5's meaning)] |
| 1 | 0 | |
| 2 | 0 | page directory, filled by the kernel (`ui32OffsetToPDDevPAddr` = 8) [capture; hardware, open driver] |
| 3 | 3200f000 | CCB base (kdata, 0x10420 bytes: a 0x10000-byte ring and 0x420 bytes of room past its end, so a command that starts near the end runs on contiguously; the control words are word 4's block, not this) [capture; hardware, open driver] |
| 4 | 3200aa40 | CCB control: write offset, read offset (kdata, 8 bytes) [capture; hardware, open driver] (0x3200aa60 is a transfer context's) |
| 5 | 0 | commands in progress: the microkernel adds 1 when it takes a kick and subtracts 1 when it advances the read offset [capture: microkernel trace] |
| 6 | 3200e000 | PB descriptor (kdata, 0x3c) [capture; hardware, open driver] |
| 7 | 10462000 | 0x40-byte scratch block in the 3D params heap (the microkernel's per-context data) [capture; hardware, open driver] |
| 8-15 | 0 | |
| 16 | pid | creating process ID (replaying the captured one is harmless) [capture; hardware, open driver] |

**The command ring.** The client writes each command at the control's write
offset and then sets the write offset to `(woff + size) & 0xffff`, with size
the command's word 0 (0x2c0 for a TA command). The microkernel reads the
command at CCB base + read offset and advances the read offset by the same
size, `& 0xffff`. A command is never split at the ring's end: one that
starts near it runs into the 0x420 bytes of room. The transfer context's
ring has 0x4c8 bytes of room. [capture: microkernel trace; hardware, open
driver]

### PB descriptor (kdata, 0x3c)

The PB is allocated as the requested size + 0x22000 bytes in the 3D params
heap. Here that is 0x400000 + 0x22000 = 0x422000 bytes = 0x422 pages.

| off | ref.bin | field |
|---|---|---|
| 0x00 | 0 | flags: b1-4 = app-hint thresholds overridden [unconfirmed] |
| 0x04 | 0x422 | pages in the PB |
| 0x08 | 04210000 | free list head: `last page<<16 \| first page` |
| 0x0c | 04200000 | `(last page - 1)<<16` |
| 0x10 | 10422000 | free list address |
| 0x14 | 0 | 0 at kick 1; 0x19 after the first TA (pages in use?) [guess] |
| 0x18, 0x1c | | copies of 0x08, 0x0c |
| 0x28 | 0x432 | TA threshold = pages + 0x10 |
| 0x2c | 0x400 | ZLS threshold = pages - 0x22 |
| 0x30 | 0x400 | global threshold = pages - 0x22 |
| 0x34 | 0x300 | PDS threshold = min(ZLS, TA) - 0x100 |
| 0x38 | 3200e03c | PB block list (kdata, 0x10 bytes): {pages, first page \| last page<<16, PB descriptor address, 0} (the capture has 0x3200e000 there) |

All other rows [capture; hardware, open driver]: the formulas match the
captures, and the open driver writes them.

**Free list** (3D params heap, 0x40000 bytes): one u32 per
page, `prev<<16 | next`, indexed by page number from the heap base. Page 0 has prev 0xffff, and the last page has next
0xffff: `ffff0001 00000002 00010003 … 041f0421 0420ffff`. The client writes
it once, before the first kick. [capture; hardware, open driver]

### Render target

For w × h, no multisampling [capture: the probe's 720×480 window (mode 1)
and the pbuffers (mode 0); hardware, open driver]:

- `xt = (w+31)>>5` and `yt = (h+15)>>4`. For 720×480 that is 23 × 30.
- **Macrotile mode 0** (w·h < 0x4b000 pixels, a DDK threshold setting):
  2×2 macrotiles. The exact threshold is [unconfirmed].
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
  - [0] = the address of a 4-byte kdata word, the frame word. The client
    writes the frame index + 1 there before each scene. The microkernel never
    reads it [capture: microkernel trace]: it is the client's own record of
    which entry is next;
  - [1] = a completion counter the microkernel advances. +1 at the finish of
    a TA that does not end the scene, +1 at the end of a render whose render
    details +0xd4 has b10. The DDK trace shows 0, 1, 2 over a two-kick frame
    [capture]. Its starting value doesn't matter to the microkernel: the DDK
    client writes 1, the open clients and powervr_sgx write 0, and both
    render [hardware]. The DDK's debug output calls it "RT frames";
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

  All [capture; hardware, open driver]; the purposes of the 0x40, 0x50
  and 0x88 TA-data blocks are [guess].

- **Which entry renders.** The client names the entry in the TA command
  (+0x44, which the microkernel reads as render details +0xdc). It alternates
  by frame: the frame word's value & 1 picks the entry. Entry word 1 holds
  the render in flight on that entry; the render's end clears it and sets the
  entry's state word (+0x34) to `0x10 | (render details +0xd4 >> 14) << 12`
  [capture: microkernel trace]. If the alternation and the frame word
  disagree, the frames retire the wrong entry: their PB pages are never
  freed, and the 3D locks up on the fourth frame (2026-10-28) [hardware].
- **Renders in flight.** The microkernel tracks two renders in flight in
  the TA3D control block (+0x50 and +0x54; the kick picks a free slot, and
  the render's end clears its slot) [unconfirmed]. So one RT data set with
  its two entries holds two renders in flight. The open clients keep one set
  per render target they draw to back to back (the back buffer, then the
  swap's front buffer), so a render never waits on the other target's. What
  a third kick does while both entries have a render in flight is
  [unconfirmed].

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
- [capture; hardware, open driver]

### Background object block (3D params heap, 0x200 bytes)

The client fills it once per RT. The render details point at it as
`0x2000000 | (addr - PB base)>>4` (+0x48).

- [0-2] = 0 from the client. The microkernel writes them at every 3D start:
  [0] = render details +0x130 and [1] = +0x134 (the background object's PDS
  secondary and middle words, cmd+0x08 and +0x0c), and [2] = the word at
  render details +0x114 + 4 × (+0x108) (its PDS primary). [capture:
  microkernel trace]
- [3-8] = a 2-D rotation matrix (the identity, plus a per-target value, for
  no rotation) [capture (no rotation); unconfirmed (as a rotation)].
- [10] = 0x202.
- [15] = 0x100000.
- [16] = 0x1000, [17] = 0x3f80.
- [18] = `((w+0x1000)·0x100)<<8 | 0x10`, and [19] = 1.0.
- [20] = `(h+0x1000)·0x100`, [21] = 0x1000, [22] = 0x3f80.
- [10]-[22]: [capture; hardware, open driver].
- These are screen-space vertices in fixed point, offset by 0x1000 [guess].

### Render details (kdata, 0x1ec)

The client writes the first 0xd0 bytes.

| off | ref.bin | field | tag |
|---|---|---|---|
| 0x00 | 10000000 | 3D request base (PB base & 0xfff00000) | capture; hardware (open driver) |
| 0x04 | 0 / 3e500000 | ZLS request base: lowest of the depth/stencil load/store addresses & 0xfff00000 | capture; hardware (open driver) |
| 0x08 | 0x80 | a client value ORed with 0x80 | guess |
| 0x0c | 000f0000 | client values ORed | guess |
| 0x10 | 80000000 | b31 \| a render-target value | capture |
| 0x14 | 0 / 2 | depth bias units (glPolygonOffset units, rounded) | capture (`polyoff`); hardware (open driver) |
| 0x18 | 0 / 1.0 | depth bias factor (float) | capture (`polyoff`); hardware (open driver) |
| 0x1c | 0 | a client value | guess |
| 0x20 | 20006000 | region headers of this render | capture; hardware (open driver) |
| 0x24 | 0 | a client value | guess |
| 0x28 | 20003000 | region headers of the next render | capture; hardware (open driver) |
| 0x2c | 0 / 001710b4 | ZLS control: `0x111000 \| (xt-1)<<3`, ORed with bits of the drawable (mask `0xf80ee007`), windows only; FBOs get `0x140018`-style words with no store. store = b2 \| b17 \| b18 (float32 depth), load = b1 \| b13 \| b14 (the render after a readback split: `001770b6`; proven natively, with 3D flags b7 \| b8 and a read-write buffer), b19 negates the stored depth. b0/b1 with invalid stencil words lock up the 3D (`docs/TEX.md` section 7) | capture; hardware (store, load; the lockup); unconfirmed (b19, the drawable bits) |
| 0x30, 0x34 | | **depth** address - ZLS base, in both words (earlier read as depth/stencil store) | capture; hardware (open driver) |
| 0x38, 0x3c | 80000000 | b31 \| **stencil** address - ZLS base, in both words (earlier read as depth/stencil load) | capture; hardware (open driver) |
| 0x40 | 3f800000 | ISP background depth: the depth every tile starts from (1.0). glClearDepth does **not** change it: the clear writes depth through its own vertices' z, with depth write on in its ISPA (`cdepth`) | hardware |
| 0x44 | 0x300 | client values ORed | guess |
| 0x48 | 02046300 | background object: `0x2000000 \| (block - PB base)>>4` | capture; hardware (open driver) |
| 0x4c, 0x50 | 0, 0x88 | render-target values (0x88 = a render-target flag) | capture |
| 0x54 | 0 | 2 when the render context has a TA dependency | guess |
| 0x58 | 0x30 | constant | capture |
| 0x5c | 2e006000 | pixel event PDS program (end of tile) | capture; hardware (open driver) |
| 0x60 | 5 | its data rows | capture; hardware (open driver) |
| 0x64 | 1 | a client value | guess |
| 0x68 | 1 | a render-target flag | capture |
| 0x6c-0xc8 | 0 | client values (multisample / PBE extras) | guess |
| 0xcc | 0x100 | b8 follows a client kick flag | capture (0x100); unconfirmed (the flag) |
| 0x13c | 07c00000 | `n<<22`, n from the render context; 0x7c00000 by default | capture (0x7c00000); unconfirmed (other n) |

The words past +0xd0 are set at kick time, and the microkernel reads them:

| off | field | what the microkernel does with it | tag |
|---|---|---|---|
| 0xd0 | address of an 8-byte sync-heap word the client sets to 1 per scene | zeroed at the render's end, unless +0xd4 has b2 or b3 | capture |
| 0xd4 | copy of the TA command's 3D flags (cmd+0x1c), taken at every kick | b3 (0x8): the render completes on the 3D end alone, with no DPM-free rendezvous; the 3D start then writes 0x63c = 0 instead of 3 (UKERNEL.md §5). b9 (0x200): the 3D TQ write-complete is written at the end. b10 (0x400): RT data header [1] += 1 at the end. b14 and up: shifted into the RT data entry's state word | capture |
| 0xd8 | RT data address | header [1] counter (above) | capture |
| 0xdc | the RT data entry this render uses; must equal cmd+0x44 | its word 1 cleared, word 13 set to idle at the end; its word 4 (background block) filled at 3D start | capture |
| 0xe0 | copy of cmd+0x3c, the HW device sync list, taken at every kick | write-complete of entry (+0x108) set at the end | capture |
| 0xe4-0xf0 | copy of CMDTA_SHARED +0x1c..+0x28, the 3D TQ sync | read-complete (and with +0xd4 b9 write-complete) set at the end | capture |
| 0x104 | 1: the number of sync-list passes | the end advances +0x108 until it reaches this count | capture |
| 0x108 | the pass cursor, 0 at the first 3D start | indexes the sync list (+0xe0) and the +0x114 array; reset to 0 after the last pass | capture |
| 0x110 | address of an 8-byte kdata word | not read by the microkernel in the traces | guess |
| 0x114 | address of an array of words, one per pass, holding the background object's PDS primary word (`rows<<26 \| offset>>4`); the client rewrites it before each kick | copied into the background block word 2 at 3D start | capture |
| 0x124 | the TA state register 0x274, saved at each TA finish | restored into 0x274 at the next kick | capture |
| 0x130, 0x134 | the background object's PDS secondary and middle words | copied into background block words 0-1 at 3D start | capture |
| 0x140 | copy of cmd+0x20 (the frame index), taken at every kick | | capture |
| 0x144, 0x148.. | the 3D status-value count and its (address, value) pairs | each value written at the end | capture |

All from the vendor microkernel's memory trace over a generated frame
(`tools/ukernel/run.py`).

## Field layouts for packing

The words above as bit fields, for a packer (csbgen). Status:
- **K**: the field and its meaning are known (the section named has the evidence);
- **O**: only constant values have been seen; pack the value given;
- **U**: unknown; pack the value given, or 0 where the column says so.

K, O and U say what a packer can do with a field. What each rests on is
the tag in the section named.

Widths are only as wide as the evidence shows. "≤" marks an upper bound from
the neighboring fields, not a measured width. Observed values are from the
DDK captures on disk (36, `caps-ddk/` 2026-10-28 and 2026-11-08).

### VDM state-program words (§2)

**Block type** (every VDM word 0): b29-31 = 2 state program, 3 state program
of the terminate form, 4 draw, 6 end of stream (the nibbles 4, 6, 8 and c
read as b28-31). This is consistent with every word seen. b28 is the fan flag
in a draw, and 0 in every state-program word: their addresses, `addr>>4`,
end below b26 for the PDS heaps. So the type is not proven to stop at b29,
but nothing contradicts it [O].

State-program word 0: `type | (addr - heap)>>4` [K].

Info word:

| bits | field | status |
|---|---|---|
| 26-31 | PDS data rows (16 B each) | K |
| 0-? | a size in 4-dword units. It fits the dwords the state task *forwards*: 12 → 3, 15 → 4, 18 → 5, 2 (terminate) → 1, 0 (empty update) → 0 [O]. It is not the DMA's own count (the 25-dword triangle DMA gives 5). The rotating "counter 0-3" STATE.md §2 mentions has never been seen nonzero in the DDK's words. Its 8-draw capture emits the TA block once. vp5's three different TA blocks, at 0x3040e100/e180/e200, carry sizes 3, 3, 4 and nothing more. So whether a counter is ORed or added can't be told; the DDK packs 0 there. Width ≤ 8 bits (b8 is set in the other form) | U (rule fits) |
| rest, TA-block form | `0x2022400` (b10, b13, b17, b25) | O |
| rest, secondary-DMA form | `0xe100` (b8, b13-15). Seen: 10 dwords → 3, 27 dwords → 6, which doesn't fit ceil(n/4): the size rule for this form is U | O |
| type 6 (terminate) | `0x0e022401` | O |
| empty update | `0x0400e100` | O |

What b8, b10, b13-15, b17 and b25 mean is U.

### VDM draw (§2)

| word | bits | field | status |
|---|---|---|---|
| 0 | 31 | draw marker (1) | K |
| 0 | 26-27 | primitive: 0 triangles, 1 lines, 2 points, 3 strip | K |
| 0 | 28 | fan (with primitive 0) | K |
| 0 | 22-23 | 3 in every draw | O |
| 0 | 0-? | index count; ≤ 22 bits (b0-21); seen up to 192 | K (width U) |
| 1 | 0-31 | index buffer address; b0 clear for 16-bit, b0-1 for 32-bit indices | K (32-bit: unconfirmed) |
| 2 | 28-31 | batch − 1: 15 in every draw | O |
| 2 | 24 | 32-bit indices (no 32-bit capture) | K (unconfirmed) |
| 2 | 0-23 | index offset; 0 in every draw; width ≤ 24 bits | K (width U; meaning unconfirmed) |
| 3 | 0-21 | max index: `0x3fffff` in every draw; width ≥ 22 bits | O |
| 4 | 28-31 | vertices per USSE task − 1 (rule in §2) | K |
| 4 | 0-27 | vertex PDS program `addr>>4` | K |
| 5 | 0-6 | PDS data rows | K |
| 5 | 7-21 | ceil(input attribute bytes / 16) | K |
| 5 | 22-23 | 3 in every draw. Vertices per task − 1 is ≥ 7 in every capture, so "min(n, 3)" with n = vertices per task − 1 can't be told from a constant 3 | O |
| 5 | 24-31 | ceil(output dwords / 4) | K |

### TA state block (§3)

| word | bits | field | status |
|---|---|---|---|
| region clip 0 | 31 | set on every region clip seen; 0 on the scissor objects' "no clip" words | K (as enable: guess) |
| region clip 0 | 16-? / 0-? | xmin, xmax in 32-px tiles; seen up to 0x16; ≤ 15 bits each | K (width U) |
| region clip 1 | 16-? / 0-? | ymin, ymax in 16-px tiles; seen up to 0x1d | K (width U) |
| output select | 16-? | output dwords per vertex; seen up to 24; ≤ 16 bits | K (width U) |
| output select | 8 | the shader writes the point size | K |
| output select | 11-13 | `0x3000` (b12-13) on every draw, `0x1800` (b11-12) on the clear, 0 on scissor objects (`0x00040000`) | U: pack per kind |
| MTE control | 0-1 | cull (§3) | K |
| MTE control | rest | `0x01555620` on every draw (one ES1 capture: `0x15555620`); `0x40` on the clear | U: pack per kind |
| TSP word 0 | 3n..3n+2 | set n's size: 1, 2, 3, or 5 for 4 components; seen up to 5 sets. 10 sets fit in 30 bits; the hardware maximum is U | K |
| TSP word 1 | n | set n is F16 (mediump or lowp); one bit per set | K |
| PDS word 1 | 0-15 | primary attribute units of 4 dwords | K (width: the field ends at b15) |
| PDS word 1 | 16-17 | 3 in every capture | O |
| PDS word 1 | 18-24 | (secondary attribute dwords + 3)>>2; 7 bits | K |
| PDS word 1 | 25-28 | the rule in §3 "PDS words"; 4 bits. For one unit with no secondary attributes the DDK writes 12 (`18030001`; captures var1, var2, var2low and the one-sample shader). A winsys that writes 0xb there (`16030001`) works, but it is not the DDK's word | K (partly) |
| PDS word 1 | 29-31 | 0 in every capture | U (0) |

**ISPA / ISPB.** Values seen: ISPA `01d00200`, `03d00200`, `07e00100`,
`01d08200`, `01d00000`; ISPB `000010f0` and `000000f0`. Every set bit is in
the fields the §3 tables name. No untabled bit has been seen set, so pack
untabled bits as 0 [O].

**Header.** Values seen: `d4c3`, `df43`, `dfc3`, `d485`, `d583`, `d4c1`.
- The header names the groups that follow (§3).
- 0x2000 is clear in every header seen. Which group it would name is U;
  pack it as 0.

**TA terminate.** The DOUTA words `0x00002000` (ctrl 0) and `0x0016000e`
(ctrl 0x100, three times) are forwarded as pa0/pa1 by a 2-dword state emit
(§2). They are state words to the MTE, but their fields are U. Pack them as
constants.

### PBE (§4)

| word | bits | field | status |
|---|---|---|---|
| 0, 1 | 16-? / 0-? | clip y / x in pixels; seen up to 479 / 719; ≤ 16 bits each | K (width U) |
| 2 | 16-27 | pitch / 2 − 1 | K |
| 2 | 15 | set on every surface (name U; with it clear the PBE writes nothing, per the TEX.md §6 replay) | K (meaning: guess) |
| 2 | 28-29 | rotation, 0x10000000 × k (unconfirmed; only 0 captured) | K (values U) |
| 2 | 30-31 | downscale / AA, 0x40000000 × k (unconfirmed; only 0 captured) | K (values U) |
| 2 | 0-14 | 0 in every capture | U (0) |
| 3 | 0-31 | address & ~3 | K |
| sideband | 0-5 | PBE format code | K |
| sideband | 9 | always set | O |
| sideband | 10 | dither | K |
| sideband | 12 | set by a DDK condition not identified; never seen set | U |
| sideband | 13 | twiddled destination | K |
| sideband | 6-8, 11 | a per-surface value is added for format codes 3-14; 0 in every capture (codes 0 and 3 seen) | U (0) |

### Texture words (§5)

**T0**, beyond the §5 table:
- b17-26 holds the pitch for layout 6 (strided). The 2026-11-08 captures say
  the pitch is counted in 32-bit words; they agree with texels only for
  32-bit formats.
- For the other layouts:
  - b17-20 is 0xf without mipmaps and 0xd with them;
  - b21-26 is the LOD bias, 0x3f = none [hardware];
  - which of b17-20 is "mipmapping off" beyond b18 is U.
- b0-2, b11, b13-16 and b27-29 are 0 in every capture [U: 0].
- b30 is the L8/A8/LA88 bit.
- b31 is 0 in every capture [U: 0].

**T1 format codes 0x0c and 0x0d: which byte order each reads is U.**
The DDK's own uses:
- 0x0d for an RGBA8888 texture upload (`t8888`) and for sampling an
  RGBA8888 FBO that the PBE rendered with format code 0 (`fbo64`);
- 0x0c for the background's reload of the window back buffer, also written
  with PBE format code 0.

STATE.md's names ("0x0c BGRA8888 render target", "0x0d RGBA8888") come
from those uses, not from a measured byte order. The DDK may also swizzle
on upload. A one-change test would settle it: a `PROBE_TEXPAT=1` RGBA8888
texture sampled under each code, with the texture memory and the readback
compared.

**T3**: values seen:
- `00800800`: `tex1`;
- `01028800`: `tex2`;
- `00800000`: the background load.

The §5 text has T3 as the shader's half of the word, set by the sample
instruction. Its fields are U, so pack the value the sample form calls for.

## Captures

These were taken on a CE5300 board with `probe-st` (`tools/probe/probe.c`) under
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
| base3 | the probe defaults, second session (probe-st3, 2026-10-28 afternoon) | base for the rows below |
| sc1 / sc2 | glScissor(100,50,200,100) / (37,23,101,57) | load, reset, set objects; region clips |
| sc3 | glScissor of the whole target | nothing (= base3) |
| sc4 | sc1 without glClear | the draw carries the region clip (header `dfc3`) |
| sc5 | a second draw with glScissor(400,200,150,120) | reset over the old tiles, set the new |
| sc6 | glScissor(0,0,0,0) | reset only |
| nc0 | no glClear | the load object |
| vp1 / vp2 | glViewport(100,50,200,100) / (37,23,101,57) | reset + set over the viewport, viewport words |
| tri2 / fan | 4 vertices as GL_TRIANGLE_STRIP / GL_TRIANGLE_FAN | VDM prim 3 / prim 0 + b28 |
| ln1 / ln4 / ln9 | GL_LINES, glLineWidth 1 / 4 / 9.5 | VDM prim 1, ISPA type 1, ISPB width |
| lstrip / lloop | GL_LINE_STRIP / GL_LINE_LOOP | a prim-1 list: count 6 over identity indices (wrong) / count 8 over 0 1 1 2 2 3 3 0 |
| pt1 / pt8 / pt16 | GL_POINTS without gl_PointSize / 8.0 / 16.0 | nothing / VDM prim 2, ISPA type 3, size output |
| spr16 | 16-px points, gl_FragColor = gl_PointCoord | ISPA type 4, tc0 iteration |
| ptattr | gl_PointSize from an attribute | fminmax clamp against 511 |
| kb0 | glBlendFunc(ONE, ZERO), color 0.8 0.6 0.4 0.5 | sop2 c1=one c2=zero |
| k1-k5, k7, k8 | constant factors (src, dst, both), blend color 0.25 0.5 0.75 0.375 | 4 sopwm + sop2, blend color in sa2 |
| k6 | k1 with blend color 0.5 0.125 1 0.625 | sa2 only |
| e1base | probe1 (GLES1): clear + a triangle with vertex colors, alpha 0 / 0.5 / 1 | base for the e1 rows |
| e1gt5 / e1lt5 / e1gt25 / e1ge5 | glAlphaFunc GREATER 0.5 / LESS 0.5 / GREATER 0.25 / GEQUAL 0.5 | ISPA pass type 1, atst8, sa1 |
| e1nev / e1alw | NEVER / ALWAYS | no atst8: ISPA b20+b21 / the base |
| e1tex / e1texat | an RGBA texture, GL_MODULATE / + alpha test | sop2 modulate into r0, atst8 from r0 |
| e1atbl | alpha test + SRC_ALPHA blend | atst8 writes p0, (p0) blend |
| e1pt / e1spr / e1ln | ES1 points 8 / sprite 16 + texture / lines width 4 | as in ES2 |
| ln16 / ln106 | GL_LINES width 16 / 1.06 | ISPB `ff` / `10` (truncated) |
| sc7 | glScissor for the first draw, disabled for the second | one ISPC-less pass-type-3 object over the target |
| pt100 | gl_PointSize 100 | sa10 `5ffc5640` (F16 100.0 in the low half) |
| ptvar / sprvar | points with a varying vec2 / + gl_PointCoord | coordinate set tc0, varying tc1, size last |

Every capture's TA state block, PBE words, RT numbers, PB descriptor and
free list are rebuilt from the formulas by `build.py --check` (run with
each capture's GL state). They all match bit for bit: ref, d0, dless, s1,
drange, cullb, polyoff, dither0, cmask, b1, pb and pb2.

## Hardware proof (blob-free replays)

- **Setup:** `tools/decode/mkreplay.py <capture> --poke VA:word` on a CE5300 board
  with the static x86_64 `tools/replay/replay`, no DDK loaded,
  2026-10-28.
- **The check:** each result is compared with a prediction computed from
  the capture's own glReadPixels output.
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
| q0 | scissor (control) | none (`sc1`, glScissor(100, 50, 200, 100)) | identical | 0 differ |
| q1 | scissor, ISPC set/reset | reset object ISPC `00000000` → `02000000` (`0x30401088`) | the whole target visible: the triangle beyond the scissor, cut to the clear's tiles 3-9 × 20-26 (sticky region clip) | 0 differ |
| q2 | scissor, set geometry | set strip's left x 100.0 → 150.0 (`0x3e51d054`, `0x3e51d064`) | the scissor's left edge at 150 | 0 differ |
| q3 | scissor, region clip | set object region clip `80030009` → `80030006` (`0x3040110c`) | nothing right of x = 224 (tile 6's end) | 0 differ |
| q4 | scissor, ISPC set/reset | set object ISPC `02000000` → `00000000` (`0x30401108`) | nothing visible: an all-zero frame | 0 differ |
| l0 | lines (control) | none (`ln1`, two GL_LINES) | identical | 0 differ |
| l1 | ISPB width | `ln4` ISPB `000040f0` → `000010f0` (`0x30401088`) | the DDK's `ln1` frame | 0 differ |
| l2 | ISPB width | `ln1` ISPB → `000098f0` | the DDK's `ln9` frame (width 9.5) | 0 differ |
| l3 | VDM prim | `tri2` (strip) VDM word `8cc00004` → `84c00004` (lines, object type left 0) | the `ln1` frame | 720 differ: nothing drawn |
| l4 | ISPA object type | `ln1` ISPA `01d08200` → `01d00200` (type 0) | (discovery) | the lines vanish: type 1 is required |
| p0 | points (control) | none (`pt8`, gl_PointSize = 8.0) | identical | 0 differ |
| p1 | point size | sa10 `5ffc4800` → `5ffc4c00` (`0x30401108`) | the DDK's `pt16` frame | 0 differ |
| p2 | sprite size | `spr16` sa10 → `5ffc4800` | 8-px sprites, gl_PointCoord (i+0.5)/8 | 0 differ |
| p3 | sprite size | `spr16` sa10 → `5ffc5000` | 32-px sprites, (i+0.5)/32 | 0 differ |
| p4 | ISPA object type | `spr16` ISPA `01d20200` → `01d18200` (type 3) | (discovery) | same squares, coordinate 0: type 4 generates it |
| b0 | constant blend (control) | none (`k1`: CONSTANT_COLOR, ZERO; color 0.8 0.6 0.4 0.5, blend color 0.25 0.5 0.75 0.375) | identical | 0 differ |
| b2 | constant blend | k1's src-RGB sopwm `90807980` → `91807980` (c1 s2 → 1-s2, `0x370083a0`) | RGB of the DDK's `k2` frame, alpha of `k1` | 0 differ |
| b3 | constant blend | the same word → `908079c0` (s2a) | the DDK's `k3` frame | 0 differ |
| kk0 | k18/k19 (control) | none (`kb0`: ONE, ZERO) | identical | 0 differ |
| kk1-kk4 | sop2 selectors | `kb0`'s sop2 with c1=k18 / c2=k19 / a1=k18 / a2=k19 | (discovery) | k19 reads 0; k18 reads leftover per-pixel state |
| a0 | alpha test (control) | none (`e1gt5`: ES1, glAlphaFunc(GREATER, 0.5), vertex alpha 0, 0.5, 1) | identical | 0 differ |
| a1 | alpha reference | sa1 `80008e00` → `40008e00` (`0x2e018038`, PDS data) | the DDK's `e1gt25` frame | 0 differ |
| a2 | alpha func | sa1 → `80002e00` (LESS) | the DDK's `e1lt5` frame | 0 differ |
| a3 | alpha func | sa1 → `8000ce00` (GEQUAL) | the DDK's `e1ge5` frame | 0 differ |
| a7 | alpha func | sa1 → `8000ee00` (ALWAYS) | the DDK's `e1base` frame (no alpha test) | 0 differ |
| a4 | pass type | ISPA `03d00200` → `01d00200` (opaque) | (discovery) | failing pixels drawn with garbage |
| a5 | pass type | ISPA → `09d00200` (punch-through) | identical | 0 differ |
| a6 | atst8 h3 | `f934000d` → `f9340005` | (discovery) | render stalls, right part unwritten, no recovery |

p8 sets the min and mag bits together. The triangle samples a magnified
texture, so only mag is proven by the pixels; min rests on the captures
(and the open driver).

## Building a frame from scratch

Done in `tools/gen` (see `docs/GEN.md`): a frame built from these formulas
alone is pixel-exact on a CE5300 board. Two things it needed that this file didn't
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

## Gaps

Facts above tagged **unconfirmed**, each with the hardware test or capture
that would confirm it. Shipping code must not rely on an unconfirmed fact
until it is confirmed.

### §1 TA command

- Header cmd+0x14: the DDK can write `n<<27` instead of 0x50000000 under a render-context setting → a one-change probe capture per render-context creation option, looking for a cmd+0x14 other than 0x50000000.
- Header cmd+0x1c: 3D flags b0, b1, b3, b4 and b12 follow client kick flags (meanings unknown) → one-change probe captures (depth/stencil kept or discarded, readbacks, pbuffer vs window, multiple targets) to find what sets each bit, then a replay poking each bit and checking the pixels.
- TA flags: 0x60 for a middle kick → a capture of a scene the DDK splits into three or more kicks (many draws, or a PB-pressure probe).
- TA flags b0 pulses the TA bit of SOFT_RESET → a microkernel trace showing the SOFT_RESET writes on a first kick, or a replay of a two-frame stream with b0 cleared on frame 2's first kick, checking for leftover geometry.
- TA flags b4 is the USE code-cache invalidate (0xa08 = 1) → a replay that rewrites a shader's code in place between two frames, with b4 set and cleared on frame 2, checking which program's pixels appear.
- TA flags b5 invalidates the TA-side caches (0xad0, 0xae0, waits on 0x138) → a microkernel trace of a kick with b5 set showing those register writes, or a replay changing the TA state block in place between frames with b5 cleared.
- TA flags b6, past the register-sample edge: the 0x804 bit, the MADD cache invalidate and the USE data-cache flush → a replay that changes a texture's texels in place between two frames with b6 set and cleared, checking for stale texels.
- Tail +0x238: TE AA b31 = x2 multisample in X, b30 = in Y → a capture with a 4x multisample EGL config.
- Tail +0x24c: TA flags 2 b19, b23 and b25 → one-change captures with multisampling and with a render that reloads its depth, looking for each bit.
- Tail +0x2a0 is a render-target value << 12 → a capture in which it is nonzero (multisampling, or a larger target).
- Tail +0x2a4 takes values other than 0x88 (with AA) → a 4x multisample capture.

### §2 VDM control stream

- Draw word 1: bits 0-1 cleared for 32-bit indices; word 2 b24 (0x1000000) = 32-bit indices → a capture of glDrawElements with GL_UNSIGNED_INT (OES_element_index_uint), then a replay of it.
- Draw word 2 b0-23 is an index offset → a capture of draws at an index-buffer offset, or a replay poking a nonzero value and checking which vertices draw.
- TA-block info word: a 2-bit counter is ORed into the size → a capture in which the DDK's counter is nonzero (several different TA blocks in flight in one frame), or a replay poking b0-1 with a DMA'd block of a known size.

### §3 TA state block

- ISPA pass types 2 (translucent + discard) and 5 (depth-only) → a capture of a blended shader that discards, and of a stencil-only or depth-only draw, then a replay poking each pass type.
- PDS word 1 b25-28 with secondary attributes and more than one unit → a capture of a shader with uniforms and three or more mediump varyings.
- The color-mask move appended to the shader is `0x90b00601 | mask<<11` → compare the appended instruction words of the `cm1`-`cm3` captures with the expression.
- The Mesa driver's discard program does an alpha test once sa4 has another function and reference → a replay of a Mesa discard draw with sa4 poked to GREATER 0.5, checking against the DDK's `e1gt5` frame.
- glLineWidth clamps to [1, 16] → captures with glLineWidth(0.5) and glLineWidth(20).
- User clip planes: the `fdpc` result lands in output o8+n (or o7+n), and the TA clips lines as well as triangles against it → an open-stack frame with one `fdpc` into cp0 and output select `00093801`, drawing a triangle and a line across the plane, read back.
- What MTE control `000003a0` and VDM draw word 2 b26 (the other two flat-capture changes) do → a flat triangle strip and fan, replayed with each word reverted.
- A first-vertex provoking convention (EXT_provoking_vertex): no field known; neither of those two words selects it → a TA header or ISPB bit scan on the `flat` replay, or CPU index rotation.
- Polygon offset applies to object types 1, 3, 5 and 6 → a replay of `polyoff` with the draw's ISPA type set to 5, reading back the depth test result against a second draw.
- Object types 5 and 6: the line width and point size they draw with, and where edge flags go → replays of `es1base` with type 5 and ISPB width `40`, and with type 6 and a point-size output.

### §4 Render target

- PBE word 2 b28-29 = rotation, b30-31 = downscale/AA → a capture of a rotated or multisampled surface, then a replay poking each field.
- PBE word 3: the address moves for rotated surfaces → the same rotated-surface capture.
- PBE format code 4 = RGBA5551 → an RGBA5551 FBO capture and a render-to-texture replay of it.
- Sideband b12, and the per-surface value added at b6-11 for format codes 3-14 → captures of RGB565, RGBA5551 and RGBA4444 FBOs and of the transfer queue's uploads, looking for either set.
- PBE format codes 0x18 (8-bit) and 0x1b (LA88) → FBO captures in those formats, then render-to-texture replays.

### §5 Texture state

- Every DOUTT word is `(texture state & mask) | sample part` → a capture of one texture sampled by two different sample forms (a plain sample and a biased or projected one), diffing all four words.
- T3: what the 0x800 part does → a replay of the `t8888` draw with T3 `00800800` → `00800000`, checking the pixels.

### §6 HW render context, render details, parameter buffer

- HW render context word 0 b5 = per-context PB → a capture of two contexts sharing a PB, or a replay with b5 cleared.
- PB descriptor +0x00 b1-4 = app-hint thresholds overridden → a capture with each PB threshold hint set.
- Render target: macrotile mode 0 below 0x4b000 pixels → captures of targets just under and just over 0x4b000 pixels (e.g. 639×480 and 641×480 pbuffers).
- Renders in flight: the microkernel keeps two slots in the TA3D control block (+0x50, +0x54) → a microkernel trace with two renders in flight, showing both slots filled and cleared.
- A third kick while both RT data entries have a render in flight → a hardware run that kicks three frames on one target without waiting, checking for a lockup or a wait.
- Background object block [3-8] is a rotation matrix → a capture of a rotated surface.
- Render details +0x2c: b19 negates the stored depth, and the drawable bits under the mask `0xf80ee007` → a replay poking b19 and reading the stored depth buffer; captures of windows with different EGL configs.
- Render details +0xcc: b8 follows a client kick flag → one-change captures looking for +0xcc without b8.
- Render details +0x13c takes `n<<22` values other than 0x7c00000 → a one-change probe capture per render-context creation option.
