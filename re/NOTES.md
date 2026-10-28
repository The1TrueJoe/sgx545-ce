# SGX545 userspace reverse engineering

Goal: an open, native x86_64 userspace for the SGX545 that talks to this
kernel driver directly, so a GL client (eventually Minecraft) no longer needs
Intel's 2012 i386 Cedarview DDK 1.7 blobs. Everything here is learned from the
blobs running on our own hardware, plus the GPL kernel headers in `src/`.

## Tools

| path | what |
|---|---|
| `trace/pvrtrace.c` | LD_PRELOAD tracer: every bridge call (IN/OUT bytes), every mmap of the DRM node, and a copy of all mapped device memory before each `SGX_DOKICK` |
| `trace/trace.sh` | runs an i386 DDK program under the tracer on the box, with a watchdog |
| `probe/probe.c` | GLES2 program driven entirely by environment variables, for captures that differ in one thing |
| `abi/layouts.sh` | exact i386 / x86_64 layouts of every bridge and microkernel struct (clang record dump with the kernel's defines) |
| `abi/ids.sh` | bridge index → name for this build (`abi/bridge-ids.txt`) |
| `decode/memmap.py` | rebuilds alloc → mmap cookie → CPU mapping → device address, so snapshots read by GPU address |
| `decode/cmdta.py` | dumps the TA command each kick points at, tagging device addresses |
| `decode/hexva.py` | hexdump device memory at a kick |
| `decode/diff.py` | word-level diff of two captures' GPU memory at the same kick |
| `decode/mkreplay.py` | turns a pvrtrace log into a replay script + data file |
| `replay/replay.c` | static x86_64 musl program that runs that script through `replay/pvr.c`, no DDK loaded |
| `decode/cmpfb.py` | compares a dumped render target with the probe's `glReadPixels` output |
| `isa/usp.py` | parses a USP binary (the compiler's pre-patch shader container); layout from libusc's writer |
| `isa/gen.py`, `isa/gen2.py` | one-op GLSL corpora for `isa/shc.c` (uniform inputs; varying inputs) |
| `isa/uschook.c` | LD_PRELOAD: saves each USP binary libusc returns |
| `isa/encfuzz.c` | LD_PRELOAD: logs every instruction libusc's assembler encodes and re-encodes one-bit-flipped copies |
| `isa/mkfields.py`, `isa/fz.py` | turn an encfuzz log into `fields.json` (per-form bit fields) |
| `isa/dis.py`, `isa/asm.py`, `isa/hw.py`, `isa/hw_*.py` | USSE disassembler/assembler; the instruction forms read from libusc's encoder |
| `isa/oracle.py`, `isa/uinst.py`, `isa/check_*.py`, `isa/hwcheck.py` | libusc's encoder in an emulator; oracle sweeps and the corpus round-trip check |
| `isa/ISA.md` | the instruction set, with the evidence for each field, and what a compiler needs (`ISA_fz.md`: the superseded first pass) |
| `pds/dis.py` | finds every PDS program in a capture (VDM stream, TA state words, raw pointers, heap scan) and disassembles code and data segment |
| `pds/PDS.md` | PDS instruction set, data-segment layout, DOUT word formats, the DDK's generators, each field tagged confirmed / strong / guess |
| `pds/DumpDecomp.java` | Ghidra headless script: decompile every function in an address range (`analyzeHeadless ... -postScript DumpDecomp.java out.c lo hi`) |

## Findings (confirmed on an EA3, 2026-10-27)

- **Mapping.** libsrv_um maps device memory with a raw `syscall(__NR_mmap2)`,
  not `mmap()`; the page offset is the cookie from `MHANDLE_TO_MMAP_DATA`.
- **Heaps** (id low byte = `SGX_*_HEAP_ID`): general 0x3e000000, TA data
  0x20000000, kernel code 0x30000000, kernel data 0x32000000, pixel shader
  0x37000000, vertex shader 0x3c000000, PDS pixel 0x2e000000, PDS vertex
  0x30400000, sync 0x2d000000, per-context 3D params 0x10000000, texture 0xf0000000.
- **A single-triangle frame** is two TA kicks into the same scene (the first
  with `bFirstKickOrResume`, the second with `bLastInScene`, which renders) and
  one `SGX_SUBMITTRANSFER` (the swap blit).
- **TA command** sits in the kernel-data CCB at `ui32CCBOffset`. The first 0x1e0
  bytes are `SGXMKIF_CMDTA_SHARED` (zero from userspace; the kernel fills it at
  kick time). The client-private part after it holds TA data buffer addresses,
  `0x01df02cf` (= 719 | 479 << 16, surface size minus one), two 1e-6 floats, the
  3D parameter buffer base and a general-heap block (likely the render details).
- **Fragment uniforms** (mediump) are written into the PDS pixel buffer as FP16
  pairs: `vec4 c` = (r,g) at one word, (b,a) at the next (diff of green vs red
  probe: one word, `0x3c000000` → `0x00003c00`).
- **USSE code** is 64-bit instructions. The probe's vertex program is 9
  instructions at the start of the vertex-shader heap; the fragment program
  shares its first instructions.
- **Clear** is a background object: three screen-space vertices (0,0) (1440,0)
  (0,960) at z=1.0, 16 bytes each (x, y, z, colour ARGB8888), drawn as one
  oversized triangle (coordinates look like half-pixel units). Changing
  `glClearColor` only changes those three colour words.
- **Vertex data** from a client array is copied into a general-heap vertex
  buffer (`0x3e51d000` here) right after the clear object's vertices; a second
  draw appends another copy.
- **VDM control stream** (general heap, `0x3e518000` here). Words that carry an
  address keep the command type in the top nibble and the address >> 4 in the
  low 28 bits (`f3040d82` → `0x3040d820`). A draw is
  `80c00003 <index buffer> f0000000 003fffff <PDS vertex program word> 01c00082`;
  the stream ends with `c0000000`, and each extra draw inserts one such block
  before the closing state words.
- **Indices.** `glDrawArrays` becomes an indexed draw over a static 16-bit
  identity index buffer (0, 1, 2, … 1024 entries); the clear object has its own
  three-index buffer.
- **PDS vertex programs** are a data segment followed by code. The clear and
  the triangle share the same 4-word code (`67800072 2f032343 030403e5
  af000000`); only the data segment differs: vertex address, DMA size in dwords
  minus one, stride (clear: 3, 16; xy-float triangle: 1, 8) and one control
  word (`0x0001010a` / `0x00011b0a`).

## Blob-free replay (proven on an EA3, 2026-10-27)

A native x86_64 static binary (`replay/replay`, no DDK library, no dynamic
loader) replays the probe's single-triangle frame (`cap/ref.bin`) and the back
buffer matches the probe's `glReadPixels` **pixel for pixel** (0 of 345600
differ), bit-identical over 4 runs.

    python3 decode/mkreplay.py ref.bin rp --dump 0x3e265000:0x159000:/tmp/rp/bb.bin
    replay/build.sh; copy replay/replay rp/script rp/data to the box; ./replay script data
    python3 decode/cmpfb.py bb.bin ref.rgba --png out.png

What the replay does, in capture order:
- connect, acquire (index 0, type 0), one memory context;
- the 195 `ALLOC_DEVICEMEM` calls with the captured heap/attribs/size/alignment,
  each mapped; **every buffer lands at its captured device address**, shared
  heaps (kernel data, sync) included, as long as no other GPU client is up;
- `WRAP_EXT_MEMORY` of a private page-aligned 0x159000-byte buffer in place of
  fb0; it lands at 0x3e10c000, so the general heap after it does not shift;
- `SGX_REGISTER_HW_RENDER_CONTEXT` from the captured 68-byte buffer (PD offset
  8); the kernel returned 0x3000e000 again, which goes in `sCommand.ui32Data[1]`
  of both kicks (the replay would also rewrite that address in written data if
  it moved; the client keeps it at two places in the 0x3200aac0 block);
- before kick 1, the whole kick-1 snapshot: each buffer zeroed, then its
  non-zero runs written (26840 bytes in all);
- kick 1 (`bFirstKickOrResume`, CCB offset 0x58), sleep 200 ms, then the words
  that changed between the kick-1 and kick-2 snapshots outside heaps 1, 8, 10
  (CCB contents and write offset, client counters, 5 VDM-stream words), kick 2
  (`bLastInScene`, CCB offset 0x318, `h3DSyncInfo`);
- poll the back buffer's sync data until write ops complete (1/1 within 1 ms
  of the wait starting), then dump 0x3e265000.

