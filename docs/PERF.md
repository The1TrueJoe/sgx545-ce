# Performance: the new stack against the old

Joseph's goal (2026-11-04): the new driver stack has to be **faster** than the old one, not just
as fast. This page is the yardstick. Every run of the suite appends a row to the results below,
and every perf change is measured against them.

| stack | userspace | kernel and firmware |
|---|---|---|
| **old** | the Minecraft install as it runs: i386 Java, gl4es, the DDK's GLES2; Xvfb, gl4es drawing to the panel | pvrsrvkm and the DDK microkernel (the normal boot) |
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
| `micro-fill`, `-fill-blend`, `-tex`, `-verts` | `sgx_perfmicro` on a 720x480 pbuffer: 8 full-target quads a frame opaque, blended and textured (Mpix/s), and a 32,258-triangle indexed mesh (Mtri/s); a glFinish per frame | rates; GPU-bound by design |
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
box, with `SGX_DEBUG=stats`. `mc-oldnew.sh new` with `MC_PROFILE=SECS` profiles Minecraft's
java over part of the standing view. Java's JIT frames show as unresolved addresses; Mesa and
the kernel are symbolized.

| date | benchmark | top CPU hotspots (share of samples) | driver stats |
|---|---|---|---|
| 2026-11-04 | Minecraft (aee60b3) | not profiled yet | ~250 k vertices a frame; vertex cache 0 hits; the draw path ~40% of the render thread, ring-slot waits ~25%, render latency ~100-125 ms |

## The driver against the DDK, same binary (2026-11-08), and the plan for after

Parked on 2026-11-08: Minecraft running correctly on the open stack comes
first, speed straight after. These are the measurements and the order to work
in.

### Measurements

- **Minecraft, same JVM** (uapi-stream: i386 gl4es + 32-bit Zulu 8 on both
  GLES2 drivers, EA1 normal boot, medians of 3): DDK 10.7/13.2 FPS
  standing/walking, ours 6.6/6.5. The gap is the driver, not the JVM. Ours
  gives the GPU ~300-335 k indices into 195-225 k vertices a frame. The DDK
  has the same 1.5 indices per vertex, batch 16 and vertices-per-task formula
  (ta3d-state's VDM capture), so it doesn't shade fewer duplicates.
- **perfmicro** (`mesa/tests/perf/sgx_perfmicro.c`, EA1 normal boot, same
  source on both; frames a second):

| 1440x960 | DDK | ours | ours/DDK |
|---|---|---|---|
| fill | 354 | 189 | 0.53 |
| fill-depth (`-d`) | 327 | 187 | 0.57 |
| fill-blend | 60.1 | 34.6 | 0.58 |
| tex (`-c`) | 421.6 | 104.6 | 0.25 |
| verts (`-c`) | 245 | 112 | 0.46 |

At 720x480 fill is 0.32 and at 360x240 also 0.32, so a fixed per-frame cost
is higher on ours too.

### Ruled out

- **Hidden surface removal.** Ours removes hidden surfaces: 2.1 Gpix/s over 8
  layers at 1440x960, more than the pipes can shade.
  - Depth writes with depth test off (an experiment) change nothing.
  - fill-depth equals fill.
- **Reloading the last frame.** No clear, so a background load each frame;
  `-c` changes nothing on ours.
- **Texture layout.** POT textures are twiddled on both.
- **Vertex shader length.** One instruction more than the DDK's.

### Found (ta3d-state's DDK capture; STATE.md "The DDK's fragment programs for a flat color and one sample")

1. **Flat color.**
   - The DDK packs the color to U8 once a draw in its secondary program, and
     the pixel program is `mov pa0, sa9` (+ the blend).
   - Ours packs f32 -> U8 every pixel (2 `pck.u8.f32`).
   - Fix: constant kind 0x12. The CPU packs the vec4 as BGRA8 at upload
     (driver side done, 7070f2b) and the compiler emits one mov
     (usse-compiler).
2. **One texture sample to the color.**
   - The DDK samples RGBA8 with T1 format 0x0d, so the texel arrives in the
     output's byte order, and the program is `mov o0, pa0`.
   - Ours uses 0x0c and unpacks to f32 and repacks: 6 instructions, 2 temps.
   - Fix:
     - map RGBA8/RGBX8 to 0x0d;
     - add a per-sampler "BGRA-ordered" mask as a fragment-shader variant,
       like the sRGB one;
     - the compiler unpacks those samplers zyxw, and emits a plain mov when
       the texel goes to the color unchanged.
   - Interim: 2 `pck.u8.u8` lane picks.
3. **Per-frame fixed cost and vertices (0.46).** Not found yet.
   - Measure submit -> kick -> done with the powervr_sgx tracepoints (m2).
     m2's first suspect on that stack: the kernel clflushes every imported
     dma-buf around every job, under X the full-screen scanout
     (`pvrsgx_flush_imports`).
   - Measure the PDS vertex fetch against the DDK's.

### Order

1. Items 1 and 2 (compiler + driver), then measure perfmicro and Minecraft.
2. Item 3, from the traces.
3. `SGX_DEBUG=zerocopy` as the default. It gave +6% walking and -4% standing
   in single runs: take medians first.
4. Ring depth (`SGX_SLOTS`), once the SLOTS=6 hang is understood.

### Measured 2026-11-09: the colour fast paths

usse-compiler's speed pass (9ce70e2: items 1 and 2's interim form, a
uniform colour as one mov from a 0x12 slot and a texel as colour by two
`pck.u8.u8`, plus 82f3f5c, 1ead616, 64d06e4, 729718d, 60eea0e). Measured on
the EA3 under the DDK microkernel, with `-c`, 3 s a test, in frames a second.
The baseline is dev without those commits. Its 1440x960 numbers match the
EA1 table above (fill 187 against 189), so the boards compare.

| test | baseline | the pass | DDK (table above) | ours/DDK now |
|---|---|---|---|---|
| 720x480 fill | 476 | 790 | 1524 | 0.52 |
| 720x480 fill-blend | 132 | 187 | | |
| 720x480 tex | 317 | 840 | | |
| 1440x960 fill | 187 | 235 | 354 | 0.66 |
| 1440x960 fill-blend | 34.6 | 52.1 | 60.1 | 0.87 |
| 1440x960 tex | 104 | 237 | 421.6 | 0.56 |
| 1440x960 verts | 110 | 112 | 245 | 0.46 |

- Full dEQP-GLES2 on the pass: 0 unexpected results, 0 recoveries.
- **Tex** is still 0.56: the DDK's one `mov o0, pa0` (T1 0x0d and a BGRA
  sampler mask) against our two lane picks. The rest of item 2 does that.
- **Fill at 720x480** (0.52) is the per-frame fixed cost, item 3.
- **PDS sizeinfo b25-28.** The DDK's rule (12 / units, capped at 0xb only
  with secondaries: its textured draw 0x18030001) against ours (always
  capped) changed nothing: tex 316.4 against 316.9 fps. Not adopted.

