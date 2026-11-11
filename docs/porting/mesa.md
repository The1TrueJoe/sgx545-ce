# Bringing up a PowerVR SGX GPU in Mesa

This is the order the SGX545 came up in, on the Intel CE5300 and Cedarview,
written so the next core takes less time. Each stage says what to establish,
the tool to establish it with, what "done" looks like, and the traps we hit.
The details live in the linked documents; this page is the map.

The kernel side (powervr_sgx, its firmware and its uAPI) comes first:
`docs/DRM-DRIVER.md`, `docs/UAPI.md`, `docs/UKERNEL.md`. Everything below
assumes a kernel that reports the core id and revision and runs a render job.

## Stages

### 1. Winsys and the kernel interface

- **Establish:** buffer allocation in each heap, mapping, a render job
  submitted and waited for, the frame rings (`docs/WINSYS.md`).
- **Tool:** `sgx_frame_test` (built with `ninja sgx_frame_test`), which runs
  the winsys without Mesa.
- **Done:** a job completes, and its fence signals once per job.
- **Traps:**
  - A completion wait that polls in fixed steps quantizes every frame to
    them. Wait on the kernel's event (`poll()` on the fd for powervr_sgx) or
    pre-sleep to the expected render time (`docs/PERF.md`, "tex gap").
  - Nothing may sit at a heap's first byte: a state word of 0 would name it.

### 2. The first clear

- **Establish:** a render target, its TA/3D state, the end-of-tile program,
  the background object (`docs/STATE.md`, `docs/PDS.md`).
- **Tool:** the frame generator (`tools/gen`, `docs/GEN.md`) builds every
  buffer of a frame from GL-level parameters. Compare its output word by word
  with your own.
- **Done:** the clear color in memory, readable from the CPU.

### 3. The first triangle

- **Establish:** the vertex fetch (PDS DMA), a fixed vertex and pixel
  program, the TA state block, the VDM stream.
- **Tool:** `tools/state/decode.py` (with `--usse` to disassemble the
  programs) and `tools/state/build.py --check` for the formulas that rebuild
  the state.
- **Done:** one flat-shaded triangle in the right place.
- **Traps:**
  - Vertices per USSE task must follow the core's rule
    (`sgx_core.vertex_task_*`). Half of it hangs the GPU.
  - A PDS DMA longer than the core's chunk loses data
    (`sgx_core.dma_chunk_dwords`).
  - Iterating a varying the vertex program doesn't write locks the GPU.

### 4. Textures

- **Establish:** formats, twiddled and strided layouts, mipmaps, cubes,
  uploads (`docs/TEX.md`).
- **Tool:** dEQP `functional.texture.*`, piglit `texwrap` and `fbo-*`.
- **Done:** dEQP's texture groups at the vendor driver's pass rate.
- **Traps:**
  - Some cores hang when sampling from outside the texture heap
    (`sgx_core.sample_texture_heap_only`).
  - Mipmapped NPOT textures and large NPOT cube maps lock the 3D
    (`sgx_core.npot_mip_hangs`).
  - Rectangle textures arrive from Mesa's fixed-function path unnormalized.
    The driver lowers them itself (`lower_rect`, then `textureSize` from
    driver constants). Compositors bind their screen pixmap that way:
    without it, xfwm4's GLX compositor showed one flat color. Test:
    `mesa/tests/x11/glxtfp.c -s 720x480 -r`.

### 5. The compiler (NIR to USSE)

- **Establish:** register allocation, constants in SA, varyings into
  coordinate sets, texture samples (PDS or dependent), discard
  (`docs/ISA.md`, `docs/COMPILER-UPSTREAM.md`).
