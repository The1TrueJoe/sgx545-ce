# sgxgen: SGX545 frames from code alone

`re/gen` builds a whole SGX545 frame from GL-level parameters and submits it
through the native x86_64 services bridge. Nothing is read from a capture,
and no Imagination library is loaded. On the EA3 (2026-10-28) it draws the
probe's reference triangle **pixel for pixel**: the back buffer's md5 is
`56f8466a…`, the same as the blob's frame. Nine variations also match the
blob's own rendering of the same scene with 0 pixels different:

- colours and vertices;
- two draws;
- depth tests;
- a viewport;
- two other surface sizes.

| file | what |
|---|---|
| `sgxgen.h`, `sgxgen.c` | the generator: context, surfaces, shaders, state objects, clear, draw, flush |
| `mkusse.py` | the fixed USSE programs as assembly text, put through `re/isa/hw.py`'s encoder into `usse_progs.h` at build time |
| `frame.c` | `frame`: a command-line scene (clear, triangles, depth, viewport, cull) → one frame, dumped |
| `fakepvr.c` | a stand-in for the kernel driver: `frame-fake` runs the generator on any x86_64 Linux and writes a trace, touching no GPU |
| `vary.py` | runs `frame` and the blob probe on the EA3 for each test case and compares them |
| `cmpcap.py` | compares a generated frame's trace with a DDK capture structure by structure, addresses mapped |
| `build.sh` | builds `frame` and `frame-fake` (static, musl, zig cc) |

`re/replay/pvr.c` (the bridge client both use) can now write a pvrtrace log
of its own run (`pvr_trace_open`). So `frame -t trace.bin` can be read by
`re/state/decode.py`, `re/pds/dis.py` and `re/decode/mkreplay.py` like a
capture of the DDK. `mkreplay.py` now reads x86_64 traces.

## Running it

    re/gen/build.sh
    scp re/gen/frame ea3:/tmp/gen/
    ea3$ /tmp/gen/frame -o bb.bin                       # the reference triangle
    ea3$ /tmp/gen/frame -k 0.25,0,0,1 -T -0.8,-0.6,0.2,-0.8,-0.3,0.7:1,0.5,0,1 -o bb.bin
    ea3$ /tmp/gen/frame -d 0x201 -T x0,y0,x1,y1,x2,y2,-0.5:1,0,0,1 -T ...,0.5:0,0,1,1 -o bb.bin
    python3 re/decode/cmpfb.py bb.bin ref.rgba          # BGRA top-first dump vs glReadPixels

The `frame` options:

- `-s WxH`: the surface size.
- `-p`: a y-up surface, laid out like an EGL pbuffer.
- `-k r,g,b,a`: the clear colour.
- `-T verts[,z]:r,g,b,a`: a triangle list with a uniform colour. With `z`,
  the positions are vec3.
- `-d func[,write]`: the depth test.
- `-V x,y,w,h`: the viewport.
- `-C face[,front]`: culling.
- `-t trace.bin`: write a trace of the run.
- `-o`: dump the render target.

Checking a frame without the GPU:

    ea3$ /tmp/gen/frame-fake -n /dev/null -t fake.bin   # any x86_64 Linux; no GPU touched
    python3 re/state/decode.py fake.bin --brief         # every state word, annotated
    python3 re/pds/dis.py fake.bin                      # every PDS program

The test suite (it needs a shell helper that runs commands on the EA3):

    EA3SH=<ssh helper> python3 re/gen/vary.py OUTDIR [case...]

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

`sgx_ctx_create` builds:

- **the PB:** the PB descriptor and block list, then the 0x422000-byte PB and
  its free list (STATE.md 6, from `FUN_0001e980`);
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

- the BGRA8888 colour buffer;
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
second with `bLastInScene`. Then it polls the colour buffer's sync object.

Every word comes from one of these sources:

| source | covers |
|---|---|
| `re/state/STATE.md` (formulas also in `build.py`) | TA command header and tail, render details, RT numbers, RT data, background block, PB descriptor, free list, TA state block words, VDM draw and state words, PBE words, texture words of the background load |
| `re/pds/PDS.md` | every PDS program: data rows, MOVS/ALU/TST/BRA/SHL encodings, DOUTU/DOUTD/DOUTI/DOUTT words. The pixel-event program is written out from its decoding |
| `re/isa/ISA.md` via `hw.py` | every USSE program, from assembly text in `mkusse.py`; the end-of-tile LIMMs use STATE.md's LIMM formula at run time |
| libGLESv2 decompiles (STATE.md / PDS.md tables) | the VDM state info word `rows<<26 \| (n + 0x2000) \| 0x2020400 \| (dwords+3)>>2` with the DDK's rotating index n (`FUN_000497f0`); the draw info word (`FUN_00017c20`) |
| [G] constants | the values the DDK uses whose meaning isn't known: the PDS middle word's b0/b16-17/b25-28, MTE ctrl 0x1555620 / 0x40, output-select flags 0x3000 / 0x1800, the background T0 low bits 0x20e90 and T3, TA flags 0x71/0x62, render details +0x08/+0x0c/+0x44/+0x64/+0x104/+0x13c, the TA terminate's DOUTA values, DOUTD b22-24 (3 clear, 0 draw, 1 vertex secondary, 2 uniforms), vertices per task (64 / output dwords, at most 16) |