Event objects, misc info, client info, transfer contexts, dispclass and the
swap (`SUBMITTRANSFER`) are all skipped.

Learned on the way:
- **Kernel bug for native 64-bit clients:** `DoMapToUser` (mmap.c) walks the
  VMA with `IMG_UINT32 ulVMAPos`, so mapping device memory above 4 GB fails
  with `VM_INSERT_PAGE failed (-14)`. i386 clients never notice. The replay maps
  with `MAP_32BIT`; the real fix is `unsigned long` in the driver (same
  pattern in osfunc.c `ulPFNRaw` is worth checking).
- Apart from that, the native x86_64 bridge (no compat layer) works for every
  call used here: connect, acquire, memctx, alloc, mhandle→mmap, wrap,
  render-context registration, DOKICK with a native `pahDstSyncHandles` array.
- **Heaps 8 and 10 are not all GPU output.** Before kick 1 the client has
  written the parameter buffer's free list (0x10422000: 1058 words = one per
  page of the 0x422000-byte PB at 0x10000000), a PB block of floats and sizes
  at 0x10463000, and two sync-heap words (0x2d003020, 0x2d003028 = 1). The
  replay writes the full kick-1 snapshot (all heaps); skipping heaps 1/8/10 at
  kick 1 was not tried, as a bad free list risks a GPU lockup on a shared box.
  Only the kick-2 delta skips heaps 1, 8 and 10.