- **Tools:**
  - `SGX_DUMP_CODE=dir` writes every program (`.code`, and `.code1` for a
    split program's second phase);
  - `SGX_DEBUG=nir` prints the NIR each compile gets;
  - `tools/isa/dis.py` disassembles.
- **Done:** dEQP `functional.shaders.*` near the vendor driver's rate.
- **Traps:**
  - Going past the last SA register locks the GPU. Keep a guard that refuses
    the program.
  - The GLSL linker's varying packing breaks PDS samples: keep
    `MESA_GLSL_DISABLE_IO_OPT`.
  - Alpha-tested programs belong in two phases: the test before the
    visibility feedback, the color after, for the pixels that pass
    (`split_phases`).
  - F16 texel return (T3 01028800) skips the unpack. It's per sample, and in
    plain RGBA order even for a BGRA-swizzled unit.

### 6. Blending, depth, stencil

- **Establish:** blending as shader code, the ISP's depth and stencil words,
  ZLS load and store across renders (`docs/STATE.md`).
- **Tool:** dEQP `functional.fragment_ops.*`.
- **Done:** fragment_ops at the vendor driver's rate, with no recoveries.

### 7. EGL, GBM and DRI3 with the display device

- **Establish:** kmsro pairing of the render device with the display device.
  On Cedarview the GPU is an auxiliary device of gma500, which libdrm can't
  describe, so the loader synthesizes its PCI identity. Also dma-buf import
  and export, and lazy scanout.
- **Tools:** `eglinfo`, `glmark2-es2-drm`, `kmscube`.
- **Done:** a full-screen GL client on KMS, and buffers shared with X.
- **Traps:**
  - Lazy scanout migrates a render target to the display device only when a
    KMS handle is asked for, which gbm does at creation.
  - An import the display device can't map (no mmap offset on its export)
    fails late, at AddFB.
  - The pin path of an export can take a lock the attach path already holds:
    X hung silently in screen init.

### 8. X with glamor

- **Establish:** Xorg modesetting with glamor on the driver, GLX through
  DRI3.
- **Tool:** an Xfce or similar session, `glxinfo -B`, `glxgears`,
  `mesa/tests/x11/glxtfp.c`, a GLX compositor.
- **Done:** a desktop with a composited window manager, windows moved, a
  second login.
- **Traps:**
  - Glamor needs GL 2.1. Below it, glamor falls back to GLES2 and text
    renders as boxes. Keep sRGB on even with its known failures.
  - A blocking dirtyfb on simpledrm-like displays stalls X behind a game's
    queued frames. That's fixed in the kernel; measure X latency with deeper
    queues.

### 9. dEQP

- **Tool:** `mesa/tests/deqp` (build, run, expectations).
- **Done:** a full GLES2 mustpass run with no GPU recoveries and a triaged
  failure list.
- **Traps:**
  - Failures can depend on test order. Run a failing pair in one process
    before concluding.
  - A use-after-free in buffer copies showed as GPU faults only in some
    tests: zero-copy replaced a copy a pending draw still read
    (`docs/PERF.md`).

### 10. piglit

- **Tool:** `docs/PIGLIT.md`.
- **Done:** the `gpu` profile triaged, the suite run on every change that
  touches state or the compiler.
- **Traps:**
  - Some tests compare results only in a given environment. Run them alone
    and under the harness before calling a regression.
  - The kernel must free each process's buffers at exit. Back-to-back test
    processes otherwise run the machine out of memory.

### 11. Performance

- **Tools:**
  - `SGX_DEBUG=stats`: per-window FPS, flush and slot waits, renders by
    cause, TA state blocks sent and skipped;
  - `mesa/tests/perf/sgx_perfmicro.c` (fill, blend, tex, verts, rows);
  - a kernel ftrace of the job tracepoints;
  - a sampler of the per-unit clock status register for TA/3D busy time.
- **Traps, in the order we hit them:**
  - Too few frame slots keep one frame in flight. Four let the next frame
    record while this one renders.
  - Per-draw state was not the TA cost (the state reuse is still worth
    having).
  - The parameter buffer size was not the limit.
  - F16 fixed-function programs and samples trim the 3D; the phase split is
    neutral in our games.
  - `docs/PERF.md` has the tables.

## Measuring performance fairly

Most of our wrong turns were measurement mistakes:

- **Like for like.** A vendor run under Xvfb pays no present copy; one under
  Xorg does. Compare the same windowing path, or subtract it.
- **Make the frame finish.** A vendor `glFinish` on a pbuffer may only kick
  the TA. End each measured frame with a 1-pixel readback.
- **Steady state only.** Samples taken while a game is still loading
  mislead. Start after warm-up and check that the window's frame rate
  matches the game's.
- **Busy is not one number.** Job-level busy (from submit to completion)
  hides idle hardware inside jobs. Use per-unit clocks, and separate "TA
  alone" from "TA during the 3D" before comparing durations.
- **Check the kick pattern and the waits first.** Before diffing state
  words, count kicks per frame and find where the CPU waits (slot waits,
  fence waits, present).
- **One variable at a time, with noise measured.** Two default runs differ
  by a few percent; don't call smaller differences.

## Adding a core

The Mesa side of a new core is one entry in
`mesa/src/sgx/common/sgx_core.c`, plus what its generation needs from the
compiler. Mesa finds a core by the kernel's core id and revision. A core
without an entry is refused ("has no description").

### The entry

Copy the SGX545 entry and change what differs. Say how you know each value in
its comment: **[HW]** measured on the core with the open stack, **[D]** from
the vendor driver's behavior, **[G]** a working guess.

| field | how to establish it |
|---|---|
| `name`, `id` | the kernel's core id, bits 16-31 (`dmesg` prints it) |
| `rev_min`, `rev_max` | the revisions you tested; widen only with a test on each |
| `usse` | `SGX_USSE1` for SGX520-545, `SGX_USSE2` for 543/544/554 |
| `verified_on` | the machines your dEQP run and desktop session used |
| `max_texture_size`, `max_cube_levels`, `samplers` | the vendor driver's limits; then piglit `max-texture-size` |
| `vs_inputs`, `vs_outputs`, `fs_inputs`, `coord_sets` | the vendor driver's limits; dEQP `functional.shaders.*` at the limit |
| `gl_max_temps`, `temps` | GL's limit, and the register file the compiler allocates |
| `rt_max_*` | only for kernels that don't report them |
| `line_width_max`, `point_size_max` | the vendor driver's ranges |
| `vertex_task_dwords`, `vertex_task_max` | the vendor driver's VDM words at several output sizes |
| `dma_chunk_dwords` | the largest PDS DMA that copies correctly (`sgx_frame_test`) |
| `f16`, `f16_sample_return` | false until a dEQP run with them is clean |
| `sample_texture_heap_only`, `npot_mip_hangs` | true until shown safe: these steer around hangs |

Anything else that differs between cores becomes a field of `struct sgx_core`
read in one place, never a test of the core's id.

### The compiler

- **USSE1 cores** (SGX530, 531, 535, 540): the same instruction families as
  the SGX545. Expect differences in register files and in which instructions
  exist (F16 and C10 arithmetic, efo co-issue, fdual, PDS samples, constant
  memory). Each becomes a field the compiler reads through
  `sgx_compile_options`. Check whether your core encodes any instruction
  differently against the shared ISA description (`mesa/src/sgx/isa`).
- **USSE2 cores** (543, 544, 554): a different instruction set. That means a
  separate ISA description, and selection and scheduling of their own: a
  larger port. Start with the ISA description and its assembler and
  disassembler tests.
- The state words (`mesa/src/sgx/csbgen`) and PDS programs
  (`mesa/src/sgx/pds`) are per family too.

### Evidence

A new entry is merged with:

1. `glxinfo -B` (or `eglinfo`) showing `GL_RENDERER: PowerVR <name>`;
2. a dEQP-GLES2 mustpass run on the core, with the results file attached: no
   GPU recoveries, and the failures listed;
3. a desktop session (X with glamor, a GL client, a window moved), with a
   screenshot;
4. the machines in `verified_on`, and the kernel and firmware versions.
