# sgxgen: SGX545 frames from code alone

`tools/gen` builds a whole SGX545 frame from GL-level parameters and submits it
through the native x86_64 services bridge. Nothing is read from a capture,
and no Imagination library is loaded. On a CE5300 board (2026-10-28) it draws the
probe's reference triangle **pixel for pixel**: the back buffer's md5 is
`56f8466a…`, the same as the blob's frame. Nine variations also match the
blob's own rendering of the same scene with 0 pixels different (**hardware**,
"Hardware results" below):

- colors and vertices;
- two draws;
- depth tests;
- a viewport;
- two other surface sizes.

| file | what |
|---|---|
| `sgxgen.h`, `sgxgen.c` | the generator: context, surfaces, shaders, state objects, clear, draw, flush |
| `mkusse.py` | the fixed USSE programs as assembly text, put through `tools/isa/hw.py`'s encoder into `usse_progs.h` at build time |
| `frame.c` | `frame`: a command-line scene (clear, triangles, depth, viewport, cull) → one frame, dumped |
| `fakepvr.c` | a stand-in for the kernel driver: `frame-fake` runs the generator on any x86_64 Linux and writes a trace, touching no GPU |
| `vary.py` | runs `frame` and the blob probe on the CE5300 board for each test case and compares them |
| `cmpcap.py` | compares a generated frame's trace with a DDK capture structure by structure, addresses mapped |
| `build.sh` | builds `frame` and `frame-fake` (static, musl, zig cc) |

`libpvr/pvr.c` (the bridge client both use) can now write a pvrtrace log
of its own run (`pvr_trace_open`). So `frame -t trace.bin` can be read by
`tools/state/decode.py`, `tools/pds/dis.py` and `tools/decode/mkreplay.py` like a
capture of the DDK. `mkreplay.py` now reads x86_64 traces.

## How each fact is established

Facts about what a generated word is, and what the hardware did with it, carry
one of these tags:

- **hardware**: a generated frame ran on the CE5300 board and its output was checked,
  by `vary.py` against the DDK's rendering of the same scene ("Hardware
  results"), or as the lockup or timeout the text describes. **open driver**
  means the open driver (`mesa/src/gallium/winsys/sgx/drm`) does the same
  thing and runs on the CE5300 board.
- **capture**: matches the DDK's output. `cmpcap.py` compares the reference
  frame with a DDK capture of the same scene, structure by structure, and
  every word is equal except address-bearing ones. So every word of the
  reference frame is **capture** as well as **hardware**. That backs the
  word's value; its meaning is only as good as STATE.md or PDS.md make it.
- **unconfirmed**: stated but not backed by hardware or a capture today. Each
  one is listed in "Gaps" at the end with the test that would confirm it.
- **guess**: a name or meaning with no other backing.

Tool behavior (options, traces, `frame-fake`) is not tagged.

## Running it

    tools/gen/build.sh
    scp tools/gen/frame ce5300:/tmp/gen/
    ce5300$ /tmp/gen/frame -o bb.bin                       # the reference triangle
    ce5300$ /tmp/gen/frame -k 0.25,0,0,1 -T -0.8,-0.6,0.2,-0.8,-0.3,0.7:1,0.5,0,1 -o bb.bin
    ce5300$ /tmp/gen/frame -d 0x201 -T x0,y0,x1,y1,x2,y2,-0.5:1,0,0,1 -T ...,0.5:0,0,1,1 -o bb.bin
    python3 tools/decode/cmpfb.py bb.bin ref.rgba          # BGRA top-first dump vs glReadPixels

The `frame` options:

- `-s WxH`: the surface size. Other sizes are **hardware** only as y-up
  surfaces (`-p`, the two `pb` cases); other sizes without `-p` are
  **unconfirmed**.
- `-p`: a y-up surface, laid out like an EGL pbuffer.
- `-k r,g,b,a`: the clear color.
- `-T verts[,z]:r,g,b,a`: a triangle list with a uniform color. With `z`,
  the positions are vec3.