- Render target: BGRA8888, top row first, pitch 736 pixels (720 rounded up to
  32), 0x159000 bytes = 736 × 480 × 4. `glReadPixels` returns it RGBA and
  bottom row first.
- `SGX_INTERNEL_STATUS_UPDATE` entries: TA ones are counters in the sync heap
  (e.g. 0x2d003040: 0x5c after kick 1, 0x70 after kick 2); each names the
  sync-heap allocation's own mem handle. The render-context buffer's last word
  is the PID (0x4f5); replaying it unchanged did no harm.

## USSE instruction set (2026-10-27, completed 2026-10-28)

See `isa/ISA.md`. libusc's USE assembler (useasm.c, `UseAssembleInstruction`
at libusc + 0x109f50) was decompiled with Ghidra and is also run in an i386
emulator (`isa/oracle.py`, bit-exact against 5045 instructions logged on the
EA3), so every field position comes from the encoder and is checked by
sweeping it (`isa/check_*.py`, ~20,000 encodings). `isa/hw.py` + `hw_*.py`
hold the forms for all 32 hardware opcodes; `isa/dis.py` decodes every word of
the three corpora (c3 adds texture, flow, int, indexing and vertex shaders)
with no unexplained bit, and `isa/asm.py` re-encodes all of them. Findings
worth knowing outside the ISA: dependent texture reads are USP sample blocks
too (the GLES finalizer writes the `smp`); the USP vertex bitmask is the set
of live attribute registers; code-block flags are the format-control state
that decides whether register bit 6 is a C10/F16 select in the colour/EFO
classes; GLSL ES ints compile to float code.

## Our own frame: authored inputs and hand-assembled shaders (EA3, 2026-10-27)