**The rest of item 2** (13eefef, 89fab5d, with usse-compiler's tex_bgra and
pds_samples_alone): perfmicro's tex is now the DDK's shape, one pure-sample
DOUTI and no instruction before the driver's `mov o0, pa0`. On the EA3 it
changed nothing: 1440x960 tex 237 → 239 fps, 720x480 836 → 835. So the
shader is not what limits the micro tests. Over both sizes ours costs
2.9 ms a megapixel for fill and tex alike (8 layers, hidden surface removal
on) plus ~0.2 ms a frame. The DDK costs ~2.1 ms a megapixel for fill, and its
tex is faster than its fill. What is left is per pixel and outside the USSE
(ISP, the tile loop, PBE), to be compared with the DDK's 3D state for the same
draws.

### Where a Minecraft frame goes (EA1, 2026-11-09)

uapi-stream's profile of the bench world (720x480, render distance 4,
standing, 60 s after joining), raw files in
`sgx545-work/perf-mc-2026-11-09/`. Ours is the x64 JVM on our desktop GL under
Xorg (dev 708a02e, powervr_sgx 535b5dc, open microkernel a57d578): 10.6 fps,
94.4 ms a frame. The DDK is the i386 JVM through gl4es on Xvfb: 12.8 fps,
78 ms.

From 5 s of powervr_sgx tracepoints (kick, IRQ, job_done, flush_imports),
a frame is four renders:

| render | GPU (kick or previous IRQ → IRQ) |
|---|---|
| Minecraft's world | 64.0 ms |
| Minecraft's two small 720x480 renders | 2 × 2.2 ms |
| X's present (client 41) | 1.1 ms |
| **GPU busy** | **69.5 ms (74%)** |
| **GPU idle** | **24.8 ms** |

The idle is two things:

1. **The kernel's dma-buf flushes, ~12 ms a frame.** Every job clflushes its
   1 or 2 imported buffers (1,413,120 bytes each) before the kick and after
   the IRQ, 1.2-1.4 ms each: 424 flushes, 688 ms in 4.9 s. Between two jobs
   the GPU waits IRQ → job_done 1.34 ms (flush), job_done → scheduler 0.16 ms,
   scheduler → kick 1.44 ms (flush), medians. The kernel runs one job at a
   time (credit limit 1), so nothing hides them. Sent to m2: skip the flush
   for buffers no CPU wrote through a cached mapping, and allow more than one
   job on the hardware.
2. **The GPU waiting on the CPU, ~13 ms a frame.** A frame is Minecraft's
   world render into its FBO, then a 2-object and a 1-object render into the
   back buffer (`SGX_DEBUG=frame`, 749a8e4). With 3 ring slots the third
   kick's end-of-frame wait lands on the world render kicked just before:
   the thread sleeps ~20-50 ms, then records the next world while the GPU has
   only the two small renders. With `SGX_SLOTS=4` the slots cycle 0-3 and the
   waits move to the previous frame's small renders, which queue behind the
   64 ms world render. So the frame is GPU-bound and a deeper ring only moves
   the wait: 8-10 fps with the logging on. Both 4-slot runs also stalled the
   session after the measured window (and one met 5 recovered GPU timeouts
   over 4 h). Not adopted.

