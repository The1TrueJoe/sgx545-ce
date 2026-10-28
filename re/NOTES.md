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
