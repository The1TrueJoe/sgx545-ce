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
