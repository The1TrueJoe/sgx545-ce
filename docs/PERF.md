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
- GL asks of fixed-function colour math only the framebuffer's precision, so this is in spec for
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
- **Colour sources** (f2247cb): a single instruction reads before it writes, so the colour's
  sources may sit in the colour register.
- **In-place repeats** (083bdd0, 65e768e): a repeat whose source dies there, read at the
  component it writes and stepping with it, writes over it. That is libusc's
  `fmad pa0, pa0, pa4, pa8 rpt=4`: iteration k reads component k before writing it, and no later
  iteration reads an earlier one's write. It applies to vregs and to dead PA inputs.
- **Per-register PA liveness** (65e768e, a919daa): each register of a vreg is placed by its own
  first and last use. 32-bit and F16 vregs both.
- **mediump demotion** (a919daa): an op over mediump operands runs in F16, as GLSL ES allows.
  Mesa had left `a * b` of mediump varyings in f32.
- **F16 colour pairs** (a919daa): (g, r) / (b, a), one register per `pck`.
- **`fdp`** (77632f3, 649281d): it writes only its last iteration's register, once and after its
  reads. So the operand names that register alone, and it may land on its own inputs.
- **Output registers** (60c90ff): a vertex shader's temporaries go in output registers before
  their first write, as libusc does (`fmad o0, pa0, sa18, o0`). The `mat4 * v` accumulator ends
  in `o0`.
- **Colour clamp** (649281d): `clamp(colour, 0, 1)` goes, since the U8 `pck` saturates.

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
- **8-bit colour math.**
  - libusc adds and multiplies RGBA8 texels and lowp colours in U8/C10 (`sopwm`, `sop2`).
    `t_two` (sum of two textures) is 1 instruction.
  - We unpack every texel to F16 or F32, compute, and pack again: 11 instructions for
    `t_two`.
  - The sampler's channel swizzle (`zyxw`) also saves libusc the reorder.
  - Example: `gl_FragColor = texture2D(s, uv)` (`tx_nd_*`) is 0 instructions for libusc. The
    sample is swizzled into the colour's byte order and its PA register is the colour.
  - We spend 4 (unpack, pack). Closing it needs the driver to take a sample swizzle and the
    colour in the sample's register.
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
    slots instead of 16. `SGX545_COMPILER_DEBUG=nocoissue` turns it off. 32 `efo` in c2+c3.
  - Driver mode, issue slots: corpus 7791 → 7726, glmark2 2080 → 1826 (temporaries 195 →
    157), gl4es Minecraft 180 → 150. Instructions go up (glmark2 1211 → 1470): a repeat is one
    instruction, its `efo`s are several.
  - Not done: `fdual`. Only 75 scalar f32 ops in the corpus could pair, and fmaxmin as a
    secondary is still open (ISA.md "Dual issue").
- **In-place repeats.**
  - libusc writes `fmad pa0, pa0, pa4, pa8 rpt=4`. We keep a repeated instruction's
    destination apart from its sources, which is safe but costs registers.

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
| done → finished | the hardware fence's callback signalling drm_sched's finished fence, which userspace waits on |

Numbers: pending (next EA1 slot).
