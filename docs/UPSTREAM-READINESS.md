# Upstream readiness

What has to be true before the driver stack goes to upstream review. This
covers code quality, not merge requests: none are written yet.

The platform is the Intel Atom CE5300, so code, comments and docs say
"CE5300". The aim is one gallium driver for the SGX5xx family. Core features
come from the kernel (`DEV_QUERY`) and a per-core table, not from SGX545
constants spread through the code.

Other components track their own readiness in their own docs
(`docs/DRIVER-UPSTREAM.md`, `docs/COMPILER-UPSTREAM.md`); this file collects the
cross-cutting status and the firmware section.

**Who edits what.** Each section has one owner, and only that session edits
it. To change another session's section, message its owner. A new area gets
a new section at the end of the file, never in the middle.

| section | owner |
|---|---|
| this intro and note | main |
| Mesa pipe driver and compiler | mesa-gallium (compiler items: usse-compiler) |
| Firmware | open-ukernel |
| Mesa winsys and platform | uapi-stream |
| Docs: neutral platform wording | ta3d-state |
| Kernel driver (powervr_sgx) | m2-kernel-driver |
| Clean-room trees (isa, pds, csbgen) | cleanroom-isa |

## Mesa pipe driver and compiler


Scope: `mesa/src/gallium/drivers/sgx` (the pipe driver), the gallium half
of patch 0001, 0002 and 0004, by mesa-gallium. The winsys, the EGL platform
and the loader/kmsro patches are uapi-stream's section below; `sgxws.h` is the
seam, kept stable during the pass. Items M1, M4 (winsys files) and M9 sit at
the seam and are listed here for completeness. The compiler
(`mesa/src/sgx/compiler`) is covered by `docs/COMPILER-UPSTREAM.md`.
`docs/DRIVER-UPSTREAM.md` (2026-11-01) is the earlier hardware-fact audit; its
doc gaps still stand. Status as of 2026-11-08, um-re 0507229 plus mg-perf.

| # | item | status | owner |
|---|---|---|---|
| M1 | upstream build is powervr_sgx only | done (8c763d3 option; patch split, W1/W2) | uapi-stream |
| M2 | features from DEV_QUERY and a core table | done: queries (W3), sgx_device.c table on sgxws_dev_info | mesa-gallium (table), uapi-stream (queries) |
| M3 | driver name for the family | decided: `sgx` (Joseph, 2026-11-08) | mesa-gallium |
| M4 | clang-format | driver clean (3320225); winsys: sgx_frame.c etc. drifted ~60 lines (uapi-stream) | mesa-gallium (driver), uapi-stream (winsys) |
| M5 | comments and naming | driver done (mg-perf b42aa0d) | mesa-gallium |
| M6 | debug options and experiment code | driver done (2026-11-11); winsys fields left (uapi-stream) | mesa-gallium, usse-compiler |
| M7 | tests in Mesa's format (`ci/`) | suites in (2026-11-11); expectations to refresh from the next full runs | mesa-gallium |
| M8 | `docs/drivers/sgx.rst` | written: mesa/docs/drivers/sgx.rst | mesa-gallium |
| M9 | build inputs from `tools/` | open | uapi-stream, usse-compiler |
| M10 | sRGB textures right (GL 2.1 is advertised) | render targets and one-level filtering fixed (b37aec7, 2a52bdf), on hardware next; border color open | mesa-gallium |

### M1. The upstream build is powervr_sgx only

- **Done.**
  - The winsys reaches the kernel through a backend ops table (`struct
    sgxws_kif`): `sgx_powervr_sgx.c` for powervr_sgx, `sgx_pvr.c` for
    pvrsrvkm.
  - The driver includes only `sgxws.h`. Its meson dependency has no kernel
    include path.
  - `sgx_frame.c` has no services structure layouts left.
- **To do.**
  - Make the pvrsrvkm backend an explicit meson option,
    `-Dgallium-sgx-pvrsrvkm=disabled` by default and downstream-only. Today
    the build switches on whether `libpvr/pvr.c` exists (`fs.exists`), while
    `sgx_frame.h` already talks of a meson option.
  - `sgx_pvr.c`, `libpvr/` and the `include/sgx-kernel` link are built
    from the GPL kernel's headers. They stay out of the upstream tree: an
    downstream patch on top, deleted by removing two files and one meson block.
  - Backend-specific debug knobs move into that backend: `flushrt`, which
    sends `SGX_FLUSH_HW_RENDER_TARGET`.
  - `SGX_TRACE`/`SGX_DRY_RUN` write a pvrtrace log for
    `tools/decode`. It is a development aid either way: decide whether it
    stays as a documented debug option or becomes downstream-only.
  - `resource_from_user_memory` (the fbdev framebuffer, services only) and
    the EGL fbdev platform stay downstream.

### M2. Features from DEV_QUERY and a core table

The driver hard-codes SGX545 values. `sgx_screen.c` has:

- `max_texture_2d_size = 2048`;
- `max_render_targets = 1`;
- 8 samplers;
- VS inputs/outputs 16/11, FS inputs 10;
- `vendor_id`/`device_id` 8086:089b.

The compiler and winsys assume SGX545 encodings throughout.

Plan:

- Add a device-info table keyed by `core_id`/`core_revision` from
  `DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET`. It holds the architecture facts
  the kernel does not report: ISA variant, sampler count, texture size, TPU
  formats, F16/C10, and the PDS/USSE limits.
  - The SGX545 entry is the only one known. An unknown core fails screen
    creation with a clear message rather than guessing.
  - SGX530/531/535/540 (USSE1) and SGX543/544/554 (5XT) differ in ISA and
    state words. The table records what each needs; filling it is per-core
    hardware work.
- Take what the kernel does report from `LIMITS_GET`: render-target size,
  samples, parameter buffer, TA kicks.
- Take the heaps and code-base indices from `HEAP_INFO_GET`. The winsys
  already does.
- Take PCI IDs from libdrm's device info (`drmGetDevice2`), not constants.
- The pvrsrvkm backend fills the same struct from its own queries, so the
  driver sees one interface.

### M3. Driver name (decision for Joseph)

`sgx` names one core. A family driver would be `sgx`: `drivers/sgx`,
`winsys/sgx`, `SGX_DEBUG`, `sgx_dri.so`. Renaming touches every file, the
loader patch (0005), kmsro (0006), the downstream packaging and every session's
scripts. It is cheap once, now, and expensive after upstream review starts.

### M4. clang-format

**2026-11-11.** Both directories already have their `.clang-format` and were
formatted. The driver and the fixed-program files are clean again after the
day's changes (3320225). Left: `sgx_frame.c` (41 lines), `sgx_frame.h`,
`sgxws.h` and `sgx_powervr_sgx.c` (a few each), to be formatted when the
winsys has nothing in flight. `powervr_sgx_kmd_stream.h` is generated, so
its generator should emit formatted code, or the file should be excluded.


Mesa's base style is `src/.clang-format`, which has `DisableFormat: true`.
Each driver opts in with its own `.clang-format`. The PowerVR code
(`src/imagination`) and most gallium drivers use 80 columns; d3d12 uses 120.

- Ours: `BasedOnStyle: InheritParentConfig`, `DisableFormat: false`,
  `ColumnLimit: 80`, in `drivers/sgx` and `winsys/sgx/drm`.
- Measured on mg-perf: 2,195 of 9,811 lines change at 80 columns, 974 at
  120.
- It is one mechanical commit with nothing else in it, done when no other
  session has winsys edits in flight (uapi-stream) to avoid conflicts.
- `ForEachMacros` comes from the parent (`u_foreach_bit` etc.).

### M5. Comments and naming

