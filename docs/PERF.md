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
| c3 (174 shaders) | um-re 1900475 | 2098 | 2963 | 1206 | 1798 + 43 | 2277 | 219 |
| c3 | 894c995 | 1812 | 2585 | 496 | | | |

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

On the new stack, Minecraft draws through st's fixed-function programs, not through these
shaders. Those programs, and glmark2's shaders through both compilers, are the next rows.

### What closed the gap so far

| commit | change | corpus effect (all sets, F32 and F16) |
|---|---|---|
| f6cdfcd | `nir_opt_shrink_vectors`: components nothing reads go. Example: `MV * v` where fog reads only `.z` | 13586 → 13113 instructions, 19760 → 18566 slots |
| becc007 | `fold_movs`: an instruction writes the output or copy destination itself, so there is no trailing `mov` | → 12968, → 18257 |
| 887e523 | `place_in_pa`: temporaries in PA registers after the input's last read, as libusc does | temporaries 7910 → 5820 |
| 111d7e7 | 32-bit vecs and swizzles of SSA values are aliases, not register copies. Example: `vec4(x)` | → 12238, → 17489 |
| 894c995 | a comparison read only by selects keeps its `testmask` mask (no 1.0/0.0 `movc`); selects of F16 values move whole pairs (`movc.i32`); vregs shrink to the components used | → 11857, → 17018; temporaries → 4973 |

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
- **8-bit colour math.**
  - libusc adds and multiplies RGBA8 texels and lowp colours in U8/C10 (`sopwm`, `sop2`).
    `t_two` (sum of two textures) is 1 instruction.
  - We unpack every texel to F16 or F32, compute, and pack again: 11 instructions for
    `t_two`.
  - The sampler's channel swizzle (`zyxw`) also saves libusc the reorder.
- **Loops.**
  - We unroll every loop fully. `lp_nest` is 66 instructions against 11, but it runs no
    branches, so it is no slower.
  - libusc keeps small loops and predicates the exit.
- **In-place repeats.**
  - libusc writes `fmad pa0, pa0, pa4, pa8 rpt=4`. We keep a repeated instruction's
    destination apart from its sources, which is safe but costs registers.