The replay no longer has to copy the captured frame's inputs. `mkreplay.py`
pokes overrides after the snapshot loads and again before each kick
(`replay`'s `poke <dev addr> <word>...` op):

    mkreplay.py ref.bin out --verts=-0.8,-0.6,0.2,-0.8,-0.3,0.7 --clear 0.25,0,0,1 \
        --color 1,0.5,0,1 --usse '0x37008388:pck.u8.f32 o0.f8.f9, c52, c48 [f1.0 f2.8 h12]; ...'

- **Authored vertices and colours, pixel-exact.** The triangle's floats go at
  vertex buffer +0x30, the clear colour (ARGB8888) at +0x0c/+0x1c/+0x2c, and
  `c` as FP16 pairs at `0x2e006184`/`0x2e006188`. The back buffer matches a
  probe run with the same PROBE_VERTS/PROBE_CLEAR/PROBE_COLOR: 0 of 345600
  pixels differ. The three clear-vertex colour words are the only clear state,
  and the uniform copy in the PDS pixel data is the only one the frame reads.
- **The probe's fragment shader, as the driver finalised it** (pixel-shader
  heap, buffer `0x37008000`):
  - secondary program at `0x37008300`: header word `fa440700 00000000`,
    `op0xf1`, `and.test`, `ld pa0 <- [#0]`, `wdf`, then
    `pck.u8.f16 pa9.f8.f9, pa9.f6, pa9` and `pck.u8.f16 pa9.f7.f10, pa10, pa10.f6`
    (the FP16 `c` packed to 8 bits per channel);
  - main program at `0x37008380`: the same header word, then `mov o0, sa9`.
    The secondary program's `pa9` is the main program's `sa9`, and `o0` is the
    pixel colour.
  - The rest of the buffer and the small heap-4 allocations are the driver's
    own programs (clears, byte-mask variants); they don't take part in this draw.
- **f1.17 (h18) ends a program.** It is set on the last instruction of every
  finalised program. A program can grow into the free space after it (the
  secondary program grew from 6 to 7 instructions) with no other change.
- **Pack byte order (confirmed on hardware):** destination flags f7-f10 are
  bytes 0-3 of the BGRA8888 pixel, so f7 = B, f8 = G, f9 = R, f10 = A. In each
  pack, src1 goes to the lower selected byte and src2 to the higher one.
- **Constant colour, no uniform.** Replacing the main program with
  `pck.u8.f32 o0.f8.f9, c52, c48` + `pck.u8.f32 o0.f7.f10, c52, c52 [f1.17]`
  draws (0,255,255,255). That confirms c52 = 1.0 and c48 = 0.0 as f32
  hardware constants, and that `o0` takes a direct pack.
- **Arithmetic on the uniform.** `fmad16 pa9, -pa9, c45, c45 [f1.0 f2.29 h12]`
  ahead of the secondary program's packs gives 1 - c: c = (1, 0.5, 0, 1)
  drew (0,128,255,0). h12 repeats it over pa9 and pa10 (both FP16 pairs).
  f2.29 is set on every vec4 fmad16 in the corpus; f2.30/f2.31 appear when
  src2/src3 is a register, so they are probably per-operand repeat
  increments (**guess**: constants were left without them and worked).
- `isa/asm.py` assembles from `fields.json` and re-encodes nine compiler and
  driver encodings bit-exactly (`asm.py --check`). It refuses anything whose
  fields were never fuzzed.

## USSE semantics on hardware: one register per draw (EA3, 2026-10-28)