### Inventory: the replay against the generator

`mkreplay.py` writes 26,840 bytes of the capture before kick 1, into 117
buffers. The generator covers what the frame reads:

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
| the driver's other programs: 70 small heap-4 buffers, 32-slot background programs at 0x2e002000/0x2e005000, texture-heap words, transfer contexts | not needed: nothing in the frame references them |

`cmpcap.py ref.bin gen.bin` compares a generated trace with the capture,
structure by structure: it follows the same pointers in both and maps the
addresses. Every word is equal except address-bearing ones.

## Hardware results (EA3, 2026-10-28)

`vary.py`'s cases are each checked against the DDK probe (`probe-st`) on the
same box. Each triangle is drawn by the blob on its own. The prediction
composes those images in the case's priority order, so the overlap rule
(last draw wins, or the depth test) is what is being tested:

| case | frame | pixels differing |
|---|---|---|
| ref | the probe's triangle, clear (0,0,0.5,1) | 0 of 345600 (md5 `56f8466a…`) |
| colors | other vertices, clear (0.25,0,0,1), colour (1,0.5,0,1) | 0 |
| two | red then blue, overlapping, no depth test: blue covers | 0 |
| depth | LESS, near red drawn first, far blue second: red keeps the overlap | 0 |
| depth-far-first | LESS, far blue first, near red second | 0 |
| depth-nowrite | LESS with depth writes off: blue passes against 1.0 and covers | 0 |
| depth-greater | GREATER against the background depth 1.0: nothing drawn | 0 |
| viewport | glViewport(100, 50, 400, 300) | 0 |
| pb128x48 | 128×48 y-up surface (2×2 macrotiles), two draws | 0 of 6144 |
| pb320x200 | 320×200 y-up surface, depth pair | 0 of 64000 |

A depth test needs no depth buffer here. The ISP holds depth per tile and
starts each tile from render details +0x40 (1.0). Without ZLS nothing is
loaded or stored.

## Things learned on the way

### Vertex programs end with an extra phase

A finalized vertex program is two `phas` phases plus a last one:

    phas.i #0, #0 [wait=7]
    emit.t1 r0, #0, #0 [end f12 f10 f3_20]

The phase before it ends with `emit.t3` (the vertex's outputs), and its
`phas.i` points here. The first generated frame left this epilogue out (the
DDK's word count stopped at the first `end`). The vertex task then ran into
whatever followed it, and the TA locked up. The clear's vertex program has the
same epilogue.

### Layout: the microkernel reads the previous client's render

After a client's frame, the microkernel's TA/3D control (dumped on SGX
recovery as `T3C-60..7C`) still holds pointers to that client's RT data
entry and render details. It reads them when the next client's TA starts.

- **Two clients with the same kernel-data layout work.** That covers any DDK
  client, and a replay of a DDK capture.
- **A different layout hangs the TA.** The reads land on whatever the new
  client put at those addresses. The first two generated frames, each run
  after a replay, locked up (`lockup_ta`). Every word was right. The same
  frame on a fresh boot was pixel-exact.
- **The fix is the DDK's layout.** `sgx_ctx_create` and `sgx_surface_create`
  allocate the kernel-data and sync heaps in the DDK's exact order:
  - dummies for its two transfer contexts;
  - three render-details sets.

  So every structure lands at the DDK's address (render details 0x3200e560,
  RT data 0x3200aac0, and the rest). Replay → frame and frame → replay are
  both proven since.
- **The DDK's own cleanup doesn't work here.** It sends
  `SGX_FLUSH_HW_RENDER_TARGET` (cleanup type RT) when it removes a render
  target. On this srvinit-started EA3 that request timed out ("Wait for
  uKernel to clean up (1) failed"), and the GPU locked up right after. It is
  off by default (`SGXGEN_FLUSH_RT=1` sends it, as does `mkreplay.py
  --flushrt`). Whether the blob-initialised microkernel handles it was not
  tested.
- **What a Mesa driver needs.** It must either reproduce this layout, or find
  the cleanup the microkernel accepts. Two different native clients would hit
  the same problem.

### Smaller things

- **Block list.** Its word 2 is the PB descriptor's address (STATE.md said the
  PB's).
- **CCB control.** Render context word 4 is 0x3200aa40, the write/read
  offsets; 0x3200aa60 belongs to a transfer context.
- **Free list.** The DDK allocates 0x40000 bytes, one u32 per page of the
  whole 3D-params heap; sgxgen does the same.
- **The DMA constants.** The vertex secondary constants are 2/255, 1/255,
  2/65535 and 1/65535: the first frame had 1/32767 for the third.
- **Placement.** Every PDS program the DDK makes starts on 32 bytes, every
  USSE program GL allocates on 128, every DMA source on 32. sgxgen does the
  same (`PDS_ALIGN`, `USSE_ALIGN`, `DMA_ALIGN`). Whether the hardware needs
  it was not tested: the alignment change alone did not fix the TA hang.
- **Rings start 0x100 in.** That keeps a PDS state word or DOUTU word from
  ever being 0.

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
  - more than one frame per context;
  - more draws than the rings hold.
- The [G] constants above.
- The RT cleanup timeout (see "Layout").
- The kick's TA/3D status updates are left out. The DDK uses them for its
  ring-buffer bookkeeping, and the frame doesn't need them.