- About 50 comments in the driver, winsys and compiler name the boot
  image's application, the bench box or game titles (the Minecraft overlay,
  "EA1"). Rewrite each to say what GL usage triggers the behavior and which
  test covers it (dEQP/piglit name), and move the history to `docs/log`.
- "as the DDK does" comments: cite the `docs/` section instead
  (DRIVER-UPSTREAM.md §2).
- The platform is "CE5300" everywhere.

### M6. Debug options and experiment code

- **Done in the driver (2026-11-11).**
  - Dropped: `fsconst`, `nodrvconsts`, `tspf16`, `novs`/`nofs` (the fixed
    programs stay as the fallback for a shader the compiler refuses),
    `zerocopy` (the default on powervr_sgx; `nozerocopy` turns it off) and
    `SGX_TA_SPLIT`.
  - Folded into `draw`: `attrs` (the clear line now carries the frame's draw
    count) and `blits`.
  - Kept, documented in `sgx.rst`: `nir`, `draw`, `frame`, `sync`, `stats`,
    `serial`, `hangabort`, `flushall`, `novcache`, `nof16`, `noffmediump`,
    `nof16samples`, `nophasesplit`, `nozerocopy`, `nozls`, `nolazyscanout`;
    `SGX_DRY_RUN` and `SGX_FRAME_GAP_US` for bisecting a hang;
    `SGX_SLOTS` (frames in flight, 4 by default), which the install guide
    on issue #5 gives users as the latency setting.
  - `MESA_GLSL_DISABLE_IO_OPT` is no longer set by the driver: the
    compiler's NIR options carry `nir_io_dont_optimize_varyings`, a flag the
    GLSL linker reads (patch 0006).
- **Left.**
  - Winsys (uapi-stream): the `ta_split` and `tsp_f16` fields of
    `sgxws_dev_options`/`sgxws_draw` are now always 0, and
    `SGX_STATE_ALWAYS` and `SGX_PB_PAGES` read the environment directly.
  - `flushrt` moves into the pvrsrvkm backend (M1).
- **The compiler: done (1ba7774).** `SGX_DISABLE`, `SGX_F16_CONST` and
  `SGX_F16_ITER` are one `SGX_COMPILER_DEBUG` flag table (`nopa`, `noout`,
  ..., `nocoissue`, `nof16const`, `nof16iter`), read once per compile.
  `SGX_PRINT_PRE_I2F` is dropped. See C1.

### M7. Tests in Mesa's format

- **There.** `drivers/sgx/ci/` holds `sgx-{fails,flakes,skips}.txt`
  (dEQP-GLES2) and `sgx-piglit-{fails,flakes,skips}.txt`, in deqp-runner's
  format.
- **Done.** `ddk-*.txt` moved to `mesa/tests/deqp/ddk/` (1b86487).
  `deqp-sgx-sgx545.toml` (dEQP-GLES2 mustpass) and
  `deqp-sgx-sgx545-piglit.toml` (the `gpu` profile, `PIGLIT_NO_WINDOW=1`)
  in the shape lima's and freedreno's have. Mesa CI runs them once a farm
  has the hardware.
- **To do.** Refresh the expectations from full runs of the current dev
  (dEQP mustpass and piglit gpu on a CE5300 board), with `mesa/tests/deqp/mkexpect.py`.

### M8. `docs/drivers/sgx.rst`

Write it for Mesa's docs tree. It covers:

- supported hardware (SGX545 on the CE5300; the family plan);
- the kernel driver it needs;
- GL/GLES versions and extensions;
- the debug options table;
- known limitations: no 3D textures, NPOT cube restrictions, one render
  target, no MSAA; flat shading (glShadeModel(GL_FLAT)) is the primary
  colour only, through the TA's vertex colour source, the one source the PDS
  iterates flat; the secondary colour stays interpolated, and a flat generic
  varying (GLSL 1.30) has no hardware switch (a texture coordinate set
  ignores DOUTI b25-26); the last vertex provokes, though core Mesa always
  advertises EXT_provoking_vertex (GL_FIRST_VERTEX_CONVENTION is ignored
  until a provoking-vertex field is found, or the draw rotates each
  primitive's indices); depth textures are compared on the nearest
  texel only (the F32 depth isn't filtered) and aren't rendered into;
- how to run the tests.

### M9. Build inputs from `tools/`

`mesa/build.sh` generates `sgx_usse_progs.h` with `tools/gen/mkusse.py`.
Upstream, the fixed programs are assembled by the compiler's encoder at build
time (DRIVER-UPSTREAM.md §5.2, COMPILER-UPSTREAM.md).

Compiler half done (2026-11-11): `mesa/src/sgx/compiler/sgx_fixed.c` holds the
programs as assembly text, and `sgx_fixed_progs_build()` assembles them
through the isa library into a caller-owned struct. The result is
byte-identical to `mkusse.py`'s header: 42 programs, relocations included.
The gtest `sgx_fixed.programs_assemble` assembles and decodes every program.
It runs when a context is created, not at build time: `mesa/build.sh`
cross-compiles, and a build-time generator would need native builds of the
isa library, mesautil and isaspec. Left for the winsys: call it, then drop
`sgx_usse_progs.h` and `build.sh`'s call to `tools/gen/mkusse.py`.

### M10. sRGB textures (must fix before the MR)

The driver advertises desktop GL 2.1, so `EXT_texture_sRGB` is offered. 2.1
is X's glamor minimum: under 2.0 glamor fell back to GLES2, and text drew as
solid boxes (704f81a). Sampling decodes sRGB right, in the fragment shader,
but piglit's `ext_texture_srgb` fails on:
- LINEAR filtering: the TPU filters the encoded texels and the shader
  decodes the result, where GL decodes first (`texwrap formats`: 237 for
  255 next to a darker texel). Fixing it means filtering in the shader
  (four NEAREST taps, decoded, then mixed) for sRGB views, or a TPU sRGB
  format if the hardware has one;
- alpha test against an sRGB framebuffer (`fbo-alphatest-formats`, `fbo-srgb`);
- border color (`texwrap formats bordercolor`), as for every format.

Mipmap generation of sRGB textures passes since adecb99 (the device chain
past the last level).

**2026-11-11 (b37aec7, 2a52bdf, waiting for a hardware run).**
- `fbo-srgb` and `fbo-alphatest-formats` drew nothing: the driver refused an
  sRGB texture as a render target. It now renders through the linear format,
  which is right while `EXT_framebuffer_sRGB` isn't exposed.
- LINEAR filtering of an sRGB texture sampled at one level (one level, or no
  mipmap filter) is done in the shader: four texel-centre samples, decoded,
  then mixed (`lower_srgb_filter`). Mipmapped minification still filters the
  encoded texels, because the shader doesn't know the level the TPU picks.
- Border color fails for every format (CLAMP_TO_BORDER is drawn as
  CLAMP_TO_EDGE). The same four-tap path can choose the border color per tap.

The 40 failures are in `ci/sgx-piglit-fails.txt` under their reason. A driver
advertising 2.1 with these failing will be questioned, so they get fixed, not
listed, before the MR.

### Compiler (usse-compiler)

Scope: `mesa/src/sgx/compiler`. Status as of 2026-11-08, um-re 0507229
plus the compiler commits after it. Every step below was checked
byte-identical on the corpus, glmark2 and the Minecraft gl4es set (11,868
programs, F32, F16 and driver mode). `docs/COMPILER-UPSTREAM.md` is the
earlier licensing and provenance audit.

| # | item | status |
|---|---|---|
| C1 | debug options in a `debug_named_value` table | done (1ba7774) |
| C2 | clang-format | done (31f6f18) |
| C3 | NIR passes in Mesa's shape | done (69923e0, options asserted 2026-11-11) |
| C4 | unit tests under `meson test` | done (7e0421a; 22 cases 2026-11-11) |
| C5 | structure doc | done (`compiler/README.md`, 7e0421a) |
| C6 | no global mutable state | done (`hw_const`) |
| C7 | comments cite `docs/`, not libusc or the DDK | done (2026-11-11) |
| C8 | DDK-compatibility code out of the compiler | done (2026-11-11) |
| C9 | file size | open (optional) |

- **C1. Debug options.**
  - `SGX_COMPILER_DEBUG` replaces `SGX_DISABLE` and the two F16 switches.
    It uses `DEBUG_GET_ONCE_FLAGS_OPTION` and is stored in the compile
    context, so there is no raw `getenv` and no global state.
  - The flags are listed in `compiler/README.md`.
  - The dead `SGX_PRINT_PRE_I2F` dump and an unused helper are gone.
  - The compiler now builds without `-Wno-missing-prototypes`.
- **C2. clang-format.**
  - `mesa/src/sgx/.clang-format`: `InheritParentConfig` from Mesa's
    `src/.clang-format`, `DisableFormat: false`, 80 columns, as `src/asahi`
    and `src/freedreno` have it.
  - It sits above `compiler/`, `isa/`, `pds/` and `csbgen/`; formatting
    those three is cleanroom-isa's.
  - Trailing comments that wrapped were moved above their fields.
  - Still to do: Mesa's `.clang-format-include` needs a `src/sgx/**/*`
    line (a patch item) so CI enforces it.
- **C3. NIR passes.**
  - The backend's own passes take the shader, return progress and run
    through `NIR_PASS`: `lower_mediump`, `narrow16`, `uniform_formats`,
    `lower_tex_derefs`, `lower_uniforms`.
  - Rewrites are `nir_shader_{instructions,alu,intrinsics}_pass` callbacks;
    the block walks left are analyses.
  - The caller creates the shader with `sgx_nir_options()` (the driver's
    `get_compiler_options`, the gtest's builder; sgxc sets it on the
    standalone frontend's NIR), and `sgx_lower()` asserts it.
- **C4. Tests.**
  - `meson test --suite sgx` runs `sgx_compiler` (gtest) beside
    cleanroom-isa's `sgx_isa`.
  - Shaders are built with `nir_builder` and compiled; every word must
    decode through the isaspec disassembler.
  - 22 cases: varyings, positions and efo co-issue (and its chains), the
    int-select loop bound, an early-exit loop, partial colors, ceil,
    or-of-compares masks, textures (BGRA units, PDS-only samples, F16
    returns, a dependent sample), the color fast paths, mediump fmad16, lowp
    C10, discard in one phase and split in two, constants past SA (ld.a.d),
    and skipinv clear on what feeds a derivative.
  - Each case was checked to fail on the bug it guards (the new ones with
    `SGX_COMPILER_DEBUG=nomediump` and with the helper marking disabled).
  - The DDK corpus harness (`tools/usc`) stays in this repository.
- **C5. Structure doc.** `compiler/README.md` covers the files, the pipeline
  (lowering, analysis, selection, backend passes, allocation, encoding), the
  debug flags and the tests. Mesa's `docs/drivers/sgx.rst` (M8) can link
  it or take a section from it.
- **C6. Global state.** `hw_const()` built a static table on first use (a
  race between two contexts compiling at once). It is now a const table.
  This also fixes a latent bug: a literal equal to the old filler value
  0xdeadbeef no longer matches hardware constant 0.
- **C7. Comments.** Every comment that named libusc, the DDK or
  glslcompiler as the behavior matched now cites the docs section, corpus
  shader, capture or hardware test it rests on (byte-identical). Box and
  application names: none left in the compiler.
- **C8. DDK-compatibility code** (COMPILER-UPSTREAM.md §3).
  - Samplers: the compiler takes the caller's unit (`struct sgx_layout`), or
    else the sampler variable's binding. sgxc numbers unpinned samplers in
    order of use, as the corpus does.
  - Labels: the compiler numbers its own from 0. The USP writer
    (`tools/usc/sgx_usp.c`) keeps 3/4/5 for start, feedback and phase 1 and
    puts the compiler's after them.
  - `n_pa` counts the iterations and the color only. The USP writer adds the
    DDK driver's placeholder color iteration's register to the header.
  - The varying-to-set allocation stays. It is the compiler's allocation,
    and the driver uses it for both stages; the rule is in ISA.md "What a
    compiler needs".
  - Every step is byte-identical on the 11,868 programs.
- **C9. File size.** `sgx_compile.c` is 5.8k lines, the size of
  `ir3_compiler_nir.c`.
  - Splitting it into selection, register allocation and encoding files means
    exporting about 64 of its 128 functions (prefixed, with prototypes in
    `sgx_priv.h`).
  - Worth it only if review asks.

## Firmware (the open SGX microkernel, `ukernel/`)

The microkernel is MIT-licensed source (`ukernel/ukernel.py`) assembled to a
small binary a loader runs on the SGX core. Upstreaming has two destinations:
the **source** (this repository `powervr_sgx`, under `ukernel/`) and the **built
binary** (linux-firmware, loaded by the DRM driver).

| item | state |
|---|---|
| Reproducible build | **Done.** Pure-CPython assembler, no timestamps/paths/randomness; byte-identical `.bin`+`.json` on any machine, any CPython ≥ 3.7. One command: `ukernel/build.sh`. |
| Pinned toolchain | **Done (documented).** Only CPython ≥ 3.7; no external packages. Recorded in `ukernel/README.md` → Building. |
| MIT license | **Done.** `ukernel/LICENSE` + an `SPDX-License-Identifier: MIT` header on every source file. |
| Release manifest | **Done.** `core`, `version` (major.minor), `features`, `build_id`, sizes, entry offsets, patch sites. |
| Per-core identity | **Done.** The per-core config `cores/<core>.py:CORE_NAME` (selected by `build.py <core>` / `SGX_CORE`, default sgx545) names the target SGX core; build.py derives the binary name and manifest `core` from it, so SGX540/SGX531/… images never collide. |
| Feature-bit documentation | **Done.** `ukernel/README.md` → Feature bits (bit 0 CLEANUP_V2, bit 1 SECURE). |
| Build/feature README | **Done.** `ukernel/README.md`. |
| linux-firmware WHENCE entry | **Draft below** (not submitted). |
| Neutral docs pass | **Pending ta3d-state** — `docs/` and any Documentation text use CE5300/SGX5xx neutrally; dev-box history → `docs/log/`. |
| DPM out-of-memory handler | **Done.** The OOM partial-render handler is confirmed on real hardware — the last open functional gap; general workloads now run (e.g. Minecraft 15 min in-world, 0 GPU recoveries). Behaviour in `docs/UKERNEL.md`; bring-up in `docs/log`. |

### Draft linux-firmware WHENCE entry (not submitted)

The binary linux-firmware would carry is the **packaged** image the DRM driver
requests (header + feature bits applied by the driver's packaging tool over the
`ukernel/build.py` output), named per core as the driver requests it:

```
Driver: powervr_sgx — PowerVR SGX5xx (e.g. SGX545 on the Intel Atom CE5300)

File: powervr_sgx/sgx545_ukernel.bin
Version: 1.0
Source: https://github.com/The1TrueJoe/powervr_sgx   (ukernel/ — ukernel.py +
        cores/<core>.py + build.py; the DRM driver's packaging tool applies the
        header/feature bits)
License: MIT.  See LICENSE.powervr_sgx for details.
```

Naming (no project name upstream; one file per core; the driver's own
subdirectory): `powervr_sgx/<core>_ukernel.bin` — `powervr_sgx/sgx545_ukernel.bin`
here, `powervr_sgx/sgx540_ukernel.bin` for another core. The directory matches the
DRM driver's module/request path (`powervr_sgx/`, not `powervr/`). build.py emits
`<core>_ukernel.bin` from the selected core (`cores/<core>.py`); the driver's
`firmware=` request and its packaging tool use the same name, kept in sync across
build.py, the driver, the docs and the test scripts.

**Still needed before linux-firmware submission:**

1. **MIT license file entry.** linux-firmware carries each file's license text in
   its own tree and the WHENCE `License:` line references it. Add the MIT text as
   `LICENSE.powervr_sgx` (or reuse an existing MIT entry if the maintainers prefer)
   and point the `License:` line at it.
2. **Public source URL.** **Set** — the source is the monorepo
   `https://github.com/The1TrueJoe/powervr_sgx` (`ukernel/`), which must be publicly
   reachable at submission so a third party can reproduce the binary from it.
3. **Version scheme.** `Version` tracks the manifest `version` (the SGXMKIF
   interface major.minor); the manifest `build_id` identifies the exact build. Any
   change to the emitted bytes bumps the build, and the checked-in golden hash
   (`ukernel/golden/sgx545.sha256`) is what proves a given Version's binary. Source
   and built binary are versioned together.

### Not a blocker, noted
- **Done.** The microkernel source's inline bring-up provenance (box names and
  dates, session attributions, vendor-binary PC citations, internal dump/tool names)
  was thinned for upstream; the mechanism and field explanations stayed, and the
  removed history is preserved in
  `docs/log/entries/2026-11-11-ukernel-source-provenance.md`. Comments-only — the
  golden image is unchanged.

## Mesa winsys and platform

Scope: `mesa/src/gallium/winsys/sgx/drm`, the loader/kmsro/dril and EGL
patches, `mesa/build.sh`, and the test rigs under `mesa/tests`. Owner
uapi-stream. Status as of 2026-11-08.

| # | item | status |
|---|---|---|
| W1 | powervr_sgx is the upstream backend; pvrsrvkm downstream only | done |
| W2 | the Mesa patches as standalone commits | done |
| W3 | device info from DEV_QUERY (M2's queries) | done |
| W4 | generated headers and the uAPI header | done (stream packers); uAPI copy until the kernel merge |
| W5 | comments: no boards, apps or dates; DDK references | done |
| W6 | `sgx_frame_test` | done |
| W7 | Mesa logging and helpers | done |
| W8 | kmsro's render-node pick | open |
| W9 | test rigs: neutral names, no project names | done |

### W1. One upstream backend

- **Done.**
  - `sgx_powervr_sgx.c` talks to powervr_sgx and is always built.
  - `sgx_pvr.c` and `libpvr/` (the DDK's services driver, pvrsrvkm) build only
    with `-Dgallium-sgx-pvrsrvkm=true` (8c763d3, off by default).
- **Upstream.** The pvrsrvkm backend, its option and the EGL fbdev platform
  are not part of the submission. They live in `mesa/patches/pvrsrvkm/` and
  in two winsys files.

### W2. The patch series

- **`mesa/patches/`, the upstream series.** Each file is one Mesa commit with
  its own description.
  1. `gallium: add the sgx driver`: meson, the pipe loader's DRM descriptor,
     the DRI target, and a loader predicate that picks sgx for powervr_sgx.
  2. `sgx: add the SGX545 ISA trees and sgx-asm/sgx-disasm (-Dtools=sgx)`:
     the tools option, and `src/sgx` entered for it. Applies to Mesa alone.
  3. The isaspec changes (two commits). Apply to Mesa alone.
  4. `kmsro, dril: sgx as a render-only GPU`.
  5. GLES2 texture swizzle for glamor.
  6. `sgx: build src/sgx for the gallium driver` (needs 1 and 2).
- **`mesa/patches/pvrsrvkm/`, downstream only.**
  1. The EGL fbdev platform hooks.
  2. The loader predicate for pvrsrvkm.
  3. EGL surfaceless on a device with only a primary node (pvrsrvkm has no
     render node).
  4. The meson option.
- **Build.** `mesa/build.sh` applies both sets. The pvrsrvkm code builds
  only with the option or `-Dplatforms=fbdev`.
- **Checked.** Patched in order onto Mesa 26.2.5, the two sets give the same
  tree as before the split, apart from comment wording.
- **For the merge request.** The overlay (`mesa/src`) joins patch 1 or splits
  into driver, winsys and `src/sgx` commits.

### W3. Device info

- **Done.**
  - `sgxws_dev_info()` returns core id and revision, render-target limits
    and PCI ids:
    - from powervr_sgx's `GPU_INFO_GET` and `LIMITS_GET`;
    - PCI ids from `drmGetDevice2`;
    - under pvrsrvkm, the CE5300 core's values.
  - The heaps and code-base indices already come from `HEAP_INFO_GET`.
- mesa-gallium builds the per-core feature table on top (M2).

### W4. Generated headers and the uAPI header

- **The uAPI header.** `powervr_sgx_drm.h` is copied into
  `include/drm-uapi` by `mesa/build.sh`. Upstream, it is updated from the
  kernel's `include/uapi/drm` once the kernel driver is merged, as Mesa does
  for every DRM driver. Until then the MR carries a copy marked as such.
- **The command-stream packers.** Done: meson generates
  `powervr_sgx_kmd_stream.h` in `src/sgx/kmd_stream` from the stream XML and
  `gen_stream.py user`, as csbgen does for the state words. `mesa/build.sh`
  copies both there from `docs/uapi`, their one copy.
  - m2's `pvrsgx_*` rename (e90beba) covers the kernel's stream decoder only.
    The uAPI names (`powervr_sgx_drm.h`, `powervr_sgx_kmd_stream.xml`,
    `SGX_KMD_STREAM_*`) are unchanged, so Mesa has nothing to follow.
- **`sgx_usse_progs.h`** is gone (M9): the winsys assembles the fixed
  programs per context with `sgx_fixed_progs_build()`.

### W5. Comments

- **Done.** No boards, applications, dates or project names in the winsys.
- **Done.** The comments give a value's reason as the hardware fact, with a
  pointer to STATE.md, PDS.md or ISA.md and STATE.md's tags ([C] hardware,
  [D] capture, [G] guess), not another driver's output. The DDK is named
  only where the services backend (downstream) is described.

### W6. The winsys test

- **Done.** `sgx_frame_test` (the winsys without the gallium driver, a
  hardware check of the kernel interface) is no longer built by default.
  `mesa/build.sh` asks ninja for it.
- **Upstream.** Leave it out, or keep it as a non-default target if
  reviewers want a smoke test.

### W7. Logging and helpers

- **Uses Mesa's helpers already.** `util_vma_heap` for device addresses,
  `simple_mtx`, `util/macros.h`, `drmSyncobj*`.
- **Done.**
  - The winsys logs through `mesa_loge`, `mesa_logw` and `mesa_logi` (the
    `SGX_DEBUG=frame` lines), with an `sgx: ` prefix like the driver's.
  - The winsys reads no environment. `SGX_STATE_ALWAYS` and `SGX_PB_PAGES`
    are the driver's `debug_get_option` entries, passed in
    `sgxws_dev_options` like the others.

### W8. kmsro's render-node pick

kmsro takes a PCI render node when its kernel driver is powervr_sgx, because
on the CE5300 the display device (simpledrm) is a platform device and the GPU
a PCI function.
- **Cedarview.** The display (gma500) and the GPU sit on the same PCI
  function, and powervr_sgx binds to gma500's auxiliary device.
- **Done (782d399).** For a display device that isn't on the platform bus,
  the loader takes the render node whose sysfs device's parent is the
  display's own device, when its driver is in kmsro's list. It reads
  `/sys/class/drm`, because libdrm's device list leaves out the auxiliary
  bus. A unit test on fake sysfs trees runs in mesa-debian CI, and there's a
  `gma500_dri.so` stub.
- **Open.** Confirm on Cedarview hardware (stick v3) before the MR.

### W9. Test rigs

- **Done.**
  - `mesa/tests`, `mesa/patches` and `kit/` name no project and no board.
  - The box password comes from `BOX_PASS`.
  - The RAM-boot script is `mesa/tests/x11/ce5300-ramboot.sh`.
  - The firmware file is `sgx545_ukernel.bin`.

## Docs: neutral platform wording

Text that will go upstream names the platform and GPU neutrally: the Intel
Atom CE5300, the PowerVR SGX545 and the SGX5xx family. The dev-board history
moves to `docs/log/`. The replacements:
- the dev-board names (EA1, EA3, HC-800) become "a CE5300 board", or a plain
  dated hardware tag;
- shorthand for our microkernel ("UK", "our µK") becomes "the open microkernel";
- IP addresses, box workspaces and product names go.

Long bring-up narratives move to a dated `docs/log/entries/` file, with a
pointer left in their place.

Box-specific references, counted on 2026-11-08 (and again on 2026-11-11, when the last ones went) by
`grep -E 'Control4|\bEA[0-9]\b|EA-[0-9]|HC-800|10\.0\.0\.|Bench plug'` plus the
downstream distribution's name (which must not appear anywhere in this
repository, 2026-11-08).
The last column is the count of lines naming a branch, commit or log entry.

| doc | box refs | branch/log refs | status | owner |
|---|---|---|---|---|
| UKERNEL.md | 0 | 5 | done 1621077 (two narratives moved to `log/entries/2026-11-08-ukernel-bringup-history.md`) | ta3d-state |
| UKERNEL-DECISIONS.md | 0 | 0 | done 6e71335 | ta3d-state |
| SRVINIT.md | 0 | 0 | done 6e71335 | ta3d-state |
| STATE.md | 0 | 1 | done 6e71335 | ta3d-state |
| TEX.md | 0 | 0 | done 6e71335 | ta3d-state |
| PDS.md | 0 | 0 | done 6e71335 | ta3d-state |
| ISA.md | 0 | 0 | done 2026-11-11 | ta3d-state |
| DRM-DRIVER.md | 0 | 16 | done 2026-11-11 (log pointers kept: our own docs may keep them) | ta3d-state |
| UAPI.md | 0 | 1 | done 2026-11-11 | ta3d-state |
| GEN.md, WINSYS.md | 0, 0 | 0 | done 2026-11-11 | ta3d-state |
| COMPILER-UPSTREAM.md, DRIVER-UPSTREAM.md | 0, 0 | 1, 1 | done 2026-11-11 | ta3d-state |
| NOTES.md, PERF.md, PIGLIT.md, CLEANROOM-COVERAGE.md | 11, 26, 7, 15 | — | internal working notes, stay as they are (decided) | main |
| README.md | 0 | — | done 2026-11-11 | ta3d-state |
| PDS.md, UKERNEL.md, MAIN-MERGE.md, porting/microkernel.md | 0 | — | done 2026-11-11 (references that had come back since 2026-11-08) | ta3d-state |
| In-tree upstream text: `drm/Documentation/gpu/powervr_sgx/*.rst`, `mesa/docs/drivers/sgx.rst` | 0 | 0 | checked 2026-11-11: no board names, no downstream name, no commits, branches or log paths | ta3d-state |

Decided (main, 2026-11-08):
- **Internal notes stay as they are.** NOTES, PERF, PIGLIT,
  CLEANROOM-COVERAGE and `docs/log/` are this repository's working records and
  don't go upstream.
- **Upstream text names no commits, branches or log paths.** That covers
  `Documentation/gpu`, Mesa's `docs/drivers`, the uAPI header's kernel-doc,
  and anything copied from UAPI.md or DRM-DRIVER.md. Such text states the
  facts and their evidence directly. Our own `docs/` may keep the pointers.
- **Firmware file names follow the per-core naming, without a project name**
  (open-ukernel 69b4427, m2's next driver commit).
  - The build writes `<core>_ukernel.bin`, i.e. `sgx_ukernel.bin`, plus
    `.bin.pat` and `.json`. SRVINIT.md's `srvinit -o` example uses it.
  - The driver requests `powervr_sgx/sgx_ukernel.bin`, the same name in
    the driver's directory; UKERNEL.md §4a uses it.
  - The DDK image keeps its own name, `sgx545-ukernel-1.7.862890.bin`.

Later and optional: the inline hardware-provenance comments in
`ukernel/ukernel.py` (see the firmware section).

## Clean-room trees: `mesa/src/sgx/isa`, `pds`, `csbgen`

Owner: the clean-room implementation session. The USSE instruction set and
the PDS programs (isaspec), and the TA/3D state words (csbgen). Status as of
2026-11-11 (MR-readiness pass on dev 7c7d624 plus core-id-asm).

### Done

| item | state |
|---|---|
| Layout like Mesa's other isaspec users | `isa/` and `pds/` follow `src/etnaviv/isa`: the XML, the `isa.h` the generated decoder includes, custom targets for the decoder (`<name>-isa.c/.h`), the encoder (`-encode.h`) and the enums, a static library and an `idep_`, and `tests/meson.build` |
| Layout like Mesa's csbgen users | `csbgen/` follows `src/imagination/csbgen/rogue`: `<name>.xml` -> `csbgen/<name>.h` through the shared `gen_pack_header.py`, and the per-file loop is the same. `sgx_csb.h` plays the part of the Vulkan driver's `pvr_csb.h` (address type, includes, the pack-in-place macro). The XML files are renamed `vdm.xml`, `ta.xml`, `pbe.xml`, `tex.xml` |
| Tests under `meson test` | `meson test --suite sgx` runs `sgx_isa` (assemble -> encode -> disassemble over three lists; a line's given words must match), `sgx_pds` (the same for PDS, plus the DOUT data words), and the new `sgx_csb` (66 state words the documentation quotes, packed and unpacked). Together with `sgx_isa_tools` (sgx-asm and sgx-disasm over the lists, hex and binary) it passes 4/4 on a `-Dtools=sgx -Dbuild-tests=true` build (no gallium driver) of Mesa 26.2.5 with the whole series applied, with no compiler warnings. The csb test found an XML default that was wrong for one T3 form (tex2 clears b23); the test case now sets it. |
| Nothing outside `meson test` in the Mesa tree | `isa/tests/standalone.sh` (a library build without Mesa, which `tools/usc/check_enc.py` and `drm/tools/gen_usse_corpus.py` use) moved to `mesa/tests/isa-standalone/`, which isn't copied into Mesa. Its `.gitignore` went with it |
| Mesa style | one `mesa/src/sgx/.clang-format` for all of `src/sgx` (shared with the compiler): `InheritParentConfig`, `DisableFormat: false`, column limit 80, as `src/asahi` and `src/freedreno`. Every C file in the three trees is clean under clang-format 21 and 22 (2026-11-11). One macro list, the assembler's flag table, is hand-wrapped between `clang-format off/on`. Comments are in American English (`colour` -> `color`). `mesa/patches/0002` opts `src/sgx/isa`, `pds` and `csbgen` in to `.clang-format-include` (not `src/sgx/**/*`: `src/sgx/compiler` isn't clean yet) |
| License headers | every source file, XML, meson file, script and test list has `Copyright © 2026 Joseph Telaak` and `SPDX-License-Identifier: MIT` (the test lists as `#` comments). The READMEs carry none, as Mesa's don't |
| A README per tree | `isa/README.md` and `pds/README.md` now describe the tree rather than its history: what it is, the files, the generated files, the tests, the isaspec patch, what isaspec can't express, and who uses it. The answered-gap lists and the old encoder's story stay in `docs/log`. `csbgen/README.md` is new: files, use, conventions, tests |
| No DDK inputs | the build and the tests read only the XML, the test lists and Mesa. The test lists' words are the ones the ISA/PDS/state documentation quotes. Comments no longer name the DDK, a capture rig or USEASM, and the `#i16-fmt` enum nothing used (an unused-variable warning in the generated decoder) is gone |
| Unchanged output | after the restructure, the offline winsys harness (clears, draws, scissors, end-of-tile, over 60 frames) is still byte-identical to um-re 9145c84 (66.9 MB of rings). `mesa/tests/isa-standalone` runs the ISA lists: 324 instructions, 0 failures |
| Warnings | no warnings from the three trees on the host build (clang) or the zig x86_64 build. Since 2026-11-11 the PDS library needs no warning flag (isaspec commit 3, item 3), and the ISA-first tree builds with `-Dwerror=true` under clang and gcc 16 |

### Open

1. **Where the trees go.**
   - `src/sgx/` is a vendor-neutral top-level directory, like
     `src/etnaviv`. Imagination's code lives in `src/imagination/`, so
     reviewers may want `src/imagination/sgx/` (or `sgx/`).
   - The state XML could then join `src/imagination/csbgen/` beside `rogue/`
     and `volcanic/`. That needs `src/imagination/meson.build` to enter
     `csbgen` for the gallium driver too, not only for the Vulkan driver and
     tools.
   - A decision for the MR series, not for the code.
2. **The isaspec change** (`mesa/patches/0003`) goes upstream first. Done
   2026-11-11: the file is three commits in `git format-patch` form (`git
   am` applies them as three, `patch -p1` as one), the first two with a
   `docs/isaspec.rst` sentence (the third is item 3):
   - `isaspec: allow <param>s on derived fields of bitset type`;
   - `isaspec: encode: stop at the override that defines the field` (the
     decoder already takes the first override that holds).
   - Checked: the code generated for ir3, etnaviv and qrisc is unchanged by
     the first commit. With the second, etnaviv's and qrisc's is unchanged
     and ir3's encoder loses 120 lines that can't run (cat1's immediate
     overrides test exclusive `SRC_TYPE` values). Compare with a fixed
     `PYTHONHASHSEED`: upstream's encoder output already varies with it.
   - isaspec has no test harness, so each commit message says how it was
     checked: the generated-code comparison, and for the second, that the
     SGX545 test (`sgx_isa`) fails without it (2 of 324: the indexed F16
     operands lose their F16 flag; checked 2026-11-11).
   - Open for review: a reviewer may still ask for a minimal XML case, or
     for the second commit's `Fixes:` tag (the commit that introduced the
     override ladder; not looked up).
3. **The PDS decoder's unused tables.** Done 2026-11-11: a third isaspec
   commit (`isaspec: decode: leave out roots nothing decodes from`, in
   `mesa/patches/0003`). decode.py drops the roots decoding can't start
   from (neither `#instruction` nor a field's type) and their bitsets;
   encode.py still sees them. ir3, etnaviv, qrisc and the USSE decoders
   are byte-identical; the PDS decoder loses its six encode-only roots, and
   `-Wno-unused-variable` is gone. Removing only the root tables wasn't
   enough: `-Werror` then caught the bitsets under them
   (`-Wunused-const-variable`).
4. **The documentation the comments cite.** Done 2026-11-11: nothing in
   the three trees cites `docs/ISA.md`, `PDS.md`, `STATE.md` or tools
   outside Mesa any more. Each comment states the fact itself, and the
   text notation is summarized in `isa/README.md` ("Text notation"), which
   the tools and the assembler point at. The test lists say what their
   words are (known words) without naming a source.
5. **The tools.** `sgx-asm` and `sgx-disasm` (USSE) are built with
   `-Dtools=sgx`, as `etnaviv-disasm` is with `-Dtools=etnaviv`. The
   option is `mesa/patches/0002`, and the `sgx_isa_tools`
   test runs them. The microkernel's build is to assemble through
   `sgx-asm` (UKERNEL-TOOLING.md §2). Still open: PDS in `sgx-disasm` (today
   `sgx_pds_test -d`).
6. **csbgen's unknown bits.** Constant bits of unknown meaning are
   `unknown_<bits>` fields with defaults. csbgen also has `mbo`
   (must-be-one), which fits only bits that are always 1. A reviewer may
   prefer fewer named unknowns. The spec side tags each field known,
   only-constants-seen, or unknown (STATE.md "Field layouts for packing").
7. **PDS word 1 for one unit with no secondaries.** The winsys writes 0xb,
   but the documented value is 12 (`18030001`). The packers keep the
   winsys's bytes. Settling it is a driver change: winsys/driver owners.
8. **`src/sgx/compiler`** isn't covered by this pass (the compiler
   session's tree): no `.clang-format`, and it has its own tests.
9. **The meson hooks.** Done 2026-11-11: the ISA trees and tools can land
   before the driver.
   - `0002` (the tools option) applies to Mesa 26.2.5 alone and enters
     `src/sgx` for `-Dtools=sgx`; with `0003` and only `isa/`, `pds/`,
     `csbgen/` and `src/sgx/meson.build` it builds and passes `meson test
     --suite sgx` 4/4.
   - `0006` (`sgx: build src/sgx for the gallium driver`) adds
     `with_gallium_sgx` to that condition, so it needs `0001` and `0002`.
   - `src/sgx/meson.build` enters `compiler/` on
     `gallium_drivers.contains('sgx')`, so it doesn't need `0001`'s
     `with_gallium_sgx` either. In the MR the compiler arrives with the
     driver.
10. **The core id.** `ISA_GPU_ID()` is `core * 100 + format control`
    (`sgx_isa_gpu_id()`), and the root bitset is `<gen>`-limited to SGX545
    (54500-54599). isaspec offers no second parameter, so the core shares
    `gpu_id` with format control, as freedreno's `gpu_id` carries its chip.
    The SGX530 plan (agreed with the sgx530-ukernel spec side, 2026-11-11,
    not started): the root widens once to 53000-54599 (535 and 540 can't
    reach the encoder: `sgx_isa_core_supported()` refuses them), 545-only
    classes get 54500-54599, a form that differs is a same-name bitset per
    core, and a field that differs is an override on `ISA_GPU_ID() / 100`.
    `-c 530` covers only the classes ISA.md marks confirmed on the 530, and
    sgx-asm refuses a form outside the core instead of writing 0.
11. **Cross-tree paths.** Done 2026-11-11: `src/imagination/csbgen`
    exports `pvr_gen_pack_header_py` (as `src/intel/genxml` exports
    `gen_pack_header_py`), `src/meson.build` enters `src/imagination` for
    `-Dtools=sgx` too (`0002`; `0006` adds `with_gallium_sgx`), and
    `csbgen/meson.build` uses the variable and `inc_imagination`. On 26.2.5
    `0002` also moves `subdir('csbgen')` out of `src/imagination`'s driver
    condition, as Mesa main already has it. The other option (the XML
    beside `rogue/`, item 1) remains a location decision.
12. **Test form.** The tests are plain C programs plus one Python script
    under `meson test`, as `src/freedreno/ir3/tests` does. etnaviv's ISA
    test is a gtest; a reviewer may prefer that.

## Kernel (powervr_sgx)

Owner: powervr_sgx (m2). Covers `drm/powervr_sgx`, the gma500 patches in
`drm/gma500` and the uAPI header `docs/uapi/powervr_sgx_drm.h`. This is the
status as of m2-drm 3f36c8a4 (2026-11-12). That tree has:
- rev 3 uAPI step 1: per-command structs, D6 cache ranges and explicit sync
  only;
- the TLB-free VM release;
- the v0.1.1 fixes;
- every item of the 2026-11-11 presubmit review except B1.

The RFC is cut after sgx530-kernel's per-core restructure (`wip/multicore`)
lands on m2-drm. Joseph asked for this order.

Each open item says what it blocks. **RFC** means it is fixed, or disclosed
in the cover letter, before Joseph sends. **Merge** means it is needed only
before a v1 that can merge.

### Open: blocks the RFC

| # | item | state | plan |
|---|---|---|---|
| K1 | Per-core restructure | sgx530-kernel's series: a register-write and struct-bytes KUnit golden first (47374f27), then the core table and heap layouts with no change for the SGX545 | sgx530-kernel owns it. K2 and K3 follow on top, gated by the same golden |
| K2 | Firmware words written by index | The rest of the firmware structures are already C structs with `static_assert` offsets (`pvrsgx_fwif.h`). Left: the background object (`b[0..22]`, `pvrsgx_render.c` ~462 and ~802), the PB block header (`blocks[0..2]`, ~78) and `ta_data[0..2]`. 16 fields are `unk_*` with a known value and no known meaning | Structs for the three. Name every `unk_*` that the GPL headers give a meaning; keep the rest as `unk_*` with their value and say so. The golden must not change |
| K3 | Encodings as named fields | `pvrsgx_pds.c` builds PDS instruction words from shifts. `pds_word()` and `BG_PDS_MIDDLE` are in `pvrsgx_render.c`, and the microkernel's `limm` patch is in `fw_limm()` (`pvrsgx_fw.c` ~140) | `GENMASK` field definitions plus `FIELD_PREP`. The golden must not change |
| K4 | Comments that cite out-of-tree docs (review M12) | 5 came back with rev 3: `pvrsgx_cmd.c`, `pvrsgx_cmd.h`, `pvrsgx_cmd_checks.c` (2) and `pvrsgx_drv.h` cite `docs/uapi/rev3-commands.md` or `UAPI.md` | Put the facts in the comment or in `Documentation/gpu/powervr_sgx` |
| K5 | Device wedging (drm-uapi.rst) | A failed restart sets `sdev->lost`: the cores stay in reset, bus mastering is off, fences are signaled and ioctls return -EIO. No uevent is sent | `drm_dev_wedged_event(..., DRM_WEDGE_RECOVERY_REBIND)` on that path. The prerequisites already hold |
| K6 | fdinfo (drm-usage-stats.rst) | No `show_fdinfo` | `drm_show_memory_stats()` through the shmem helper's status, and per-client `drm-engine-render` time. Recent drivers have it, and reviewers ask for it |
| K7 | Completion by polling (review M8) | With a features-2 microkernel, completion is polled every jiffy (`pvrsgx_job.c` ~215); with DONE_SEQ (features 6) it comes from the interrupt, with a 100 ms backstop. Released v0.1.x kernels accept features 0x3 only | **Approved (2026-11-12).** The driver requires DONE_SEQ and the per-jiffy path goes. The order is in `m2-k7-firmware-baseline-plan.md` (notes). Step 2 is done: `sgx545_ukernel-m8-f6.bin` (ukernel-4b6a324e, md5 7079b4c3) loads as features 0x6 on a CE5300 board with one interrupt per job, and uAPI, hang, frame, blit and PRIME are right. It ships in v0.2.0, with the firmware/kernel Depends/Breaks pair |
| K8 | Runtime checkers on today's tree | The last clean single-process run under lockdep, KASAN, UBSAN, kmemleak and DMA-API debug was 57ca2d0 (2026-11-08). The three-process soak lost the box's userspace on 11-08, cause open | Running on a CE5300 board (`wip/debug-k8`). It adds DMABUF_DEBUG, the ww_mutex slowpath check, lockup and hung-task detectors, netconsole and a new `powervr_sgx_unplug` test. Runs: uAPI, frame, blit, hang, PRIME, runtime-PM rounds, unbind with a mapping open, `pm_test`, kmemleak, then the soak last (1, 2, 3 processes) |
| K9 | The series itself (Phase B) | The 2026-11-11 dry run is stale (rev 2 uAPI, before D6), and it built only from patch 11 on | Re-cut with `drm/ci/rfc-series.sh` on current drm-misc-next. Every patch builds and passes checkpatch `--strict`, kernel-doc, sparse, smatch and gcc/clang W=1, proven per patch in CI. `b4 prep`, then `b4 send --dry-run` only |
| K10 | The cover letter and `docs/rfc/README.md` (with K18) | The subject and patch list say 8 patches, but there are 10 patch texts and the series has 12. dEQP is 16,587 there and 16,638 today | Refresh in Phase B; K18 lists the rest. The AI-disclosure wording (G0) is Joseph's |
| K14 | Synchronous VM_BIND takes fences (research U11) | `drm-vm-bind-async.rst`: "A synchronous VM_BIND takes neither in-fences nor out-fences." Ours requires `FLAG_SYNC`, yet it still waits on `sync_ops` and signals them with an already-signaled fence | **Decided:** sync binds take no fences. `sync_ops` must be empty, and an async mode is a later flag, as Rogue did. With uapi-stream on the Mesa side. In the rev-4 bump |
| K15 | Ioctl numbers and version (U10) | The header carries downstream numbers with gaps (0x0b and 0x0d retired, SUBMIT_JOBS at 0x0e), DRM version 0.3, and a revision history in its DOC block | **Decided:** contiguous numbers from 0 and version 1.0, with no history in the header (history goes in UAPI.md). This is the rev-4 bump: our Mesa and kernel ship in step. It also states that BOs are always zeroed and that quirks are never removed for a GPU (U13, U15) |
| K16 | Fence-signaling work on a shared workqueue (S3) | `poll_work` and `reap_work` signal fences from `system_dfl_wq` (`pvrsgx_job.c` ~209, ~214, ~304) | A driver workqueue with `WQ_MEM_RECLAIM`, or the scheduler's submit workqueue |
| K17 | The linux-firmware entry (F3, F5-F7) | The WHENCE draft gives `Source:` as a URL, which `check_whence.py` refuses. There are two names (`sgx_ukernel.bin`, `sgx545_ukernel.bin`) and no version in the name. The MIT note is stale: `LICENSES/MIT` already exists. MIT has no patent grant (LICENSE-CRITERIA 3.1) | One versioned name, e.g. `powervr_sgx/sgx545_v1.bin`, that ships with K7 in v0.2.0. A new name means old kernels never see the new image, so the firmware package can carry both. WHENCE gets `Licence: MIT` and either in-tree source or a URL in free text. The patent question and the linux-firmware AI trailer are Joseph's |
| K18 | Cover letter and README facts (research section d) | The patch lists disagree. Transfer, runtime PM, devcoredump and tracepoints are called follow-ups but sit in v1 patches. Question 6 undercounts: it is three CPA passes, two with a per-page clflush, plus PAT memtype operations per page; the lines are :256/:281 in v7.3. Question 3 asks about "hardware this old". The Cc still has Binns as a reviewer; the drm/imagination maintainers are Belle and Santivetti. The README's AI-tag claim is wrong (Assisted-by was removed in v7.4-rc1, 1e035297ffda). The docs title says SGX545, the cover SGX5xx | Make the text and the series agree. Rewrite question 6 in about five lines with function names, 41d88484c71c and Hellström's and Hansen's direction. Drop question 3 (K14 answers it). Fix the Cc and the README. Add the lineage, implicit-sync and 32-bit-DMA lines. "No vendor binaries or vendor userspace", not "no vendor code" |
| K19 | Lockdep priming (S4) | Lockdep doesn't know that the scheduler and done locks are taken under reclaim | At init: `fs_reclaim_acquire()`/`might_lock()` on those locks, as Brost asked of ethos-u |
| K20 | A permanent vmap of every import (M3) | D6 keeps the exporter's `dma_buf_vmap()` for the import's whole life, to clflush the listed ranges | Justify it in the uAPI docs and the cover letter: a vmap can't be taken in the signaling path. Expect the question |

### Open: disclosed in the RFC, fixed before the merge

| # | item | state | plan |
|---|---|---|---|
| K11 | One parameter buffer for the device (review B1) | Every client's render context shares the device's PB, mapped read-write at the same 3D-params address in every VM. One client's GPU work can read or corrupt another's binned geometry and the DPM free list. The research ranks it the most likely design blocker (the rocket precedent) | **Moved to "blocks the RFC": option A (Joseph, 2026-11-12, "best upstream practice").** The microkernel saves the outgoing PB's DPM state and loads the incoming one's. open-ukernel's `wip-pb-switch` 4730d4fe is software-validated, with the golden held. Feature bit PB_SWITCH = BIT(3), so upstream gets features 0xe. Kernel side: a PB per VM on its first render context, on top of sgx530-kernel's golden. Tests: two clients alternating frames on a CE5300 board, hang blame, Minecraft at 16 and 4 MiB, and an isolation check. Cover question 4 goes |
| K12 | F1: transfer state across clients | After one client's transfers have run, a second client's first transfer faults until a microkernel restart. Mesa submits no transfers | **Decided:** the transfer job stays out of the RFC series, and its uAPI job type is reserved. That also answers U9 (no uAPI without a user) |
| K13 | System suspend | Drain, microkernel stop, then start on resume (255f24c). Proven with `pm_test` on a CE5300 board only | **Decided:** disclosed in the cover letter |

### Open: merge only

- [ ] Joseph signs and sends, with one-time permission each time. G0 (the
  disclosure wording) and G1 (ask dri-devel about new C drivers first) are his.
- [ ] Userspace reviewed in canonical Mesa, and a Mesa reviewer's Acked-by on
  the uAPI patch (G2, drm-uapi.rst). The Mesa MR goes out with the RFC.
- [ ] The microkernel in linux-firmware (`powervr_sgx/sgx545_ukernel.bin`, G3;
  the WHENCE draft is in the Firmware section). Its features word decides K7.
- [ ] The uAPI tests as an IGT `powervr_sgx` binary. `drm/tests/IGT-PLAN.md`
  maps every test onto igt-gpu-tools. The port is parked until the uAPI has
  had its review, so the tests aren't ported twice. The cover letter states
  the plan.
- [ ] The x86 WC/WB page-attribute cost per BO (cover letter question 6).
  Re-measure after mesa-gallium's BO cache lands. If it still shows,
  CPU-cached BOs with D6 flushes go to Joseph as a uAPI decision
  (`m2-wc-page-pool-proposal.md`).
- [ ] More cores: a DT platform driver for `img,powervr-sgx` and the SGX530
  (sgx530-kernel), on top of K1.

### Done since the 2026-11-08 status

| what | where |
|---|---|
| Validator: exact forms and every operand slot. Code base 14 aliases base 0. The threat model is in `Documentation/gpu/powervr_sgx` (B2) | 1bd1e3cb, 9a685789, 2d03eddf; E3 fuzz and the NVAL table in rev 3 step 1 |
| PRIME import: only its DMA addresses are checked, and the exporter's pages are never touched (B3) | 09e407c5; D6 |
| D6 cache ranges: per-job 2D ranges flushed through the exporter's vmap, before the job and after it for writes. Explicit sync only (M5) | rev 3 step 1 (92442046) |
| VM_BIND with drm_gpuvm `sm_map`/`sm_unmap`, freeing deferred to a BOOKKEEP fence callback (M6). A VM's last unmap frees at once when no walk can be in flight | fdb30269; 70d04a7f (v0.1.1) |
| Arm-then-push submit (M1). Monotonic CCB sequence numbers (M2). The GPU is halted before any fence signals on a hang (M3). Unbind with files open (M4) | 10695c13, b862f516, 3c79a631, 1e60417e |
| **Runtime PM per job (M7): the stopgap is gone.** Each job takes its reference at submit, and `run_job` never resumes the GPU | c45ca872, d889d5cc |
| Completion from the microkernel's DONE_SEQ (M8, kernel half) | a3b8c8dc; firmware in K7 |
| A BIF page fault ends the job at once with -EFAULT (M9). `drm_dev_set_dma_dev` behind gma500 (M10) | c8863684, 4dc69604 |
| The uAPI header matches the code (M11). Docs match the code, and comments stand on their own (M12; see K4 for the rev 3 regressions) | c4fba4e4, ab5cc656, 0e793fdb, 67483f85 |
| No `overlap` parameter and no `prepare_job` (M13). gma500 exports pin without a GTT slot (M14); gma500 hands the SGX over | d01ca426, 2a8020a1, 5bb5d765 |
| The graveyard: no parameter. Released objects go after the next completed render, everything at a restart, and the lost paths reap | 9798b17, 5ff9dd95 (v0.1.1) |
| `job_timeout_ms` is the only module parameter | |
| sysfb DIRTYFB: declined upstream (fix X). Only the CE5300 test kernels keep it | `drm/sysfb/README.md` |

### drm-uapi.rst

| requirement | state |
|---|---|
| Open-source userspace, the real driver rather than a toy | yes: the Gallium driver passes 16,638 of 17,165 dEQP-GLES2 tests with no vendor code |
| Userspace reviewed in the canonical upstream, with an Acked-by | merge only (G2) |
| The kernel merges before the userspace | at merge |
| Render node only, every ioctl `DRM_RENDER_ALLOW` | yes |
| Hot-unplug: fences `-ENODEV`, ioctls `-ENODEV`, mmaps keep working | yes (M4); the mmap read after unbind is in K8 |
| Device reset: fence errors, per-context guilty state, submits from an affected context refused | yes: `-ECANCELED`, `reset_count`, guilty/innocent |
| Reset causes: devcoredump | yes |
| Device wedging | no: K5 |
| Recommended return values (`-ENOENT` for lookups, `-EIO` for a dead GPU, `-ENODEV`) | yes (M11) |
| IGT | merge only, with the plan stated |
| uAPI header under MIT, with kernel-doc | yes: `(GPL-2.0-only WITH Linux-syscall-note) OR MIT`, `kernel-doc -Werror` in CI |

### Tools (CI)

All green on m2-drm 3f36c8a4:
- checkpatch `--strict` on every driver file, the tests and the uAPI header;
- `kernel-doc -Wall -Werror`;
- sparse (`C=2`), smatch and coccinelle;
- gcc W=1 in tree and out of tree, and clang W=1 (`LLVM=1`);
- the i386/x86_64 uAPI layout check and its negative case;
- KUnit under QEMU (23/23 on 92442046).

The `debug` CI input builds the RAM-boot kernel with lockdep, KASAN, UBSAN,
DMA-API debug, kmemleak and debug objects (K8). A per-patch build of the
series is K9.

The plan of 2026-11-08 (DDK compat out of tree, the follow-up layering, a
generated register header, one `pvrsgx_` prefix, the ISA generation in the
core table) is done. Its record is in git history before this revision.
