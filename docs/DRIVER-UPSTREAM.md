# The gallium driver and winsys: upstream readiness (2026-11-01)

This checks `mesa/src/gallium/drivers/sgx545` (the driver, 4.0k lines) and
`mesa/src/gallium/winsys/sgx545/drm` (the winsys, 3.6k lines) against AGENTS.md
§3.5 and §3.7, as `docs/COMPILER-UPSTREAM.md` does for the compiler. It covers
`um-re` as of the commit that adds this file.

For each piece of hardware knowledge the code uses, the tables give:

- which `docs/` section describes it;
- what the section rests on: `capture` (DDK captures and one-change diffs),
  `hardware` (our own frames, replays or pokes on an EA) or `[G]` (a captured
  value whose meaning is unknown);
- what has to happen before upstream:
  - **keep:** the doc covers it and the code follows the doc;
  - **doc gap:** the code uses something the doc doesn't say yet. Fill the
    doc, with a hardware test where noted;
  - **compiler:** hand-encoded instruction words that should come out of the
    compiler's encoder;
  - **not upstream:** DDK compatibility, or the services interface that M2/M3
    replace.

## 1. Licensing (§3.5)

- **Driver: clean.** It sees the winsys only through `sgxws.h`: GL-level
  state, the hardware's words as `docs/` describes them, and opaque handles.
  Its meson dependency (`idep_sgx545_ws`) carries no kernel include path and
  no kernel defines.
- **Winsys: not upstreamable as it is.**
  - `sgx545_frame.c`/`.h` include libpvr's `pvr.h`, whose `abi/` is generated
    from the GPL kernel headers.
  - The winsys lays out microkernel structures (`SGXMKIF_CMDTA`,
    `PVRSRV_SYNC_DATA`, `SGXMKIF_HWDEVICE_SYNC_LIST`) by those headers'
    offsets.
  - That is right for the overlay and wrong for Mesa. M3 replaces this layer
    (§4).
- **Build input from `tools/`.** `mesa/build.sh` generates
  `sgx545_usse_progs.h` with `tools/gen/mkusse.py`, which assembles through
  `tools/isa/hw.py`. Upstream, the fixed programs are assembled by the
  compiler's own encoder at build time (§5).
- **Headers.** Every file carries `Copyright © 2026 Joseph Telaak` and
  `SPDX-License-Identifier: MIT`. `mkusse.py` writes them into the generated
  header.

## 2. The driver (`drivers/sgx545`)

Most of the driver is Mesa plumbing with no hardware knowledge:

- screen and caps;
- resources and transfers;
- CPU vertex gathering and the vertex cache;
- index handling;
- the CPU blit path;
- fences;
- the debug-option table.

The hardware facts it holds:

| piece | where | docs/ | rests on | plan |
|---|---|---|---|---|
| texel layouts: twiddle (Morton, tiles of min(w,h,16)), linear NPOT pitch to 8 texels, mip chain | `sgx545_texture.c` | TEX.md §1 | capture (`PROBE_TEXPAT`, every texel placed by `check.py`) | keep |
| texture words T0/T1/T3: filters, wrap, format codes, layout, T0 b30 for L/A/LA, strided layout 6 | `sgx545_texture.c` | STATE.md §5 "Texture words, measured"; TEX.md §1, §11 | capture; T0 b21-26 = 0x3f [G], T3 0x00800800 | keep |
| RGBA8 sampled as format 0x0c (BGRA8888) | `sgx545_texture.c` | TEX.md §1 | hardware (byte-identical A/B) | doc gap: TEX.md should say why 0x0c is right for our compiler (it unpacks xyzw) |
| DOUTI words: iterate half (source, comps, b24, 0x6000000, F16 b28, last b29); sample half (set+1, 2-D 0x200 / projected 0x400, 0x12000 pure sample, last b30); a sample merged into the preceding plain iteration | `sgx545_program.c` | PDS.md "DOUTI" | capture, except projected (0x400): "captures only show 2-D" | doc gap: capture or hardware-test a projected sample (texture2DProj) |
| DOUTI source 0x22 (the discard's depth plane) | `sgx545_program.c` | PDS.md "DOUTI" ("not captured"); ISA.md "Phases, visibility feedback and discard" | capture (ISA.md's discard words) | doc gap: PDS.md should cite the discard capture and say what 0x22 is |
| discard: sa3 = 0xe000 \| depth compare \| 8 (no write); sa4 = 0xee00 | `sgx545_draw.c` | ISA.md "Phases, visibility feedback and discard" | capture (three values); sa4 [G] | doc gap: the sa3/sa4 table goes into ISA.md; hardware-poke sa4 |
| pass type (blend 1, discard 4, both 2) | `sgx545_draw.c` | ISA.md "The draw state", STATE.md §3 ISPA | capture | doc gap: STATE.md's ISPA table should list pass types 1, 2 and 4 |
| ISPA depth compare = the GL enum's low bits; every render starts at depth 1.0 | `sgx545_draw.c` | STATE.md §3 ISPA, §6 render details +0x40 | capture + hardware [C] | keep |
| constant formats: F32, F16 (round to nearest even), C10 (clamp(lrint(v·255)), ±2.0), our kinds 0x10/0x11 | `sgx545_draw.c` | ISA.md "Hardware checks" (hwt_p3, tests/c10); COMPILER-UPSTREAM §3 | hardware | keep (kinds 0x10/0x11 move into `sgx_const_map`) |
| what a compiled program needs (constants, iterations, samples, branches) | `sgx545_program.c` `read_program` | `sgx_compiler.h` `struct sgx_shader` | — | done (b2ef377): the driver reads `struct sgx_shader`; nothing writes or parses a USP in `mesa/` |
| dependent samples: the compiler's smp template (`sgx_sample_info.smp`) patched (dst l21-27, state in SA at l7-13, bank l30-31) + `wdf`, branch offsets moved | `sgx545_program.c` `insert_samples` | ISA.md "The texture-sample sequence" | hardware (A/B pixel-exact) | compiler: it emits the smp and wdf in place; the driver only passes the texture words |
| gl_FragCoord: the compiler's iteration (set + 1 = 14) has no DOUTI source we know, so the shader is refused | `sgx545_program.c` `read_program` | PDS.md "DOUTI" (sources: set + 1, 0x20 colour) | — | doc gap: capture the DDK's DOUTI for gl_FragCoord |
| gl_PointCoord: iterated as texture coordinate set 0 | `sgx545_program.c` | PDS.md "DOUTI" (w1 b4/b5 "coordinate replacement", not captured) | — | doc gap: capture a point-sprite draw |
| varyings: set = location − VAR0 on both sides | `sgx545_program.c`, `sgx545_screen.c` | — (our convention) | — | keep |
| render targets BGRA8888 only, rows padded to 32 px | `sgx545_screen.c`, `sgx545_resource.c` | STATE.md §4 "PBE words", "Format codes" | capture (code 0), FBO formats [C] | keep |
| "as the DDK does" comments (indices, uniform pins, ghosting, context sharing) | throughout | — | — | reword to cite the doc section and the test that fixed the behaviour |

## 3. The winsys: GPU command words

These are the words a Mesa driver over a new uAPI still writes. M3 moves them
from `sgx545_frame.c` into the driver proper, or into a `sgx545_cmd.c` next
to it.

