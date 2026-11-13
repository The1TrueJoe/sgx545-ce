# Minecraft and a same-binary head-to-head: Intel's driver against ours

CE5310 board (CE5310 board), 2026-11-08 19:40 to 22:10 UTC, nothing flashed. Vanilla Minecraft 1.7.10
in a new benchmark world (seed 5915084639795924495, spawn 265/64/41; the old world was lost
with a reinstall), render distance 4, 720x480. Each run: 30 s warm-up after joining, then 60 s
standing and 60 s walking (`mesa/tests/x11/mc-oldnew.sh`). A run with a GPU recovery, a failed
submit or a blank screenshot counts as void.

## The shipped paths

| stack | standing | walking | runs |
|---|---|---|---|
| old: gl4es on the DDK, i386 JVM, pvrsrvkm | 10.3 (9.7-11.4) | 13.0 (12.6-13.2) | 3 |
| new: GLX on our Mesa (um-re d2357fd), x64 JVM, powervr_sgx + DDK microkernel | 8.7 (8.5-9.0) | 9.7 (9.0-10.1) | 3 |
| the same, mesa-gallium's mg-perf c586c90 | 9.3 | 10.3 | 1 |
| the same, c586c90 with `SGX545_DEBUG=zerocopy` | 8.9 | 10.9 | 1 |
| new on the open microkernel (f0dc24d or fd1e151) | void | void | 4 |

- On the open microkernel, every run hung early: "job timed out (ccb 51/53, consumed 50/52)".
  The devcoredump shows an MMU fault: `BIF_INT_STAT 0x000d0004`, `BIF_FAULT 0x1ffff051`. That is
  page index 0xffff of the 3D parameter heap (0x10000000 + 0xffff x 4 KiB), its last page,
  which is never mapped.
- It is the same with secure mode (fd1e151, features 2) and without (f0dc24d, features 0).
- Then "microkernel did not restart: device lost". Two of the runs went on against the lost
  device, with every submit failing, and showed 21-26 FPS. `mc-oldnew.sh` now voids such runs.
- A register sample of the DDK microkernel across the same world join shows no DPM out-of-memory
  event at all: `EVENT_STATUS` bits 1, 2 and 25 stay clear, and `0x724` peaks at 0xab pages. So
  the open microkernel's fault is not a missing out-of-memory path. It takes an all-ones page
  index somewhere in its free list or DPM setup. open-ukernel has the samples.

## The same binaries on both drivers

To see how much of the gap is the driver itself, only the GLES2 library changes. The same i386
chroot, gl4es, i386 Zulu 8 and world; pvrsrvkm with the DDK microkernel on the normal boot; ours
through its pvrsrvkm backend (um-re 2fcc37b, i386). Runs alternate between the two drivers.

| Minecraft through gl4es | Intel DDK | ours | ours / Intel |
|---|---|---|---|
| standing | 10.7 (10.6-11.6) | 6.6 (5.9-6.7) | 0.62 |
| walking | 13.2 (12.9-14.1) | 6.5 (5.4-6.8) | 0.49 |

mesa-gallium's 06b66e6 gave 6.4 and 6.5 in one run. Its stats show what the vertex stage gets:
about 300-335 k indices into 195-225 k vertices a frame.

`sgx545_perfmicro` (i386, the same binary, EGL pbuffer, 3 runs each, stable to 1%):

| test | Intel DDK | ours | ours / Intel |
|---|---|---|---|
| fill: N opaque full-target quads, depth test off | 4435 Mpix/s | 1309 | 0.30 |
| fill-blend | 648 Mpix/s | 368 | 0.57 |
| tex | 4850 Mtexel/s | 874 | 0.18 |
| verts | 8.22 Mtri/s | 3.88 | 0.47 |

- **The JVM isn't the gap.** With the JVM and gl4es the same, ours is about half the DDK's
  speed.
- **fill and tex.** The DDK's opaque fill rate is more than the pixel pipes can shade, so its
  hardware hidden-surface removal draws only the top layer. Ours appears to shade every layer.
  That is draw state, not shader speed, and the largest single factor.
- **fill-blend and verts.** HSR cannot help there. These gaps are shader and vertex throughput:
  the compiler, USSE scheduling, VDM.
- **The render thread.** In the Minecraft runs it waits on the GPU for about half of each 2 s
  window, plus 350-530 ms waiting for a free slot.

## Method notes

- `LIBGL_NOTEST=1` stops gl4es from presenting on both drivers. A first head-to-head batch with
  it set measured frames nobody saw, and is void. `mc-oldnew.sh` now leaves it off unless
  `MC_NOTEST` is set, and the screenshots are checked for content.
- The game's log, its devcoredumps and optional perf profiles are kept for each run.
- A DDK capture of 12 steady frames (pvrtrace, about 350 MB) is kept off the repository for
  ta3d-state's VDM comparison.
