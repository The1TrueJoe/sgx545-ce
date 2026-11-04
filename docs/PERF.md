# Performance: the new stack against the old

Joseph's goal (2026-11-04): the new driver stack has to be **faster** than the old one, not just
as fast. This page is the yardstick. Every run of the suite appends a row to the results below,
and every perf change is measured against them.

| stack | userspace | kernel and firmware |
|---|---|---|
| **old** | the openhc-minecraft install as it runs: i386 Java, gl4es, the DDK's GLES2; Xvfb, gl4es drawing to the panel | pvrsrvkm and the DDK microkernel (the normal boot) |
| **new** | our Mesa: x64 Java, GLX/DRI3 on Xorg (modesetting + glamor on simpledrm); EGL surfaceless for the microbenchmark | powervr_sgx (a RAM boot) with the DDK or the open microkernel, as each row says |

## The suite

`mesa/tests/perf/run-suite.sh old|new` runs on the EA1, after the host has staged:
- the Debian root and our Mesa (`mesa/tests/x11/README.md`);
- `/root/m3` (`mc-oldnew.sh`, `x11-up.sh`, `memdump`);
- the microbenchmark in `/root/perf/bin`;
- the old stack's glmark2 in its own directory inside the install's root.

Each measure runs 3 times. The table gives the median, with min and max in the run's
`summary.txt`. Every run needs 0 GPU recoveries (pvrsrvkm's `HWRecoveryResetSGX` /
`Recovery triggered`, powervr_sgx's `job timed out`); a recovered run is void, since a recovery
fakes frames.

| measure | what | both stacks |
|---|---|---|
| `minecraft-standing`, `-walking` | Minecraft 1.7.10 vanilla, 720x480, RD 4, Fast, no clouds or smooth lighting, VSync off, unlimited FPS, `-Xmx768M`; the A/B rig's benchmark world, a fresh copy each run; 30 s warm-up, 60 s standing, 60 s holding W (`mesa/tests/x11/mc-oldnew.sh`) | frames presented per second |
| `glmark2-window`, `-offscreen` | glmark2 2023.01 (Debian), its default scenes, desktop GL, `-s 720x480`, in a window and with `--off-screen` | glmark2's score |
| `micro-fill`, `-fill-blend`, `-tex`, `-verts` | `sgx545_perfmicro` on a 720x480 pbuffer: 8 full-target quads a frame opaque, blended and textured (Mpix/s), and a 32,258-triangle indexed mesh (Mtri/s); a glFinish per frame | rates; GPU-bound by design |
| `glmark2-es2-window`, `glmark2-es2-drm` | new stack only: glmark2's ES2 build in a window, and straight to the display through GBM (Xorg stopped) | glmark2's score |

How each stack runs the shared measures:
- **Old:** glmark2 is the desktop-GL build through `gl4es-run-ddk`, as every desktop-GL program
  on that stack runs. The DDK has no X11 window system, only its LinuxFB one. The
  microbenchmark runs on the DDK's own EGL through its i386 loader.
- **New:** glmark2 is the same build over GLX on our driver, and the microbenchmark is on our
  EGL surfaceless.

## Results

| date | um-re | microkernel (new) | measure | old | new | new/old |
|---|---|---|---|---|---|---|
| 2026-11-04 | aee60b3 | DDK | minecraft-standing (mc-oldnew.sh only) | 13.5 | 6.1 | 0.45 |
| 2026-11-04 | aee60b3 | DDK | minecraft-walking (mc-oldnew.sh only) | 16.3 | 6.4 | 0.39 |

## Where the time goes on the new stack

`mesa/tests/perf/profile-new.sh` runs glmark2 and the microbenchmark under `perf record` on the
box, with `SGX545_DEBUG=stats`. `mc-oldnew.sh new` with `MC_PROFILE=SECS` profiles Minecraft's
java over part of the standing view. Java's JIT frames show as unresolved addresses; Mesa and
the kernel are symbolised.

| date | benchmark | top CPU hotspots (share of samples) | driver stats |
|---|---|---|---|
| 2026-11-04 | Minecraft (aee60b3) | not profiled yet | ~250 k vertices a frame; vertex cache 0 hits; the draw path ~40% of the render thread, ring-slot waits ~25%, render latency ~100-125 ms |