`isa/hwt.py` turns the replay into an instruction tester. The main pixel
program (0x37008388, after its `phas.i` word) is replaced by a test program
that loads inputs with `limm`, runs the instruction under test and ends with
`mov o0, rN [end]`. The PBE stores o0 unchanged in the BGRA8888 back buffer, so
a pixel inside the triangle *is* rN, bit for bit; the replay dumps one row of
32 pixels (all must agree) and the background pixel (the clear colour, to
catch a program that didn't run).

    S=<scratchpad>; CAP=<dir with ref.bin> REPLAY=<static replay> EA3SH=<ssh helper> \
        python3 isa/hwt.py hwt_p1 [-k fmad] [--dry]

- The captured main program had **no temporaries**: its DOUTU word 0 (PDS
  data 0x2e018020) is 0x00010700. Poking 0x04010700 (top byte 4) gives it
  r0-r15, and every r0-r15 test reads back exactly, consistent with the
  top byte being the temporary count in fours (PDS.md; whether temporaries
  fail without it was not tried).
- One base script and data file serve every test (the data is uploaded once);
  each test only adds its `poke` lines before each kick. About one draw per
  second over ssh; ~400 draws in all, no hang, no SGX recovery, no reboot.
- Branches that would leave the program are made safe by landing pads: each
  candidate target is a `mov o0, rK [end]` with its own rK, so the readout
  says where the branch went.
- With format control off, a C10 operand names r64+, outside the program's 16
  temporaries: the reads give per-pixel garbage but no fault.

Results are in `isa/ISA.md` ("Hardware checks" and the **hardware** tags),
logs in `isa/hwt_out/`. The ones a compiler would otherwise get wrong:
- an F16 operand number under format select counts 16-bit halves (`2*reg +
  half`), so F16 sources are limited to r0-r31;
- fsin/fcos take quarter turns (sin(t·π/2)) at ~16-bit accuracy;
- scalar ops' h2 writes an F16 result into the source's half and zeroes the
  other half;
- f32 -> f16 `pck` truncates whatever rnd says;
- c46/c47 are F16 (1, 1), not 0.5 / 1.0 as libusc's table says;
- fadm = (a + b) * c, fmsa = a*a + b*c; fminmax = min(max(a, b), c),
  fmaxmin = max(min(a, c), b);
- movc's u8/i16/c10 types select per byte / half / channel (signed);
- `setfc`'s second argument is the colour-class C10 enable, on at task start.

Second round (hwt_p7-p11), after the first write-up:
- **Assembler bug found and fixed:** without `[fmt]`, `asm.py` silently
  dropped a `.f16` / `.c10` on `test` operands, so hwt_p7-p9's "F16/C10
  test" draws were plain 32-bit compares (their suites now say so and
  assemble to the words that ran). `hw.parse` now sets `fmt` itself when an
  operand has a format suffix (as USEASM does) and refuses any operand that
  doesn't survive encoding. hwt_p10 redid the draws.
- **New operand syntax:** F16 operands whose numbers count halves (float
  classes and test under format select, efo under F16 format control: all
  three shown on hardware) print and assemble as `r0.f16.lo` / `r0.f16.hi`
  (encoded 2*reg + half); bare `.f16` there is an error. The corpus now reads
  as what it does: `fmad r0, pa0.f16.lo, ...` / `fmad pa0, pa0.f16.hi, ...`.
  hwcheck.py and all four oracle sweeps pass with it.
- `setfc`'s first argument is the efo F16 enable, off at task start.
- fmad is not fused (the product is rounded first); fmad16 rounds to nearest.
- A repeat steps an F16 operand by one half.
- test chan on C10 compares: 0/1/2 = that channel, 5 = any, 6 = all.
- A C10 source's alpha (sop2) is its channel 0.
- fdp writes only the last iteration's destination.
- testmask byte/word write 0xff/0xffff.
- `br [link]` + `lapc` return to the next instruction.
- The bitwise ops behave as named, and a register shift by 33 gives 0.
- **The EA3 GPU is shared:** another session's blob probe captures collide with
  the replay (its buffers then land elsewhere: `replay:4: buffer 0 landed at
  ...`, rc 2). hwt.py's runner now waits while any other process has
  /dev/dri/card0 open and retries a collided draw.

## PDS programs (2026-10-27)

See `pds/PDS.md`. In short: the DDK's PDS generator is statically linked into
libsrv_um/libGLESv2/libGLES_CM/libOpenVG and Ghidra decompiles it cleanly, so
most encodings come straight from its literal-plus-shift formulas. A program
is 16-byte data rows (DS0 DS0 DS1 DS1) then 32-bit code ending `af000000`.
MOVS (`07……`) is the DOUT instruction: four 2-bit swizzle selects pick words
from a DS0 pair and a DS1 pair, dest 2 DOUTI / 3 DOUTD / 4 DOUTT / 5 DOUTU.
Vertex fetch = `MUL16 ds1[0x32], vertex index, stride` + `MOVS.x DOUTD` per
attribute + DOUTU. Pixel programs = DOUTU + DOUTI per varying (+ DOUTT with
the texture's 4 state words for each non-dependent sample). TA state words
point at pixel programs as `data_dwords<<24 | (addr-0x2e000000)>>4`; DOUTU
word 0 is `code base index | (offset/8)<<4`. `pds/dis.py` explains every bit
of every instruction in 13 captures (textured, dependent, 4-attribute,
interleaved, two varyings among them).