The import flushes, after m2's "flush only listed imports" (m2-drm 30919fc):
Minecraft 10.7 → 11.2 fps, flushes 424 → 318 per 5 s. What is left is
Minecraft's own back buffers. The DRI3 loader asks for every back buffer
with `__DRI_IMAGE_USE_SCANOUT`, so kmsro makes each a cached simpledrm dumb
buffer, imported and flushed around both small renders (~5 ms a frame). Only
X's screen pixmap is scanned out (X copies the window: its 1.1 ms job a
frame). Nothing tells the pipe driver that an image is a back buffer (dri2.c
maps `__DRI_IMAGE_USE_BACKBUFFER` to no bind flag). So on such a display a
scanout render target now starts as the SGX's own buffer and becomes a dumb
buffer, with its contents, only when a KMS handle is asked for: X's root
pixmap does that, client back buffers never do. With m2's listed-import
flushes, Minecraft's import flushes went 318 → 102 per 5 s (553 → 132 ms),
one 1.4 MB import a frame (X's); the desktop smoke stayed clean, es2gears
135 → 160 fps, glmark2 67 → 83.

With both gone the frame is the GPU's 69.5 ms: 14.4 fps, past the DDK.

CPU (perf, 20 s, the whole JVM): JIT 42%, libjvm 32%, libgallium 11%,
kernel 10%. The client thread is 39% of samples (DDK 43%), the C2 compilers
31% (still compiling at 60 s on the x64 JVM). Box idle 33% (DDK 40%). Mesa's
CPU is not the bottleneck: the stats give the client thread 1.0-1.2 s of
CPU in 2 s, draws 180-250 ms of it.

**TA against 3D** (open-ukernel's frame-profile microkernel 775b19a,
`UK_FRAMEPROF=1`: running totals of TA and 3D busy ticks and their counts in
the control block, the EDM's ~1 kHz tick; same scene with 31f9341's vertex
change, ~9 fps in the window): over 5 s the TA is busy 37.6% (320 kicks, 2 a
render) and the 3D 23.4% (160 renders). That is ~42 ms of TA and ~26 ms of 3D
a frame, almost all of it the world render. The vertex path is the larger GPU
bucket, and perfmicro verts is 0.46 of the DDK.

### The vertex path against the DDK (perfmicro verts, 2026-11-09)

ta3d-state diffed the DDK's and our state for perfmicro's verts test on the EA3
(the same binary; captures kept outside the repos).
- **The point size.** st gives every vertex program a point size and we output
  it on triangles: 5 dwords a vertex into the parameter buffer, which the VDM
  rounds up to an 8-dword output area. The DDK's position-only vertex is 4.
  Fixed in 31f9341 (only points draws keep it). On the EA1 tex went up 11-13%
  (1440x960 265 → 300 fps), and verts (120 → 121), fill and Minecraft (10.3/10.8
  → 10.6/10.6) didn't move. Full dEQP-GLES2 unchanged.
- **Matches:** ISPA, region clip, MTE control, W clamp, viewport, TE/tiling
  words, batch 16, vertices per task 16, 16-bit indices. The DDK's index-list
  count is wider than 16 bits (one list of 96,774 indices; we split at
  65,535: negligible).
- **The vertex DMA** is 2 dwords on the DDK (the VBO as it is) and 4 on ours
  (gathered and widened to f32 x 4). `SGX_DEBUG=zerocopy` (2 dwords) changes
  nothing: 110.6 → 108.7 fps on the EA3.
- **Our frame** on the EA3 (`SGX_DEBUG=stats`, 110 fps, 9 ms): ~2.6 ms of CPU
  in the draw (every index read, rebased to 16 bits and copied; the vertices
  gathered), then ~6 ms kick to done. The DDK's whole frame is 4.1 ms. So the
  GPU's vertex work is ~2x the DDK's, with the TA state matching; next are the
  two vertex programs and their DOUTU words (the DDK sets 0x10000 on every
  DOUTU, we never do).

### TA kicks within a frame, and the vertex gap on the EA3 (2026-11-09)

`SGX_TA_SPLIT=n` (0bf22f3, services backend, off by default) kicks the TA
every n objects. The DDK does the same (ta3d-state: TA flags 0x71/0x60/0x62;
3D flags, TA flags 2 and the 3D sync on the last kick only; the target's sync
on every kick), so the GPU tiles while the CPU records the rest.
- Correct: dEQP fragment_ops/draw/fbo/depth_stencil_clear/texture.units/
  builtin_variable/rasterization unchanged with n = 2 (2,818 tests),
  0 recoveries.
- perfmicro's new `rows` test (the verts mesh as 127 draws, a colour each):
  1440x960 58.4 → 63.3 fps, 720x480 80.4 → 86 fps with n = 16-32. verts and
  fill are unchanged (one or eight draws).
- A scene left open, with a first kick and no last one (a client killed
  mid-frame), put services into a microkernel-init recovery loop until a
  reboot. Every scene a flush opens must be closed. powervr_sgx needs a
  uAPI for it (uapi-stream: a RENDER flag that leaves the scene open on its
  rt_dataset, and a PB gate for other contexts).

The DDK on the same box, same binary (i386 perfmicro):

| test | DDK 720x480 | ours | DDK 1440x960 | ours |
|---|---|---|---|---|
| fill | 1432 | 791 | 392 | 233 |
| verts | 537 | 146 | 329 | 110 |
| rows | 238 | 80 | 205 | 58 |

The vertex-heavy gap (3.4x) is twice the fill gap (1.7x). Our 720x480 verts
frame (6.85 ms) is ~2.6 ms of CPU in the draw (every index read, rebased and
copied; the DDK draws from the buffers as they are) and ~4.1 ms kick to done;
the DDK's whole frame is 1.86 ms. With the vertex programs, TA state and
vertex DMA width ruled out, the parameter buffer size (4 → 16 MB: no change)
too, the next suspects are the memory the vertex path reads (our rings'
allocation flags against the DDK's VBO/IBO) and the CPU index path
(GPU-resident index buffers with the VDM's index base, instead of rebasing).

### perfmicro against the DDK, rendered frames (2026-11-09) — supersedes the ratios above

**Every earlier perfmicro comparison with the DDK in this file compared
TA-only frames with rendered ones.** perfmicro ended each frame with a
glFinish. On the DDK's pbuffer that only kicks the TA and keeps the scene
open: ta3d-state's captures show its 3D renders once every 44 to 566
"frames" (1,215 kicks, 13 scenes). Ours renders every frame. perfmicro now
ends each frame with a 1-pixel glReadPixels (7884888), which renders on
both.

EA3, normal boot, the same binary (frames a second; ours/DDK):

| test | DDK 720x480 | ours | ours, zerocopy | DDK 1440x960 | ours | ours, zerocopy |
|---|---|---|---|---|---|---|
| fill | 812 | 777 (0.96) | 748 | 237 | 230 (0.97) | 233 |
| fill-blend | 200 | 186 (0.93) | 186 | 53.6 | 51.9 (0.97) | 51.8 |
| tex | 1067 | 818 (0.77) | 804 | 335 | 238 (0.71) | 238 |
| verts | 358 | 137 (0.38) | 322 (0.90) | 199 | 107 (0.54) | 190 (0.95) |
| rows | 99 | 81 (0.82) | 85 | 70 | 59 (0.85) | 62 |

- Fill and blend are at the DDK's speed: the per-pixel gaps of the 0.5x
  tables above were the DDK not rendering.
- **Texturing** was 0.71-0.77 of the DDK, and the GPU wasn't the cause.
  ta3d-state's captures of perfmicro tex matched word for word (TA, ISP,
  PDS, texture words, render details, per-tile programs, the texture's
  memory), and the A/Bs of the two near-differences (PDS middle word b25-28
  12, render target in the general heap) changed nothing. The services
  backend polled a render's completion at 50 us steps doubling to 1 ms, so
  a 3 ms render was seen done at 3.55 ms. Now a render's wait sleeps to
  3/4 of its target's last render times, then polls every 50 us, tex is
  237 -> 302 fps at 1440x960 (0.90) and 810 -> 880 at 720x480 (0.83); fill
  and blend are at or above the DDK's, verts +8%. (powervr_sgx waits with
  poll() on its fd, event-driven: no such rounding.)
- **Vertices:** 0.38-0.54 by default and 0.90-0.95 with
  `SGX_DEBUG=zerocopy` since 72b75fb: an indexed draw from buffers takes its
  indices as they are, from the index buffer's device copy, with no CPU work
  a draw. Zero-copy is the default on powervr_sgx since dEQP-GLES2 mustpass
  (16,637 pass, 0 new fails, 0 timeouts) and piglit (no new fails) ran clean on
  the EA1 (7bca567); services keeps it opt-in, `SGX_DEBUG=nozerocopy` turns it
  off. Its 79 (214 in a long run) dEQP fails
  depended on test order: a stream uploader writes more into its mapped
  buffer, and the device copy was refreshed only at the unmap, so later
  tests' draws read zeros (fixed 7543399, 0d2b9ec: the copy is refreshed
  for the bytes a draw reads while a write map is open). Under zerocopy all
  214 pass and a 3,898-test subset matches the default path. Left: one
  chunk (clipping.triangle_vertex.clip_three) made 18 recoveries on the EA3
  (services); 264/264 and 0 recoveries on the EA1 (powervr_sgx). Minecraft
  gains nothing from zerocopy (10.8 vs 11.0 fps): its geometry is display
  lists, drawn from the vertex cache.
- Minecraft swaps every frame, so the DDK renders there too: its profile (TA
  ~42 ms, 3D ~26 ms) stays the honest target.

### perfmicro on the open stack (EA1, 2026-11-10)

What users run: powervr_sgx and the open microkernel (EA1 normal boot, kernel
7.1.8-m2drm, dev 3329021, default settings: zero-copy off), against the DDK
on the same EA1 (its own boot). Every frame rendered (a 1-pixel readback);
two rounds each, averaged; frames a second:

| test | DDK 1440x960 | ours | ours/DDK | DDK 720x480 | ours | ours/DDK |
|---|---|---|---|---|---|---|
| fill | 243 | 236 | 0.97 | 829 | 752 | 0.91 |
| fill-blend | 53.9 | 53.8 | 1.00 | 201.5 | 196.5 | 0.98 |
| tex | 341 | 298 | 0.87 | 1075 | 854 | 0.79 |
| verts | 202.5 | 116.5 | 0.58 | 360 | 158.5 | 0.44 |
| rows | 72.5 | 61 | 0.84 | 102 | 84 | 0.83 |

- The open stack has no polling quantization (powervr_sgx waits with poll()
  on its fd). tex 0.79-0.87 is what is left to find on the pixel side,
  higher at the small size: a per-frame cost.
- verts is the CPU index path (every index read and rebased each draw);
  zero-copy's resident indices take it to ~0.9 on the EA3. Zero-copy
  becomes the default on powervr_sgx once its full dEQP + piglit run is clean.

### perfmicro on the open stack against the DDK (EA3, 2026-11-11)