| piece | where (`sgx545_frame.c`) | docs/ | rests on | plan |
|---|---|---|---|---|
| PDS instruction encodings: MOVS/DOUT, ALU, TST, BRA, SHL, HALT; row layout (DS0 DS0 DS1 DS1, code after) | `pds_*`, the `MOVS`/`ALU` macros, `pds_emit` | PDS.md "Program layout", "Instruction encoding" | capture (`dis.py` on 13 captures) + hardware (every program we run) | keep; better, describe the PDS ISA in isaspec as well and generate the encoder |
| DOUTU words: code base + offset, temporaries, b24/b25 (iterations/samples) | `doutu_w0`, `sgxws_draw` | PDS.md "DOUTU" | capture | keep |
| DOUTD control: dwords−1, register at b8, b25 secondary, b22-24 = (dwords−1)&3 per chunk, at most 16 dwords per DMA | `dma_ctrl`, `pds_dma_task`, `vdm_draw` | PDS.md "DOUTD" | capture (b22-24); hardware (the 16-dword limit, Minecraft's arm) | doc gap: PDS.md lacks the 16-dword limit and the b22-24 rule |
| pixel PDS program: DOUTU, a DOUTI per input, DOUTT after a sampling DOUTI, DS pair placement | `pds_pixel` | PDS.md "What the DDK generates" | capture | doc gap: PDS.md describes what the DDK generates; it should also state what a driver's program needs (inputs, order, what each DOUT takes) |
| VDM state program: `tst; bra; doutd; tst; bra; doutd; doutu` | `state_block` | PDS.md "What the DDK generates" | capture | doc gap + simplify: the tests branch on DS0 words that are never zero. Hardware-test a straight `doutd; doutu`, then document what the program must do |
| pixel-event program (29 instructions: event bits in1 & 8 / & 2 / & 4, DOUTA + EOT task, ptoff task) | `pixel_event` | PDS.md "pixel-event program decoded" | capture | doc gap: document the pixel-event inputs (which bit is which event, what in0/in2 carry) from captures and hardware pokes, then write the smallest program that handles them |
| TA terminate (VDM type 6): DOUTA (0x2000, 0), 3 × (0x16000e, 0x100), state-emit task | `sgxws_ctx_create` | PDS.md "DOUTA" ("unexplained"), STATE.md §2 | capture; meaning [G] | doc gap: hardware-test what the terminate needs (one DOUTA? none?) and document it |
| end-of-tile USSE program: LIMM r0-r3 = PBE words, EMITPIX ×2 | `eot_program`, `usse_limm` | STATE.md §4 "The end-of-tile program" | hardware [C] (clip max, stride) | doc gap: ISA.md has no `emitpix` form; describe it there. Compiler: emit both through the encoder |
| PBE words: clip min/max, stride/2−1, b15, address; sideband: format code, b9, dither | `eot_program` | STATE.md §4 "PBE words" | capture [D] + hardware [C] | keep; doc gap: b15's effect, from a hardware poke |
| background object: PDS (DOUTU copy_fs, DOUTI sample set 0 `0x12000`, DOUTT strided BGRA), its block in the 3D params heap (fixed-point vertices, `0x202`, `0x3f80`), `bg_pds[1] = 0x00030001` | `background`, `sgxws_rt_create` | STATE.md §6 "Background object block", TEX.md §11, PDS.md DOUTI examples | capture; 0x00030001 [G] | doc gap: the block's fields are listed without meanings; hardware-poke them |
| VDM words: state `0x4…`; draw `0x8…` + index address + `(batch−1)<<28` + `0x003fffff` + PDS word + info; end `0xc0000000` | `vdm_draw`, `state_block`, `vdm_end` | STATE.md §2, NOTES.md | capture | keep, apart from the next two rows |
| VDM state info word `rows<<26 \| (n + 0x2000) \| 0x2020400 \| (dwords+3)>>2` with a rotating index n | `state_block` | STATE.md §2, GEN.md | capture (values) | doc gap: hardware-test a constant n (is the rotation needed?) and name the fields |
| draw info word (output dwords, input size, vertices per task, PDS rows); vertices per task = 128 / output dwords, ≤ 16 | `vdm_draw`, `vertices_per_task` | STATE.md §2, GEN.md | capture (probe-st at 4-24 output dwords) + hardware (half that hangs) | doc gap: STATE.md §2 should carry the vertices-per-task table and the field layout |
| identity index buffer for glDrawArrays | `sgxws_ctx_create` | NOTES.md, STATE.md §2 | capture | keep |
| TA state block: header bits, ISPA (depth compare, write, pass type, b21 no colour), ISPB (colour mask, line width), ISPC stencil, region clip, PDS words, viewport, output select, MTE cull, W clamp, TSP sizes | `sgxws_clear`, `sgxws_draw`, `ispa`, `mte_ctrl` | STATE.md §3 | capture [D] + replay [C]; [G]: MTE 0x1555620/0x40, output select 0x3000/0x1800, PDS middle word bits | keep; each [G] is a hardware-poke candidate |
| PDS middle word: b18-24 secondary dwords, b0-15 by varyings | `sgxws_draw` | STATE.md §3 "PDS words" | capture | doc gap: the varyings table the code computes (EA3 captures with 0-5 vec4 varyings) |
| blend epilogue: `sop2 o0, pa<c>, o0` with GL's factors as selectors, swap for reverse subtract | `sgxws_blend_words` | STATE.md §3 "Blending and the colour mask are shader code"; ISA.md sop2 | capture + hardware (A/B pixel-exact) | compiler: the compiler emits the epilogue from ISA.md's sop2 section; the winsys hand-encodes no instruction words |
| colour mask: `sopwm o0, pa<c>, pa<c> [end mask=…]`, ISPB b4-7 | `wmask_words`, `clear_pds_masked` | STATE.md §3, ISA.md sopwm | capture + hardware | compiler, as the blend |
| discard program: phase 0 `phas`, `pcoeff`, shader, `atst8 [end]` with h10; phase 1 colour out + `phas [end]` | `sgxws_upload_fs` | ISA.md "Phases, visibility feedback and discard" | capture + hardware | compiler |
| vertex program wrapping: phase 0 (prologue), phase 1 (shader, `emit.t3`), epilogue phase (`emit.t1 … f3_20`) | `sgxws_upload_vs` (copies words out of `usse_vs_xy`) | ISA.md "Phases, visibility feedback and discard", "Vertex shaders" | capture + hardware (a missing epilogue hangs) | compiler |
| the prologue `nop; test.and.ne p0, #0, #0; (p0) ld; wdf` | `usse_progs.h`, every vertex/state task | — (mkusse.py: "kept because the DDK always emits it") | capture | doc gap: hardware-test without it; if it's needed, say why in ISA.md |
| fixed USSE programs: state-emit ×32, clear VS, copy FS, pixel-event tasks, the empty task | `sgx545_usse_progs.h` (generated) | GEN.md lists them; no program text in docs | capture + hardware | doc gap: the program text and purpose go into GEN.md or ISA.md; compiler: assembled at build time by the compiler's encoder |
| frame rings and placement (PDS 0x40, USSE 0x80, DMA 0x20) | `ring_*`, `*_ALIGN` | GEN.md ("what the hardware needs is untested") | capture | doc gap: hardware-test the minimum alignments |

## 4. The winsys: services and microkernel interface (not upstream)

This is what `pvrsrvkm` and its microkernel expect from a client. A DRM
driver with an open microkernel (M2) owns it, or does it differently. None of
it moves to Mesa as written. The kernel and microkernel work takes it from
STATE.md §1/§6 and UKERNEL.md.

| piece | where | docs/ | note |
|---|---|---|---|
| heaps, allocation attributes (0x3, 0xb, 0xf, 0x4b, 0x4f) | `sgxws_bo_alloc*`, `BO()` | — (services) | becomes GEM/gpuvm |
| CCB ring, `SGXMKIF_CMDTA` (TA flags 0x63/0x10, 3D flags, frame number, region headers, RT data entry, the per-kick words) | `ta_command`, `sgxws_flush_` | STATE.md §1 | becomes the uAPI's submit ioctl |
| render details (0x1ec; [G] +0x08/+0x0c/+0x44/+0x64/+0x104/+0x13c), RT data (2 entries, frame counter), region headers, tail pointers, RT numbers (macrotiles, TE screen) | `sgxws_rt_create`, `rt_numbers` | STATE.md §4 "Screen and tiles", §6 | the kernel or the new microkernel owns these; the RT numbers are a pure formula and can live in a shared header |
| parameter buffer: descriptor, block list, free list, thresholds | `ctx_pb` | STATE.md §6 "PB descriptor" | kernel side |
| HW render context (68 bytes) | `sgxws_ctx_create` | STATE.md §6 | kernel side |
| allocations that keep the DDK's layout: two transfer contexts, three render-details sets, kernel structures never freed | `sgxws_ctx_create`, `sgxws_rt_create`, `sgxws_rt_destroy` | GEN.md "Layout" | dropped with the open microkernel |
| sync objects polled on the CPU, the render-target sync, the HW device sync list | `token_*`, `sgxws_wait_sync` | TEX.md §5 | becomes drm_syncobj / dma-fence |
| `SGX_FLUSH_HW_RENDER_TARGET` not sent | `sgxws_rt_destroy` | GEN.md | kernel side |

## 5. What to do before upstream

1. **Fill the doc gaps above.** Each gets its experiment where the table
   says so. The pixel-event program comes first: the code runs the DDK's
   program as it is, and a driver should run one written from what the
   events need.
2. **Move every hand-encoded USSE word into the compiler.** The winsys
   writes instruction words in raw hex in five places: the blend, the colour
   mask, discard's `pcoeff`/`atst8`/`phas`, the EOT's `limm`/`emitpix`, and
   the vertex wrapper. With the compiler's isaspec encoder
   (COMPILER-UPSTREAM §4), these become compiler output, and the fixed
   programs become assembly text assembled at build time.
   `sgx545_usse_progs.h` and `mesa/build.sh`'s call into `tools/` go away.
3. **Dependent samples finished by the compiler.** The USP path is gone
   (b2ef377): the driver takes `struct sgx_shader`. What is left is the smp
   patching and `wdf` insertion in `insert_samples`, which the compiler can
   emit in place.
4. **The M3 driver.** It takes over §3's command-word layer and §2's driver
   facts, written from the docs and sitting on the new uAPI. `sgxws.h`'s
   shape (GL-level state in, words out) is a reasonable starting interface,
   and it contains no services material.
5. **Validation:**
   - the A/B suites (`mesa/tests`, pixel-exact against the DDK);
   - the gl4es/Minecraft bench;
   - dEQP-GLES2 under deqp-runner with the `ci/` expectation files (M0);
   - `tools/gen/cmpcap.py`-style structure diffs of a frame against a DDK
     capture.

## 6. Not covered

- `src/egl/drivers/dri2/platform_fbdev.c` is EGL plumbing with no hardware
  knowledge. Mesa has no fbdev platform, so it stays downstream.
- The tests (`mesa/tests`) and the dEQP rig: `run-deqp.sh ddk` runs the DDK
  for comparison. No shipping test depends on it.