- `-d func[,write]`: the depth test.
- `-V x,y,w,h`: the viewport.
- `-C face[,front]`: culling. No hardware case covers it yet (**unconfirmed**).
- `-t trace.bin`: write a trace of the run.
- `-o`: dump the render target.
- `-R N`: draw the frame N times on one context and surface. By default every
  render is the surface's first: RT-data entry 1, region array 1, frame
  counter 0. With `SGXGEN_ALTERNATE=1` the frames count and alternate the two
  entries and region arrays, as the DDK and powervr_sgx do (the scheme is
  **capture**, from the DDK's 3- and 6-frame captures, and **open driver**).
  Frame 2 then has TA flags 0x61, cmd+0x20 = 1, 3D flags 0x4400, cmd+0x44 =
  entry 0, region array 0, and the RT frame word 2 (**unconfirmed** for the
  generator's two-kick frame). This mode has run only on fakepvr so far. Repeats
  in the default mode have no hardware result either (**unconfirmed**).
- `-S` / `-X` (with `-R`): each repeat on a new surface, or on a new context
  and surface: one process standing in for separate `frame` runs.

`tools/gen/fake-traces.sh [OUT]` builds `frame-fake` for the host (Linux or
macOS) and writes the offline oracle traces to `gen/`: the one-frame
reference, and two- and three-frame runs in each of the modes above.

Checking a frame without the GPU:

    ce5300$ /tmp/gen/frame-fake -n /dev/null -t fake.bin   # any x86_64 Linux; no GPU touched
    python3 tools/state/decode.py fake.bin --brief         # every state word, annotated
    python3 tools/pds/dis.py fake.bin                      # every PDS program

The test suite (it needs a shell helper that runs commands on the CE5300 board):

    BOARD_SH=<ssh helper> python3 tools/gen/vary.py OUTDIR [case...]

## The API (what a gallium driver would sit on)

```c
struct sgx_dev dev;  sgx_dev_open(&dev, NULL, trace);              /* pipe_screen */
struct sgx_ctx *c = sgx_ctx_create(&dev);                          /* pipe_context */
struct sgx_surface *s = sgx_surface_create(c, w, h, flags);        /* pipe_surface + RT */
struct sgx_shader *vs = sgx_shader_builtin(c, SGX_VS_XY), *fs = ...;
struct sgx_state st = { .depth_func = SGX_LESS, .depth_write = 1 };   /* DSA + rasterizer + viewport */
sgx_set_framebuffer(c, s);
sgx_clear(c, rgba, 1.0f);
sgx_draw(c, &(struct sgx_draw){ .prim = SGX_TRIANGLES, .count = 3, .verts = v,
                                .vs = vs, .fs = fs, .state = &st, .color = {0, 1, 0, 1} });
sgx_flush(c, 2000);                                                /* two TA kicks + wait */
sgx_surface_destroy(c, s); sgx_ctx_destroy(c);
```

Shaders are the built-in programs for now: an xy or xyz position shader, and
a fragment shader that writes a mediump uniform `vec4`. Shader objects carry
what the PDS programs need:

- the code addresses;
- the input attribute dwords;
- the output dwords;
- the secondary (uniform) dwords and where the uniform sits.

A compiler backend would fill the same struct.

## What a frame is made of, and where each word comes from

Everything in this section is in the reference frame, so its words are
**capture** and **hardware** unless a line says otherwise.

`sgx_ctx_create` builds:

- **the PB:** the PB descriptor and block list, then the 0x422000-byte PB and
  its free list (STATE.md 6);
- **the submission path:** the CCB and its control words, and the HW render
  context. The context's 68 bytes are registered with
  `SGX_REGISTER_HW_RENDER_CONTEXT`;
- **five rings:** USSE pixel, USSE vertex, PDS pixel, PDS vertex and general.
  The VDM stream gets a sixth;
- **the fixed programs:**
  - the empty task;
  - the clear's vertex and pixel programs;
  - the background object's pixel program;
  - the two pixel-event tasks;
  - the state-emit program for each TA-block length;
  - the TA terminate (VDM type 6) and the empty secondary update;
  - a 1024-entry identity index buffer.

`sgx_surface_create` builds:

- the BGRA8888 color buffer;
- the RT numbers (macrotiles, TE screen);
- the RT data with its two entries;
- the tail pointers;
- two region-header arrays;
- the three TA-data blocks;
- the background-object block;
- three render-details sets (the third renders);
- the end-of-tile program (LIMM of the four PBE words, then two EMITPIX);
- the pixel-event PDS program;
- the background object's PDS programs and texture words.

`sgx_clear` and `sgx_draw` record into the VDM stream.

- **A clear** is a TA state block (12 dwords) behind a VDM state program,
  then a 3-vertex draw at (0,0), (2w,0), (0,2h) in screen space.
- **A draw** writes:
  - its vertices;
  - the fragment uniforms (FP16) and the secondary PDS program that DMAs them;
  - the primary pixel PDS program;
  - an 18-word TA state block (ISPA/ISPB/PDS/viewport/output select/MTE/TSP);
  - the vertex secondary attributes and their state program;
  - the vertex-fetch PDS program;
  - the six-word VDM draw.

`sgx_flush` closes both kicks' streams (empty secondary, TA terminate, VDM
terminate). It writes both TA commands into the CCB and kicks twice, the
second with `bLastInScene`. Then it polls the color buffer's sync object.

Every word comes from one of these sources:

| source | covers | evidence |
|---|---|---|
| `docs/STATE.md` (formulas also in `build.py`) | TA command header and tail, render details, RT numbers, RT data, background block, PB descriptor, free list, TA state block words, VDM draw and state words, PBE words, texture words of the background load | **capture**, **hardware** |
| `docs/STATE.md` / `docs/PDS.md` tables | the VDM state info word `rows<<26 \| (n + 0x2000) \| 0x2020400 \| (dwords+3)>>2` with the DDK's rotating index n; the draw info word | **capture**, **hardware** |
| `docs/PDS.md` | every PDS program: data rows, MOVS/ALU/TST/BRA/SHL encodings, DOUTU/DOUTD/DOUTI/DOUTT words. The pixel-event program is written out from its decoding | **capture**, **hardware** |
| `docs/ISA.md` via `hw.py` | every USSE program, from assembly text in `mkusse.py`; the end-of-tile LIMMs use STATE.md's LIMM formula at run time | **capture**, **hardware** |
| [G] constants | the values the DDK uses whose meaning isn't known: the PDS middle word's b0/b16-17/b25-28, MTE ctrl 0x1555620 / 0x40, output-select flags 0x3000 / 0x1800, the background T0 low bits 0x20e90 and T3, TA flags 0x71/0x62, render details +0x08/+0x0c/+0x44/+0x64/+0x104/+0x13c, the TA terminate's DOUTA values, DOUTD b22-24 (3 clear, 0 draw, 1 vertex secondary, 2 uniforms), vertices per task (64 / output dwords, at most 16) | values **capture**, **hardware**; meanings unknown. The vertices-per-task rule beyond the reference frame's output count is **unconfirmed** |

### Inventory: the replay against the generator

`mkreplay.py` writes 26,840 bytes of the capture before kick 1, into 117
buffers. The generator covers what the frame reads (**capture**):

| replay copied | generator |
|---|---|
| PB descriptor, block list, free list | `ctx_pb` (formulas; block list word 2 is the PB *descriptor* address, not the PB) |
| CCB command header + tail, CCB control | `ta_command`, `kick` |
| render details (third set), its sync word, kdata words, HW device sync list + access word | `sgx_surface_create` |
| RT data, frame-counter word, entry words | `sgx_surface_create` |
| background block (3D params) | `background` |
| VDM stream, both kicks | `state_block`, `vdm_draw`, `vdm_end` |
| TA state blocks (pdsvertex 0x30401000 ring) | `sgx_clear`, `sgx_draw` |
| VDM state programs (400-slot static buffer at 0x3040e000) | one instance per use (`state_block`) |
| vertex-fetch and secondary state programs | `vdm_draw`, `pds_secondary` |
| TA terminate, empty secondary update (0x30400000/60) | `sgx_ctx_create` |
| pixel PDS programs (pixel event, background, clear, uniform DMA, primary) | `pixel_event`, `background`, `pds_pixel`, `pds_secondary` |
| uniforms (FP16), vertex secondary constants | `sgx_draw` |
| USSE: state emit ×3, clear VS, triangle VS, empty ×2, copy, fragment pair, pixel-event tasks, end of tile | `mkusse.py`, `eot_program` |
| vertex and index buffers | `sgx_clear`, `sgx_draw`, identity buffer |
| the driver's other programs: 70 small heap-4 buffers, 32-slot background programs at 0x2e002000/0x2e005000, texture-heap words, transfer contexts | not needed: nothing in the frame references them (**capture**: `cmpcap.py` follows every pointer; **hardware**: frames run without them) |

`cmpcap.py ref.bin gen.bin` compares a generated trace with the capture,
structure by structure: it follows the same pointers in both and maps the
addresses. Every word is equal except address-bearing ones.

## Hardware results (2026-10-28)

Every row is **hardware**. `vary.py`'s cases are each checked against the DDK
probe (`probe-st`) on the same box. Each triangle is drawn by the blob on its
own. The prediction composes those images in the case's priority order, so the
overlap rule (last draw wins, or the depth test) is what is being tested:

| case | frame | pixels differing |
|---|---|---|
| ref | the probe's triangle, clear (0,0,0.5,1) | 0 of 345600 (md5 `56f8466a…`) |
| colors | other vertices, clear (0.25,0,0,1), color (1,0.5,0,1) | 0 |
| two | red then blue, overlapping, no depth test: blue covers | 0 |
| depth | LESS, near red drawn first, far blue second: red keeps the overlap | 0 |
| depth-far-first | LESS, far blue first, near red second | 0 |
| depth-nowrite | LESS with depth writes off: blue passes against 1.0 and covers | 0 |
| depth-greater | GREATER against the background depth 1.0: nothing drawn | 0 |
| viewport | glViewport(100, 50, 400, 300) | 0 |
| pb128x48 | 128×48 y-up surface (2×2 macrotiles), two draws | 0 of 6144 |
| pb320x200 | 320×200 y-up surface, depth pair | 0 of 64000 |

A depth test needs no depth buffer here (**hardware**: the depth cases). The
ISP holds depth per tile and starts each tile from render details +0x40 (1.0).
The value 1.0 is **capture** and **open driver**; that +0x40 is where each
tile's start depth comes from is **unconfirmed**. Without ZLS nothing is
loaded or stored.

## Things learned on the way

### Vertex programs end with an extra phase

A finalized vertex program is two `phas` phases plus a last one (**hardware**,
**open driver**):

    phas.i #0, #0 [wait=7]
    emit.t1 r0, #0, #0 [end f12 f10 f3_20]

The phase before it ends with `emit.t3` (the vertex's outputs), and its
`phas.i` points here. The first generated frame left this epilogue out (the
DDK's word count stopped at the first `end`). The vertex task then ran into
whatever followed it, and the TA locked up (**hardware**). The clear's vertex
program has the same epilogue (**capture**).

### Layout: the microkernel reads the previous client's render

After a client's frame, the microkernel's TA/3D control (dumped on SGX
recovery as `T3C-60..7C`) still holds pointers to that client's RT data
entry and render details (**hardware**: seen in the recovery dump). That it
reads them when the next client's TA starts is **unconfirmed**: it is what
the hangs below point to.

- **Two clients with the same kernel-data layout work.** That covers any DDK
  client, and a replay of a DDK capture (**hardware**).
- **A different layout hangs the TA.** The reads land on whatever the new
  client put at those addresses. The first two generated frames, each run
  after a replay, locked up (`lockup_ta`). Every word was right. The same
  frame on a fresh boot was pixel-exact (**hardware**).
- **The fix is the DDK's layout.** `sgx_ctx_create` and `sgx_surface_create`
  allocate the kernel-data and sync heaps in the DDK's exact order:
  - dummies for its two transfer contexts;
  - three render-details sets.

  So every structure lands at the DDK's address (render details 0x3200e560,
  RT data 0x3200aac0, and the rest; **capture**). Replay → frame and frame →
  replay are both proven since (**hardware**, **open driver**).
- **The DDK's own cleanup doesn't work here.** It sends
  `SGX_FLUSH_HW_RENDER_TARGET` (cleanup type RT) when it removes a render
  target. On this srvinit-started board that request timed out ("Wait for
  uKernel to clean up (1) failed"), and the GPU locked up right after
  (**hardware**). It is off by default (`SGXGEN_FLUSH_RT=1` sends it, as does
  `mkreplay.py --flushrt`). Whether the blob-initialized microkernel handles
  it was not tested (**unconfirmed**).
- **What a Mesa driver needs.** It must either reproduce this layout, or find
  the cleanup the microkernel accepts. Two different native clients would hit
  the same problem (**unconfirmed**).

### Smaller things

- **Block list.** Its word 2 is the PB descriptor's address (STATE.md said the
  PB's) (**capture**, **open driver**).
- **CCB control.** Render context word 4 is 0x3200aa40, the write/read
  offsets; 0x3200aa60 belongs to a transfer context (**capture**).
- **Free list.** The DDK allocates 0x40000 bytes, one u32 per page of the
  whole 3D-params heap; sgxgen does the same (**capture**, **open driver**).
- **The DMA constants.** The vertex secondary constants are 2/255, 1/255,
  2/65535 and 1/65535: the first frame had 1/32767 for the third (**capture**,
  **open driver**).
- **Placement.** Every PDS program the DDK makes starts on 32 bytes, every
  USSE program GL allocates on 128, every DMA source on 32 (**capture**).
  sgxgen does the same (`PDS_ALIGN`, `USSE_ALIGN`, `DMA_ALIGN`). Whether the
  hardware needs it was not tested (**unconfirmed**): the alignment change alone did not fix the
  TA hang.
- **Rings start 0x100 in.** That keeps a PDS state word or DOUTU word from
  ever being 0 (**open driver** does the same). That a zero word would
  misbehave is **unconfirmed**.

## Open

- Shaders are fixed programs. Next: a compiler (the NIR backend session)
  fills `sgx_shader`.
- Not generated yet:
  - textures;
  - blending (shader epilogue + pass type 1);
  - scissor and partial-viewport objects (pass type 3, region clip; geometry
    must stay inside clip space without them);
  - stencil;
  - depth/stencil buffers in memory (ZLS);
  - more draws than the rings hold.
- The [G] constants above.
- The RT cleanup timeout (see "Layout").
- The kick's TA/3D status updates are left out. The DDK uses them for its
  ring-buffer bookkeeping (**guess**), and the frame doesn't need them
  (**hardware**).

## Gaps

Facts above tagged **unconfirmed**, each with the hardware test or capture
that would confirm it. Shipping code must not rely on an unconfirmed fact
until it is confirmed.

- `-s WxH` without `-p`: surfaces of other sizes that are not y-up render correctly → a `vary.py` case with `-s` at two or more sizes (one under 0x4b000 pixels) against the probe at the same window size.
- `-C face[,front]`: culling (0x404 FRONT, 0x405 BACK; front 0x900 CW, 0x901 CCW) culls the right triangles → `vary.py` cases with a CW and a CCW triangle under each face and front setting, against the probe with the same glCullFace / glFrontFace.
- `-R N` in the default mode (every render the surface's first: entry 1, region array 1, frame counter 0) renders every repeat correctly → `frame -R 6 -o` on the CE5300 board, each frame compared with the reference, and the PB checked for leaked pages.
- `SGXGEN_ALTERNATE=1`: frame 2 of the generator's two-kick frame has TA flags 0x61, cmd+0x20 = 1, 3D flags 0x4400, cmd+0x44 = entry 0, region array 0 and RT frame word 2, and alternating frames run → `SGXGEN_ALTERNATE=1 frame -R 6` on the CE5300 board, each frame compared with the reference; or a DDK capture of several two-kick frames on one surface, compared with `cmpcap.py`.
- Vertices per task is 64 / output dwords, at most 16, for any output count → a DDK capture of vertex programs with more outputs (one and several varyings), comparing the vertex-fetch PDS program's task size.
- The ISP starts each tile's depth from render details +0x40 → a generated frame with +0x40 set to 0.5 and the depth-greater and depth cases, checked against the probe drawn with glClearDepth(0.5).
- The microkernel reads the previous client's RT data entry and render-details pointers (`T3C-60..7C`) when the next client's TA starts → a generated frame after a replay with only the RT data and render details moved (everything else in the DDK's layout), and one with everything moved except those: only the first should hang.
- Two different native clients with different kernel-data layouts hang the same way → two generated clients with different allocation orders run back to back on the CE5300 board, without a reboot between them.
- The blob-initialized microkernel accepts `SGX_FLUSH_HW_RENDER_TARGET` → `SGXGEN_FLUSH_RT=1 frame` on a board whose microkernel the DDK's own srvinit started, then a second frame.
- The hardware needs PDS programs on 32 bytes, USSE programs on 128 and DMA sources on 32 → generated frames with each alignment lowered one at a time (for example PDS on 16, USSE on 64, DMA on 16), compared with the reference.
- A PDS state word or DOUTU word of 0 misbehaves → a generated frame with rings starting at 0, so the first program sits at offset 0, compared with the reference.
