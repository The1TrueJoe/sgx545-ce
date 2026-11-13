# Minecraft FPS: the old stack against the new

Joseph asked for Minecraft frame rates on the new driver stack against the old one. CE5310 board, 2026-11-04
04:05 to 04:45 UTC, 3 runs of each stack, no GPU recoveries in any run. The box was put back as
found: normal boot, display stack running, mc-session stopped.

| | old: i386 Zulu 8, gl4es, DDK GLES2 on pvrsrvkm, Xvfb | new: x64 Zulu 8, GLX/DRI3 on Xorg, our Mesa on powervr_sgx |
|---|---|---|
| boot | the normal boot (7.1.8, pvrsrvkm) | RAM boot of m2-drm c95e32d; **DDK microkernel** (`firmware=` the DDK image) |
| standing, FPS: median (min-max) | **13.5** (13.2-14.0) | **6.1** (5.8-6.1) |
| walking, FPS | **16.3** (15.9-16.6) | **6.4** (6.3-6.5) |
| java CPU, standing / walking (% of one core) | 344 / 209 | 269 / 172 |
| GPU recoveries | 0, 0, 0 | 0, 0, 0 |
| F3's own counter at the screenshot | 16 fps | 5 fps |

Screenshots from the same spot and view, with F3 on: `img/2026-11-04-0430-mc-bench-old-gl4es-ddk.png`
(the framebuffer, `/dev/fb0`) and `img/2026-11-04-0430-mc-bench-new-glx-powervr-sgx.png` (the scanout
memory). The terrain, water and sky are the same in both; the new stack's terrain is textured.

## Method (`mesa/tests/x11/mc-oldnew.sh`)

The same for both stacks:
- **Game and world:** vanilla 1.7.10, no OptiFine, no tweaker. A fresh copy of the A/B rig's
  benchmark world (`/opt/mc-ab/bench-world`, a swamp) every run, in a game directory of its own,
  so the player's saves are never touched.
- **Display and memory:** 720x480, `-Xmx768M`.
- **options.txt:** render distance 4, Fast graphics, no clouds, smooth lighting off, VSync off,
  maximum frame rate unlimited (260), mipmaps 0, particles all.
- **Timing:** the same clicks into the world (a second between press and release; LWJGL samples
  the mouse once a frame). Then 30 s warm-up, 60 s standing at the spawn view, 60 s holding W.

What is measured:
- **FPS:** frames presented per second, as each stack counts its swaps. That is gl4es'
  `LIBGL_FPS` line on the old stack (`MC_LINEBUF=1`, so not buffered) and the driver's
  `SGX545_DEBUG=stats` line on the new; the mean of the intervals inside each window. F3's
  counter agrees in both screenshots.
- **CPU:** the java process's user and system time over the window.
- **GPU recoveries:** counted from the kernel log over the whole run: pvrsrvkm's
  `HWRecoveryResetSGX`/`Recovery triggered`, powervr_sgx's `job timed out`.

The old stack ran as the install runs it: its own `minecraft-ab` and `gl4es-run-ddk`, unchanged,
with the display stack stopped for the runs. The three new-stack runs are three consecutive runs
of the same command, which all wrote their files under run number 1; the numbers above come from
each run's printed results.

## Why the new stack is slower

The driver's stats on the new stack, per 2 s at about 6 FPS:
- **Vertices:** about 2,500-3,000 draws and 2-3.5 M vertices, about 250 k per frame. The vertex
  cache is never used (0 hits, 0 misses).
- **CPU:** about 40% of the render thread is in the draw path, gathering vertices.
- **GPU:** about 25% waiting for ring slots, with render latency of about 100-125 ms.

The old path gets about 75 k triangles a frame at this distance (the DDK measurements of
2026-10-27). gl4es hands the GPU indexed quads. On the desktop-GL path Minecraft's quads and
display lists reach the driver as unindexed triangles that bypass the vertex cache: three times
the vertices for the GPU, and a CPU copy of each one. It is driver work, so it went to
mesa-gallium:
- keep display lists' VBOs on the GPU;
- index quads instead of expanding them (the driver turns fans into lists today).

The desktop-GL path is also new here: these are its first numbers.

Not in this comparison:
- OptiFine, which the install's own launcher uses and would add to both;
- the open microkernel;
- Xorg runs with `MESA_GL_VERSION_OVERRIDE=2.1` (the server only) until the driver reaches GL 2.1.