Current gaps on the EA3, which has no persistent open stack:
- **Ours:** a RAM boot of the CI kernel (m2-drm 9ab77e2, 7.3.3-m2drm, which
  includes 0f4d7d3), powervr_sgx and the open microkernel. Mesa is from dev
  01768aa, with its defaults: zero-copy, F16 sampling, state reuse and 4 slots.
- **DDK:** the DDK's GLES2 on the normal boot.
- The same perfmicro source for both (x86_64 on ours, i386 on the DDK), every
  frame rendered (a 1-pixel readback).
- Two rounds of 5 s each, averaged; frames a second. 0 job timeouts and
  0 recoveries.

| test | DDK 720x480 | ours | ours/DDK | DDK 1440x960 | ours | ours/DDK |
|---|---|---|---|---|---|---|
| fill | 840.0 | 758.8 | 0.90 | 248.1 | 242.1 | 0.98 |
| fill-blend | 201.3 | 196.8 | 0.98 | 53.7 | 53.9 | 1.00 |
| tex | 1104.7 | 958.1 | 0.87 | 351.0 | 334.9 | 0.95 |
| verts | 366.3 | 360.3 | 0.98 | 212.2 | 209.4 | 0.99 |
| rows | 98.1 | 95.3 | 0.97 | 72.0 | 76.7 | 1.06 |

- **verts:** 0.44-0.58 is now 0.98-0.99, with zero-copy as the default.
- **rows:** above the DDK at 1440x960.
- **What is left is per frame:**
  - At 1440x960 every test is within 5%. At 720x480, fill (0.90) and tex (0.87)
    lose about 0.13 ms and 0.14 ms a frame against the DDK's 1.19 ms and
    0.91 ms.
  - That cost doesn't grow with the pixel count, so it is in the frame's fixed
    work (kick, completion, the readback), not in the per-pixel programs.
- glmark2 isn't on the EA3, which has no x64 root and no mounted Minecraft root,
  so its numbers come from the EA1 run.

## Shader code against libusc

The DDK's compiler (libusc) is the bar for the code the GPU runs. We compare its output with
ours shader by shader:

```
python3 -I tools/usc/run_corpus.py SGXC tools/isa/corpus/c2.glsl OUT --driver
python3 -I tools/usc/count.py OUT tools/isa/corpus/c2 [--md]
```

`--driver` compiles every shader the way the gallium driver does (`sgxc -p -L -D -F`).
`count.py` reports, per shader:
- **instructions** in the main program;
- **issue slots**: a repeat counts its iterations, so `fmad rpt=4` is 4;
- **temporaries** (the header's `n_temps`). The DOUTU allocates them per instance, so fewer
  temporaries let more instances be resident to hide texture latency;
- libusc's **secondary update program** separately. It runs once per draw, not per pixel.

libusc's numbers come from the corpus USPs in `tools/isa/corpus/c1`-`c3`.

| set | when | instructions | issue slots | temporaries | libusc (main + secondary) | libusc slots | libusc temps |
|---|---|---|---|---|---|---|---|
| c2 (259 shaders) | um-re 1900475 | 1400 | 1963 | 1155 | 1099 + 36 | 1380 | 101 |
| c2 | 894c995 | 1221 | 1758 | 764 | | | |
| c2 | 5f3acd2 | 1197 | 1734 | 756 | | | |
| c3 (174 shaders) | um-re 1900475 | 2098 | 2963 | 1206 | 1798 + 43 | 2277 | 219 |
| c3 | 894c995 | 1812 | 2585 | 496 | | | |
| c3 | 5f3acd2 | 1788 | 2561 | 565 | | | |

The gl4es Minecraft shaders (`tools/usc/tests/gl4es/mc.glsl`), driver mode. Each cell is
instructions / issue slots / temporaries. libusc's USPs for these have been requested from a
box run.

| shader | 1900475 | 894c995 | libusc |
|---|---|---|---|
| mc_terrain vert | 20 / 68 / 16 | 17 / 47 / 8 | pending |
| mc_terrain frag | 19 / 19 / 6 | 19 / 19 / 0 | pending |
| mc_texfog vert | 15 / 48 / 12 | 13 / 31 / 8 | pending |
| mc_texfog frag | 15 / 15 / 5 | 15 / 15 / 0 | pending |
| mc_gui vert | 6 / 28 / 8 | 5 / 24 / 8 | pending |
| mc_gui frag | 11 / 11 / 4 | 11 / 11 / 0 | pending |
| mc_sky vert | 14 / 44 / 12 | 12 / 27 / 8 | pending |
| mc_sky frag | 6 / 6 / 3 | 6 / 6 / 2 | pending |
| **total** | **106 / 239 / 66** | **98 / 180 / 34** | |

The glmark2 shaders, 56 of the 61 (`tools/usc/glmark2_corpus.py`, then `run_corpus.py --driver`).
The five left out are terrain's three (desktop GLSL, vertex texture fetch) and bump-normals-tangent's
two. Totals are instructions / issue slots / temporaries.

| set | 894c995 | 5f3acd2 | libusc |
|---|---|---|---|
| glmark2 ES2 precision | 1513 / 2416 / 397 | 1225 / 2132 / 388 | pending |

On the new stack, Minecraft draws through st's fixed-function programs, not through the gl4es
shaders. Those programs, and libusc's output for glmark2 (`shc` takes the same corpus file), are
the next rows.

**Precision on the desktop-GL path.**
- st builds the fixed-function programs (`ff-fs`, `ff-vs`) straight as 32-bit NIR.
- Desktop GLSL has no precision qualifiers.
- So every fragment program of Minecraft and of desktop glmark2 compiles in F32 on the new stack.
- The old stack's gl4es hands the DDK mediump GLSL ES.
- Compiled as highp, the gl4es Minecraft fragment shaders take 27/21/14/9 issue slots, against
  19/15/11/6 in F16.
- `opts.fs_mediump` (626f712, `sgxc -M`, `run_corpus.py --fs-mediump`; off by default) compiles a
  fragment program's floats as mediump:
  - inputs, texels and uniforms become F16, and the ops over them are rebuilt in F16;
  - texture coordinates stay f32.
- GL asks of fixed-function color math only the framebuffer's precision, so this is in spec for
  st's `ff-fs` programs.
- Effect on the highp builds:
  - gl4es Minecraft frags: 68 → 51 issue slots, 19 → 2 temporaries, the same as their mediump
    build;
  - glmark2's frags: 901 → 886 (lighting math gains little; see co-issue and the F32 dot
    products).
- The driver sets it. For st's fixed-function programs that is `nir->info.name` "ff-fs".

### What closed the gap so far

| commit | change | corpus effect (all sets, F32 and F16) |
|---|---|---|
| f6cdfcd | `nir_opt_shrink_vectors`: components nothing reads go. Example: `MV * v` where fog reads only `.z` | 13586 → 13113 instructions, 19760 → 18566 slots |
| becc007 | `fold_movs`: an instruction writes the output or copy destination itself, so there is no trailing `mov` | → 12968, → 18257 |
| 887e523 | `place_in_pa`: temporaries in PA registers after the input's last read, as libusc does. A register read inside a loop stays busy over the whole loop: bf9048a fixes that, 887e523 alone is wrong for loops that aren't unrolled | temporaries 7910 → 5820 |
| 111d7e7 | 32-bit swizzles of SSA values are aliases, not register copies. Example: `vec4(x)`, which `nir_lower_vec_to_regs` turns into a swizzle. (Its `vec_to_regs_32` change was a misreading, undone in 5f3acd2.) | → 12238, → 17489 |
| 894c995 | a comparison read only by selects keeps its `testmask` mask (no 1.0/0.0 `movc`); selects of F16 values move whole pairs (`movc.i32`); vregs shrink to the components used | → 11857, → 17018; temporaries → 4973 |
| 98652ce | a scalar F16 `frcp`/`frsq`/`fexp2`/`flog2` is one instruction; with `driver_consts`, non-hardware f32 constants are SA literals (an `fdp` constant vector a run of them) instead of `limm` + `mov`; an F16 min/max/sat of f32 values runs in f32 and rounds once | glmark2 1513 → 1285; light-advanced frag 38 → 18 |
| 09b4931 | `narrow16`: f32 selects/min/max over values that are F16 underneath run in F16, so the uniforms under them load as F16 constants | glmark2 → 1249; ideas-lamp-lit frag 173 → 145 |
| cdd95cf | a select of one lane (a vec3's z) moves whole registers when the lane is in the same half of both sources | glmark2 → 1233 |

From here the corpus runs every set three ways: F32, F16 and driver mode (11828 files, 17675
instructions at 5f3acd2).

### Temporaries

Temporaries limit how many instances are resident, and so how much texture latency is hidden.
They were the largest gap.

Driver mode, temporaries per shader:

| set | 5f3acd2 | 649281d | libusc |
|---|---|---|---|
| c2 | 756 | 198 | 101 |
| c3 | 565 | 264 | 219 |
| gl4es Minecraft (8 shaders) | 34 | 2 | pending |
| glmark2 (56 shaders) | 388 | 195 | pending |
| corpus, every set three ways | 7207 | 3484 | |

| gl4es Minecraft shader | before (bf9048a) | after (649281d) |
|---|---|---|
| mc_terrain vert / frag | 8 / 0 | 0 / 0 |
| mc_texfog vert / frag | 8 / 0 | 0 / 0 |
| mc_gui vert / frag | 8 / 0 | 0 / 0 |
| mc_sky vert / frag | 8 / 2 | 0 / 2 |

glmark2, the biggest moves:
- every vertex shader 8-13 → 4 or 0;
- terrain frag 17 → 12, light-cel 12 → 9, bump-normals 7 → 4, light-advanced 6 → 3;
- terrain-overlay, text-renderer and desktop frags → 0.

How:
- **Split** (f2247cb): a 32-bit vreg that no repeat steps through becomes one vreg per
  component.
- **Color sources** (f2247cb): a single instruction reads before it writes, so the color's
  sources may sit in the color register.
- **In-place repeats** (083bdd0, 65e768e): a repeat whose source dies there, read at the
  component it writes and stepping with it, writes over it. That is libusc's
  `fmad pa0, pa0, pa4, pa8 rpt=4`: iteration k reads component k before writing it, and no later
  iteration reads an earlier one's write. It applies to vregs and to dead PA inputs.
- **Per-register PA liveness** (65e768e, a919daa): each register of a vreg is placed by its own
  first and last use. 32-bit and F16 vregs both.
- **mediump demotion** (a919daa): an op over mediump operands runs in F16, as GLSL ES allows.
  Mesa had left `a * b` of mediump varyings in f32.
- **F16 color pairs** (a919daa): (g, r) / (b, a), one register per `pck`.
- **`fdp`** (77632f3, 649281d): it writes only its last iteration's register, once and after its
  reads. So the operand names that register alone, and it may land on its own inputs.
- **Output registers** (60c90ff): a vertex shader's temporaries go in output registers before
  their first write, as libusc does (`fmad o0, pa0, sa18, o0`). The `mat4 * v` accumulator ends
  in `o0`.
- **Color clamp** (649281d): `clamp(colour, 0, 1)` goes, since the U8 `pck` saturates.

What still holds temporaries:
- internal registers (libusc's `efo` keeps `mat4 * v` partials in i0/i1);
- C10 lowp math in internal registers;
- `smlsi` swizzled repeats (`a.zxyw * b` in place);
- scheduling: we emit a vector op component by component (`testmask`/`movc` interleaved) where
  libusc repeats.

### What is left, largest first

- **Uniform-only math per pixel.**
  - libusc moves expressions over uniforms into the secondary update program, which runs once
    per draw. In c1 that is 877 of its 1037 instructions.
  - We have no secondary program. `driver_consts` (`-D`) covers only the C10 → F16 conversion
    of a lowp vector.
  - Mesa's preamble (`nir_opt_preamble`) would produce the code. The driver would have to run
    it from the PDS secondary program.
- **Selects instead of predicates.**
  - libusc tests into a predicate and writes the result under `(p0)` / `(!p0)`.
  - We compute a 1.0/0.0 value or a mask and `movc` it.
  - c3: `movc` 452 against 48, `testmask` 108 against 18.
- **8-bit color math.**
  - libusc adds and multiplies RGBA8 texels and lowp colors in U8/C10 (`sopwm`, `sop2`).
    `t_two` (sum of two textures) is 1 instruction.
  - We unpack every texel to F16 or F32, compute, and pack again: 11 instructions for
    `t_two`.
  - The sampler's channel swizzle (`zyxw`) also saves libusc the reorder.
  - Example: `gl_FragColor = texture2D(s, uv)` (`tx_nd_*`) is 0 instructions for libusc. The
    sample is swizzled into the color's byte order and its PA register is the color.
  - We spend 4 (unpack, pack). Closing it needs the driver to take a sample swizzle and the
    color in the sample's register.
- **Loops.**
  - We unroll every loop fully. `lp_nest` is 66 instructions against 11, but it runs no
    branches, so it is no slower.
  - libusc keeps small loops and predicates the exit.
- **Precision on the desktop-GL path** (above).
  - The driver enabling `fs_mediump` for `ff-fs` closes it for st's fixed-function programs.
  - Desktop GLSL without qualifiers would be an opt-in beyond the spec.
- **Co-issue.**
  - libusc pairs operations with `efo` and `fdual`: 134 and 36 instructions in c2+c3.
  - We emit `efo` for a sum of vector × scalar terms (`mat4 * v`, `mat3 * n`) whose vectors
    sit in a bank (uniforms, attributes, varyings), as libusc's `v_mvp`: per lane pair one
    `efo` per term with the partials in i0/i1, then `mov` from i1. A `mat4 * v` is 10 issue
    slots instead of 16. `SGX_COMPILER_DEBUG=nocoissue` turns it off. 32 `efo` in c2+c3.
  - Driver mode, issue slots: corpus 7791 → 7726, glmark2 2080 → 1826 (temporaries 195 →
    157), gl4es Minecraft 180 → 150. Instructions go up (glmark2 1211 → 1470): a repeat is one
    instruction, its `efo`s are several.
  - Not done: `fdual`. Only 75 scalar f32 ops in the corpus could pair, and fmaxmin as a
    secondary is still open (ISA.md "Dual issue").
- **In-place repeats.**
  - libusc writes `fmad pa0, pa0, pa4, pa8 rpt=4`. We keep a repeated instruction's
    destination apart from its sources, which is safe but costs registers.

### Speed pass, 2026-11-09 (not yet measured on hardware)

Driver mode, against libusc (corpus c1/c2/c3, issue slots / temporaries). Each step was
checked to make no shader worse in any mode (F32, F16, driver), and to change the
encoding of nothing it doesn't intend to.

| step | commit | what | effect |
|---|---|---|---|
| colour fast paths | 8f3d5f0 (merged fe15537) | a texel that is the colour as two `pck.u8.u8` byte picks; a uniform colour as one `mov` from a slot the driver packs to BGRA8 (constant map 0x12) | `texture2D` to colour 6 → 2 instructions, 0 temporaries; flat colour 2 → 1 |
| early-exit loops | 82f3f5c | a loop whose trip count isn't exact (an early `break`) stays a loop | `loop_v` 31 → 4 temporaries (libusc 9); c2 temporaries 198 → 171 |
| floor / in place | 1ead616 | `floor` as one `fsubflr d, c48, x` read negated (`ceil` becomes libusc's one instruction); split repeats write in place over the inputs they read last | c2 1647 / 171 → 1635 / 143, c1 797 / 280 → 785 / 276 |
| mask booleans | 64d06e4 | or / and of comparisons stay test masks (bitwise `or` / `and`) | glamor radial gradient 225 → 205 instructions, linear 163 → 145; c3 slots −15, glmark2 −6 |
| texel channels | 729718d | a texel unpacks only the channels something reads | glamor glyphs 7 → 4 |
| branch tests | 60eea0e | an `if` on a comparison tests into `p0` directly; on a mask, `test.and.eq` | c3 slots 2489 → 2467 |

Totals, driver mode: c1 797 → 783 slots (temps 280 → 276), c2 1696 → 1633 (198 → 143),
c3 2507 → 2467 (264 → 263), glmark2 1302 → 1292 (99 → 98). libusc: c2 1391 / 101,
c3 2277 / 219.

Each step has its own `SGX_COMPILER_DEBUG` flag (`notexcolor`, `nopackedcolor`,
`unrollexits`, `nomaskbool`, `noiftest`) for bisecting a hardware failure.
`tools/isa/hwt_p13.py` checks the new instructions (`pck.u8.u8` lanes, mask `or` / `and`,
`test.and.eq`) on a box: all 11 passed on the EA3 (2026-11-09). Full dEQP-GLES2 on the
EA3 with the whole pass (60eea0e): 16,637 pass, 0 unexpected, 0 wedges.
On the EA1 (708a02e, uapi-stream, 2026-11-09): the dEQP shaders/fragment_ops/texture.format
subset 11,940 pass with no new failure (`functional.shaders.*` 10,101 with the one expected
fail); glmark2 720x480 on Xorg/glamor score 51 -> 53 (most scenes 2-5% faster, loop/uniform
+9%); Minecraft 10.1 / 10.9 -> 10.2 / 11.0 FPS standing / walking, picture unchanged
(the fragment work is not Minecraft's bottleneck at this point).

efo chaining (3db45e4, libusc's chained `v_mvp`: the next chain's first efo stores the
previous chain's last lane with `d=i1`): gl4es terrain VS 29 → 26 instructions, glmark2
slots 1292 → 1242. On the EA1 (uapi-stream, A/B against `noefochain`): the dEQP vertex
subset (5,357 tests) identical; glmark2 build/texture/shading equal or slightly up
(score 172 → 173); Minecraft renders correctly.

Still open, in order:
- uniform-only math per pixel or vertex. Measured with `nir_opt_preamble` before
  building it: glmark2 would lift 25 ALU ops over 7 programs (about 2% of its slots),
  the gl4es Minecraft set 3, glamor's gradients 15-34 per program (about 15-20%), its
  fills and glyphs none. The driver already runs a per-draw fragment secondary
  (`fs->sec`), so the work is the compiler's: the preamble as a second program sharing
  the main one's constant layout. The driver's contract for it (mesa-gallium,
  `fs_uniform_sec` in `tools/gen/mkusse.py`, DOUTU'd from the per-draw secondary task):
  the DDK's prologue (`phas.i #0, #0 [wait=7]`, `nop [nosched]`,
  `test.and.ne p0, #0, #0`, `(p0) ld.a.d pa0, pa0, #0 [moe]`, `wdf`, which waits for the
  constant DMA); the constants are the task's pa registers (the main program's `sa<n>` is
  its `pa<n>`), so results go to `pa<slot>`; the last instruction carries `[end]`;
  temporaries are its own r0.., sized by `sec_temps`. `struct sgx_shader` would carry
  `sec_code`/`n_sec_code`/`sec_temps`, and the slots it writes a const-map format 0x13
  that the DMA skips. Parked behind the items below;
- fixed-function fragment programs in F16 (`fs_mediump` didn't apply to Minecraft's
  programs; the cause is being checked in the driver);
- 8-bit colour math (`sopwm`, `sop2` on texels and lowp colours, as libusc does);
- the texel's channel swizzle in the sample, so a texel colour is 0 instructions.

## Kernel driver (powervr_sgx)

What a job costs on the CPU side, from a submit to its fence, measured with the
driver's tracepoints (`powervr_sgx:pvrsgx_kick`, `pvrsgx_irq`, `pvrsgx_job_done`,
`pvrsgx_flush_imports`) together with drm_sched's (`drm_sched_job_queue`,
`drm_sched_job_run`, `drm_sched_job_done`).

    drm/tools/pvrsgx-trace.sh OUT COMMAND...    # on the box: tracefs around a command
    drm/tools/pvrsgx-latency.py OUT             # per stage, per job type

The stages:

| stage | what it is |
|---|---|
| queue → run | drm_sched: the job's dependencies, its entity's turn, the scheduler's worker waking |
| run → kick | the driver: the microkernel command built, imports flushed, the kernel CCB written and the doorbell rung |
| kick → irq | the GPU and the microkernel, to the interrupt that reports the job |
| irq → done | the interrupt thread finding the job done, imports flushed again |
| done → finished | the hardware fence's callback signaling drm_sched's finished fence, which userspace waits on |

### Baselines (EA3, 2026-11-08)

RAM boot of 46c8212 (ea3-v2). Each job type was traced back to back, so the device
stays powered: no runtime-PM resume is in these numbers. A render is the frame
test's 256x256 clear and triangle, and a blit is the blit test's 64x64 copy. No
imports are mapped, so no flushes are counted. Medians in microseconds, p90 in
brackets.

| stage | render, DDK microkernel | render, open microkernel | blit, DDK | NULL, DDK | NULL, open |
|---|---|---|---|---|---|
| queue → run | 40 (84) | 43 (51) | 56 (79) | 71 (79) | 65 (81) |
| run → kick | 16 (23) | 19 (23) | 10 (20) | 18 (43) | 18 (18) |
| kick → last irq | 268 (271) | 214 (227) | 52 (59) | no interrupt | 21 (37) |
| last irq → done | 16 (31) | 30 (52) | 22 (33) | – | 21 (26) |
| done → finished | 2 (3) | 3 (4) | 3 (3) | 5 (5) | 32 (36) |
| **queue → finished** | **344** (412) | **315** (355) | **142** (179) | **132** (168) | **155** (177) |
| interrupts per job | 3 | 1 | 1 | 0 | 1 |

What the kernel adds: the scheduler 40-70 us, the kick 10-20 us, and completion
20-30 us after the job's last interrupt. That is under 100 us a job, against
200-270 us of GPU and microkernel time for even a trivial render. The DDK
microkernel raises three interrupts a render (TA done, 3D done, render done), each
of them a wake of the interrupt thread. The open one raises one. A NULL job on the
DDK microkernel completes when the CCB is consumed, so no interrupt, and the driver
finds it by polling.

Not yet measured:
- the import flush, which needs a run with simpledrm buffers (PRIME);
- pvrsrvkm on the same jobs. The DDK services path has no equivalent tracepoints,
  and the comparison there is end to end (mesa-gallium's per-frame numbers).

Trims to consider, with these numbers:
- wake the interrupt thread only when the oldest active job is done (it is two of
  the DDK microkernel's three interrupts);
- `queue → run`: drm_sched's worker wakeup is the largest kernel share. It is
  roughly constant, and only worth attacking if submits ever queue back to back.

## Minecraft's frame on the open stack (EA1, 2026-11-10)

Bench world, standing, Xorg + our x11 Mesa, kernel ftrace grouped by render target
plus a CLKGATESTATUS sampler (per-unit clock busy, about every 18 us).

Where the frame goes, default (3 ring slots): one world render (~40 ms TA, then
~23.5 ms 3D, no gap between them), two small renders and X's copy; every unit idle
39% of the time and TA/3D never overlapping, against the DDK's 11% idle and 40%
overlap. The DDK's hardware work per frame is not smaller.

| change | stand fps | world ms | all idle | what it removed |
|---|---|---|---|---|
| default | 11.5 | 66 | 39% | |
| `SGX_SLOTS=4` | 13.3-13.7 | 66 | 27% | recording the next frame waiting for this frame's world render (slot wait, ~40% of the game thread) |
| `SGX_SLOTS=6` | 13.5 | 66 | | nothing more |
| parameter buffer 4 / 16 / 32 MiB | 10.5-11.2 | 65-68 | | nothing: no OOM at any size |
| `ffmediump` | 10.9 | 64 | | nothing |
| `phasesplit` | 11.2 | 65.6 | | nothing measurable |
| `phasesplit,ffmediump,f16samples` | 11.8 | 61.4 | | ~3.5 ms of 3D (F16 texels, no unpacks) |

- Three ring slots and three renders a frame keep one frame in flight: the game
  waits for the world render before recording the next one. Four take the GPU from
  67% to 96% job-busy. With deeper queues X's copies wait behind the game's frames
  (Xorg in dirtyfb on simpledrm): an X request's median latency is 1.85 s at four
  slots against 166 ms at three, with or without a high-priority X context
  (5721e6f, `SGX_CTX_PRIORITY=high`). X's copy of the game window waits for the
  game's newest frame, and dirtyfb blocks X's main thread on that copy at every
  present. The sysfb damage commit stopped blocking (m2-drm e874e6f): X latency
  at four slots is 23 ms median (20-53, n=20) against 25 ms at three, no damage
  lost, Minecraft 13.6 / 11.0 fps standing / walking against 12.6 / 9.2 at
  three. Four slots are the default since (`SGX_SLOTS=3` for the old
  behaviour); gma500 never had the wait.
- What is left of the idle at four slots is the vendor microkernel's too: 5.3%
  all-idle on ours against 6.5% on the vendor's, same TA and 3D a frame (a
  steady window; an earlier warm-up window had suggested 1-10 ms of
  kick-to-TA a job). Serial job turnaround is at parity.
- `phasesplit` and `f16samples` are correct (dEQP-GLES2 mustpass and 117
  fixed-function piglit tests match the baseline, no channel swaps) but the 3D is
  not where this frame's time goes. F16 fixed-function programs and F16 samples
  are the default since dEQP-GLES2 mustpass and piglit matched the baseline with
  both (`noffmediump`, `nof16samples` turn them off). The phase split is the
  default too (piglit identical with it, dEQP clean, no slower;
  `nophasesplit` turns it off): neutral in Minecraft, but it is the DDK's form
  and spares overdrawn alpha-tested pixels their colour work.

### Where the world render's TA and 3D go, and the plan

Per frame at render distance 4, standing (CLKGATESTATUS, slots 3): world TA
~40 ms, then world 3D ~22 ms, no gap between them; two small renders and X's
copy after. Standing fps is now 12.6-13.4, the DDK's 12.8 (under Xvfb).

What does not move the world TA (each within noise, +-2 ms):

| tried | what it changed | TA |
|---|---|---|
| vertex program +3 instructions (`noefochain`) | shading per vertex | same |
| gathered f32 x 4 fetch (`nozerocopy`) | bytes and streams fetched | same |
| a TA state block per draw vs one per run of equal draws (af18996: 94% skipped) | state the TA writes into the tiles | same |
| TSP precision mask as the DDK's (`tspf16`) | the parameter buffer's per-vertex format | same |
| parameter buffer 4 / 16 / 32 MiB | partial renders (none at 16) | same |

The TE's configuration (AA, macrotiles, TE screen, region headers, tail
pointers, the PB descriptor and thresholds, render details) is the DDK's word for
word on the services path; only TA flags 2 differs, with the scene structure (the
DDK's world is the first kick of a three-kick scene).

What does: render distance 2 instead of 4 takes the TA from 40 to 15 ms and the
3D from 22.5 to 16.5. The TA's time is the tiling of the primitives it is given
(~160K triangles, 486K indices into 258K vertices a frame, the DDK's count): the
hardware's rate for this scene, not something per draw or per vertex the driver
adds. The DDK's is no faster: its TA clock runs ~77 ms a frame, ~41 ms of it
alone and ~35 ms under its 3D (the same sampler on the DDK's Minecraft), so the
DDK's lead in busy time is overlap, not a quicker TA.

The plan, largest first, with what each is worth:

| item | owner | worth (frame ~75 ms) | state |
|---|---|---|---|
| four slots by default: the next frame recorded while this one renders | kernel (simpledrm's dirtyfb), then Mesa | +8-20% fps (13.6 vs 12.6 standing, 11.0 vs 9.2 walking) | done: the default, after m2-drm e874e6f made the damage commit nonblocking (X 23 ms median) |
| TA/3D overlap across frames (the next world TA under this 3D) | open-ukernel + kernel | up to the 3D's ~22 ms with four slots; ~2% at three | parked: recovery storms with `overlap=1` |
| fewer renders a frame: the small renders after the world (FBO blit, GUI) and X's copy, each a job with its own start cost | Mesa (scene merging, as the DDK's 3-kick scene), X flips | ~5-10 ms (their run time plus a kick-to-TA each) | to do |
| 3D: F16 fixed-function and F16 samples (done, ~3.5 ms); phase split (done, neutral) | usse-compiler | done | default |
| TA rate itself | none (hardware) | the DDK pays the same | |

### Where the 3D goes, against the DDK's (offline, 2026-11-11)

From captures already taken, with no new box time:
- the CLKGATESTATUS sampler (`caps-ddk/2026-11-10-busy`, 30 s each, standing at
  render distance 4, three slots);
- the pvrtrace captures of one steady frame: ours on the services path
  (`2026-11-10-mcworld` A, kicks 2506-2510) and the DDK's (`mc-31091`, kicks
  4021-4025).

**The unit split can't separate ISP, TSP, texturing and PBE.** Inside our world
render's 3D, the ISP, PBE, PDS, USSE and cache clocks are on 98-99% of the time.
The TSP, iterator and texture clocks are on 87-88% of it, as one group, so all
the sampler shows is a TSP-side group that is idle 12% of the 3D. The DDK's 3D
shows the same picture.

**Per frame, our 3D isn't slower than the DDK's in clock time:**

| | ours | DDK |
|---|---|---|
| 3D clocked | 244 ms/s at 8.3 frames/s = 29.4 ms a frame | 423 ms/s at ~11.2 frames/s (TA 866 ms/s, ~77 ms a frame) = ~38 ms a frame |
| of it with the TA clocked too | 1% | 94% |
| world render's 3D | 23.9 ms (median) | not separable: it runs under the TA |
| other 3D a frame | 5.1 ms, in 4 renders: 3.0 + 0.9 + 1.0 + ~0 ms | mostly under the TA |

The DDK's 3D shares the USSE with its TA, so its clock time runs long; its 3D
alone isn't measurable here. Its lead is still overlap: it hides its 3D under
its TA.

**The world render's state matches the DDK's except in the iteration.** About
99% of both world renders' indices go to one terrain class. Both use
punch-through, `lequal`, depth write on and colour mask `f`. Both sample the
512×256 BGRA8888 atlas (nearest, repeat, no mipmaps) and the 16×16 lightmap
(linear, clamp), with projected samples. Both pixel programs are 17 instructions
of F16 math ending in `atst8`. Ours uses 0 temporaries against the DDK's 8.

The DDK keeps both textures linear (layout 3); ours are twiddled (layout 0),
which should be no worse for the texture cache.

The difference is the PDS:

| | DOUTIs per pixel | iterated into PA |
|---|---|---|
| DDK | 3: `iterate tc0 x4 f16; sample tc1 proj`, `iterate tc3 x1 f16; sample tc2 proj`, the colour (`src22 x3`) | 3 dwords plus the 3 of the colour |
| ours | 7: `iterate tc0 x4 f16`, `iterate tc1 x1 f16`, `iterate tc2 x4`, `iterate tc3 x4`, `sample tc2 proj`, `sample tc3 proj`, the colour | 11 dwords plus the 3 of the colour |

Our program never reads `tc2` and `tc3` (pa3-pa10). They're the two sampled
coordinates, iterated as F32×4 on top of the samples that iterate them again.
The compiler's `pds_samples_alone` (89fab5db) is meant to drop such iterations,
but `sample_only_load()` refuses any projected sample (`nir_tex_src_projector`,
`c->proj`). st's fixed-function programs sample with TXP, and so does all of
Minecraft's terrain. On top of that, the driver merges a sample only into a
plain DOUTI of the same set, while the DDK merges a sample into the previous
iteration of another set.

**The other renders: a clear on its own, and ZLS traffic the DDK has none of.**

| | ours | DDK |
|---|---|---|
| renders a frame | 4: the window's clear (1 draw); the world, into the FBO; the window (the FBO blit, one draw, a depth quad); the present copy | 2: clear, world and GUI into the FBO in one scene; the window (clear, blit, depth quad) in one render. The present goes as a transfer. |
| clear | its own render: 1.0 ms of 3D, depth and stencil stored | drawn in the next render's scene |
| world render ZLS | `001710b4`: depth and stencil stored at every tile, never loaded again | `001440b0`: no store, no load |
| window render ZLS | `001770b6`: loaded and stored. Its background object also loads the clear's colour. 3.0 ms. | `001110b0`: neither, and the clear is in the render |
| present | a 3D render, 0.9 ms | `SUBMITTRANSFER` |

Our ZLS buffer is stored at the end of every render (`zls_target`) and loaded
unless the render starts with a full clear. A target switch flushes a frame
that holds only a clear (`sgx_set_framebuffer_state`, then `begin_frame`). The
window render then has to load the colour and depth that the clear render
stored, and the depth it loads is overwritten by its own depth quad before
anything reads it. Each store or load moves the 720×480 ZLS buffer, 5 bytes a
pixel, 1.7 MB.

**The levers, largest first:**

1. **The terrain's dead iterations.** Let `sample_only_load()` accept a
   projected sample when the PDS does the projection: that's the sample
   `sample tcN proj` already emitted. Merge each sample into the previous plain
   DOUTI of another set, as the DDK does. This takes 8 F32 dwords and 4 DOUTIs
   off every terrain pixel, on 99% of the world render's draws, in the 3D's
   busiest group. The size is unmeasured; the sampler can't separate the
   iterator. An A/B of the world render's 3D, with and without the change, on
   the sampler gives it.
2. **Lazy clears.** Keep a frame that holds only full clears pending on its
   target instead of rendering it at a target switch. The next render of that
   target starts with the clear: no background load, no ZLS load. That removes
   the 1.0 ms clear render and the window render's colour and depth loads.
   Measured: the window render takes 3.0 ms against 0.9 ms for the present
   copy, a full-screen sample without loads, so ~2 ms of loads and stores. About
   3 ms a frame in all.
3. **Depth stores nothing loads.** A window's depth buffer is undefined after a
   swap, so the window render needn't store it. An FBO's depth is dead once the
   next render of that FBO starts with a depth clear. Minecraft clears every
   frame, and the DDK never stores the world's depth. Storing only when a later
   render could load it needs either a hint (`glInvalidateFramebuffer`, which
   Minecraft doesn't use) or the store decided when that next render begins.
   Worth ~0.5 ms of the world render, a guess from the window render's loads.
   The window part is in lever 2's ~2 ms.

Beyond the 3D itself, the biggest lever is still TA/3D overlap: the DDK runs
94% of its 3D clock under its TA. Fewer renders a frame (the present as a
transfer or a flip, 0.9 ms) is in the plan above.

**Gaps.**
- Overdraw and per-tile object counts need the parameter buffer after the TA.
  The pvrtrace snapshots are taken at kick time, before it runs.
- Capture A is a services-path build of 2026-11-10. Lever 1's cause is
  unchanged in today's compiler (`sample_only_load`). Lever 2's is unchanged in
  today's driver (`zls_target`, the target-switch flush).
