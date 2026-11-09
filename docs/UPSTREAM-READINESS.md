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
| M4 | clang-format | not started | mesa-gallium (driver), uapi-stream (winsys) |
| M5 | comments and naming | driver done (mg-perf b42aa0d) | mesa-gallium |
| M6 | debug options and experiment code | partly | mesa-gallium, usse-compiler |
| M7 | tests in Mesa's format (`ci/`) | partly (ddk files moved, mg-perf 1b86487) | mesa-gallium |
| M8 | `docs/drivers/sgx.rst` | written: mesa/docs/drivers/sgx.rst | mesa-gallium |
| M9 | build inputs from `tools/` | open | uapi-stream, usse-compiler |

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

- **Already right.** The driver's knobs are `debug_get_option` tables
  (`SGX_DEBUG`, `SGX_DUMP_CODE`, `SGX_SLOTS`, …), with no raw
  `getenv` and no paths.
- **Keep, documented:** `nir`, `draw`, `sync`, `stats`, `serial`,
  `hangabort`, `flushall`, `novcache`, `nof16`, `noffmediump`.
- **Experiments to drop or fold in before upstream:**
  - `fsconst` (a measuring aid);
  - `attrs` and `blits` (one-off diagnostics, fold into `draw`);
  - `novs`/`nofs` (fixed programs instead of the compiler; drop once the
    compiler covers every program);
  - `zerocopy`: make it the default or drop it, once its numbers are in
    (docs/PERF.md);
  - `nodrvconsts`;
  - `SGX_FRAME_GAP_US`;
  - `SGX_SLOTS`: pick the default and drop the knob.
- **`setenv` in the driver.** `sgx_screen_create` sets
  `MESA_GLSL_DISABLE_IO_OPT=1` so that GLSL linking keeps varyings unpacked,
  letting the PDS do whole-vec4 texture reads. A driver must not set the
  process environment. It needs a compiler option instead: a
  `nir_shader_compiler_options` / io-options flag that keeps
  `nir_opt_varyings` from packing across slots, or a Mesa patch adding one.
- **The compiler: done (1ba7774).** `SGX_DISABLE`, `SGX_F16_CONST` and
  `SGX_F16_ITER` are one `SGX_COMPILER_DEBUG` flag table (`nopa`, `noout`,
  ..., `nocoissue`, `nof16const`, `nof16iter`), read once per compile.
  `SGX_PRINT_PRE_I2F` is dropped. See C1.

### M7. Tests in Mesa's format

- **There.** `drivers/sgx/ci/` holds `sgx-{fails,flakes,skips}.txt`
  (dEQP-GLES2) and `sgx-piglit-{fails,flakes,skips}.txt`, in deqp-runner's
  format.
- **To do.**
  - Move `ddk-*.txt` (the DDK comparison) out of the driver's `ci/` to
    `mesa/tests/deqp/ddk/`, where a copy already is.
  - Add a deqp-runner suite file (`deqp-sgx.toml`) and the piglit profile
    the way `ci/` dirs of other gallium drivers have them. Mesa CI runs them
    only once a farm has the hardware, but the files go in with the driver.
  - Refresh the expectations from the full runs on mg-perf (2026-11-08,
    EA3): dEQP 17,165 tests, 0 wedges; piglit in progress.

### M8. `docs/drivers/sgx.rst`

Write it for Mesa's docs tree. It covers:

- supported hardware (SGX545 on the CE5300; the family plan);
- the kernel driver it needs;
- GL/GLES versions and extensions;
- the debug options table;
- known limitations: no 3D textures, NPOT cube restrictions, one render
  target, no MSAA;
- how to run the tests.

### M9. Build inputs from `tools/`

`mesa/build.sh` generates `sgx_usse_progs.h` with `tools/gen/mkusse.py`.
Upstream, the fixed programs are assembled by the compiler's encoder at build
time (DRIVER-UPSTREAM.md §5.2, COMPILER-UPSTREAM.md).

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
| C3 | NIR passes in Mesa's shape | done (69923e0); one item left |
| C4 | unit tests under `meson test` | done (7e0421a); coverage to grow |
| C5 | structure doc | done (`compiler/README.md`, 7e0421a) |
| C6 | no global mutable state | done (`hw_const`) |
| C7 | comments cite `docs/`, not libusc or the DDK | not started |
| C8 | DDK-compatibility code out of the compiler | partly |
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
  - Still to do: `sgx_lower()` sets `s->options` to its own options. The
    driver and sgxc should create the shader with `sgx_nir_options()`
    instead, and the compiler should assert that they did.
- **C4. Tests.**
  - `meson test --suite sgx` runs `sgx_compiler` (gtest) beside
    cleanroom-isa's `sgx_isa`.
  - Shaders are built with `nir_builder` and compiled; every word must
    decode through the isaspec disassembler.
  - Cases: a fragment varying to color, a vertex position, efo co-issue on
    mat4 * v, and the int-select loop bound (d3adad7).
  - Each case was checked to fail on the bug it guards.
  - To grow: F16 and C10 paths, textures, discard, constants in memory.
  - The DDK corpus harness (`tools/usc`) stays in this repository.
- **C5. Structure doc.** `compiler/README.md` covers the files, the pipeline
  (lowering, analysis, selection, backend passes, allocation, encoding), the
  debug flags and the tests. Mesa's `docs/drivers/sgx.rst` (M8) can link
  it or take a section from it.
- **C6. Global state.** `hw_const()` built a static table on first use (a
  race between two contexts compiling at once). It is now a const table.
  This also fixes a latent bug: a literal equal to the old filler value
  0xdeadbeef no longer matches hardware constant 0.
- **C7. Comments.**
  - About 58 comments say "as libusc does" or "as the DDK does" (27 in
    `sgx_compile.c`, 16 in `sgx_nir.c`, 12 in `sgx_compiler.h`).
  - Rewrite each to cite the `docs/` section and the test that fixed the
    behavior (COMPILER-UPSTREAM.md §2.2).
  - Box and application names: none left in the compiler.
- **C8. DDK-compatibility code** (COMPILER-UPSTREAM.md §3).
  - Still in the compiler: the varying-to-set allocation, first-use sampler
    numbering, the USP label numbers 3/4/5, and the placeholder color
    iteration's PA count.
  - Upstream, the driver owns layouts. Move these behind
    `struct sgx_layout`, or into sgxc.
- **C9. File size.** `sgx_compile.c` is 5.8k lines, the size of
  `ir3_compiler_nir.c`.
  - Splitting it into selection, register allocation and encoding files means
    exporting about 64 of its 128 functions (prefixed, with prototypes in
    `sgx_priv.h`).
  - Worth it only if review asks.

## Firmware (the open SGX microkernel, `ukernel/`)

The microkernel is MIT-licensed source (`ukernel/ukernel.py`) assembled to a
small binary a loader runs on the SGX core. Upstreaming has two destinations:
the **source** (its own repository) and the **built binary** (linux-firmware,
loaded by the DRM driver).

| item | state |
|---|---|
| Reproducible build | **Done.** Pure-CPython assembler, no timestamps/paths/randomness; byte-identical `.bin`+`.json` on any machine, any CPython ≥ 3.7. One command: `ukernel/build.sh`. |
| Pinned toolchain | **Done (documented).** Only CPython ≥ 3.7; no external packages. Recorded in `ukernel/README.md` → Building. |
| MIT license | **Done.** `ukernel/LICENSE` + an `SPDX-License-Identifier: MIT` header on every source file. |
| Release manifest | **Done.** `core`, `version` (major.minor), `features`, `build_id`, sizes, entry offsets, patch sites. |
| Per-core identity | **Done.** `ukernel.py:CORE_NAME` names the target SGX core; build.py derives the binary name and manifest `core` from it, so SGX540/SGX531/… images never collide. |
| Feature-bit documentation | **Done.** `ukernel/README.md` → Feature bits (bit 0 CLEANUP_V2, bit 1 SECURE). |
| Build/feature README | **Done.** `ukernel/README.md`. |
| linux-firmware WHENCE entry | **Draft below** (not submitted). |
| Neutral docs pass | **Pending ta3d-state** — `docs/` and any Documentation text use CE5300/SGX5xx neutrally; dev-box history → `docs/log/`. |
| DPM out-of-memory handler | **Pending** — the one open functional gap before the firmware is feature-complete for general workloads (`side-session-notes/.../dpm-oom-handler-design.md`). Not a packaging item, but gates "ready". |

### Draft linux-firmware WHENCE entry (not submitted)

The binary linux-firmware would carry is the **packaged** image the DRM driver
requests (header + feature bits applied by the driver's packaging tool over the
`ukernel/build.py` output), named as the driver requests it:

```
Driver: powervr_sgx — PowerVR SGX5xx (e.g. SGX545 on the Intel Atom CE5300)

File: powervr_sgx/sgx_ukernel.bin
Version: 1.0
Source: <source-repository-url>/ukernel   (ukernel.py + build.py; the DRM
        driver's packaging tool applies the header/feature bits)
License: MIT.  See LICENSE.<id> for details.
```

Naming (no project name upstream; per core; the driver's own subdirectory):
`powervr_sgx/<core>_ukernel.bin`, so `powervr_sgx/sgx_ukernel.bin` here and
`powervr_sgx/sgx540_ukernel.bin` for another core. The directory matches the DRM
driver's module/request path (`powervr_sgx/`, not `powervr/`). build.py emits
`<core>_ukernel.bin`; the driver's `firmware=` request and its packaging tool
must use the same name — kept in sync across build.py, the driver, the docs and
the test scripts. Open before this is real: the MIT text must be added to
linux-firmware's license set, and `Source` needs the public repository URL. The
source and the built binary are versioned together by `version` + `build_id`.

### Not a blocker, noted
- The microkernel source carries inline hardware-provenance comments (which
  behaviors were confirmed on which bring-up box). These are useful engineering
  history; a final upstream-source pass can thin them or move the box-specific
  ones to `docs/log/`, but they do not affect the binary and are low priority.

## Mesa winsys and platform

Scope: `mesa/src/gallium/winsys/sgx/drm`, the loader/kmsro/dril and EGL
patches, `mesa/build.sh`, and the test rigs under `mesa/tests`. Owner
uapi-stream. Status as of 2026-11-08.

| # | item | status |
|---|---|---|
| W1 | powervr_sgx is the upstream backend; pvrsrvkm downstream only | done |
| W2 | the Mesa patches as standalone commits | done |
| W3 | device info from DEV_QUERY (M2's queries) | done |
| W4 | generated headers and the uAPI header | open |
| W5 | comments: no boards, apps or dates; DDK references | half done |
| W6 | `sgx_frame_test` | done |
| W7 | Mesa logging and helpers | open |
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
  2. The shared code in `src/sgx`.
  3. The isaspec changes.
  4. `kmsro, dril: sgx as a render-only GPU`.
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
- **The command-stream packers.** `powervr_sgx_kmd_stream.h` is generated by
  `docs/uapi/gen_stream.py` from the stream XML, outside Mesa.
  - Upstream: the XML moves into `src/sgx`, and meson generates the header,
    as csbgen does for the state words.
  - m2 is renaming the shared stream files to `pvrsgx_*`. The Mesa side
    follows that rename.
- **`sgx_usse_progs.h`** comes from `tools/gen/mkusse.py`. That is M9.

### W5. Comments

- **Done.** No boards, applications, dates or project names in the winsys.
- **Open.** About 88 comment lines cite the DDK's behavior or captures as
  the reason for a value.
  - Upstream, a value's reason is the hardware fact, with a pointer to the
    hardware documentation (STATE.md, PDS.md, ISA.md), not another driver's
    output.
  - These need rewording one by one. No code changes.

### W6. The winsys test

- **Done.** `sgx_frame_test` (the winsys without the gallium driver, a
  hardware check of the kernel interface) is no longer built by default.
  `mesa/build.sh` asks ninja for it.
- **Upstream.** Leave it out, or keep it as a non-default target if
  reviewers want a smoke test.

### W7. Logging and helpers

- **Uses Mesa's helpers already.** `util_vma_heap` for device addresses,
  `simple_mtx`, `util/macros.h`, `drmSyncobj*`.
- **Open.**
  - The winsys logs through its own `sgxws_err` and `fprintf`. Move them to
    `mesa_loge`/`mesa_logw`.
  - Read the debug environment (`SGX_DEBUG` and friends) through the
    driver's `debug_get_option` table, not the winsys's own parser.

### W8. kmsro's render-node pick

kmsro takes a PCI render node when its kernel driver is powervr_sgx, because
on the CE5300 the display device (simpledrm) is a platform device and the GPU
a PCI function.
- **Cedarview.** The display (gma500) and the GPU sit on the same PCI
  function, and powervr_sgx binds to gma500's auxiliary device.
- **Open.** Recheck the pick on Cedarview hardware (kit v2) before the MR.
  The heuristic may become "the render node of the auxiliary device's parent".

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
- "the open microkernel" becomes "the open microkernel";
- IP addresses, box workspaces and product names go.

Long bring-up narratives move to a dated `docs/log/entries/` file, with a
pointer left in their place.

Remaining box-specific references, counted on 2026-11-08 by
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
| ISA.md | 12 | 0 | not started | usse-compiler / cleanroom-isa |
| DRM-DRIVER.md | 9 | 16 | not started | m2-kernel-driver |
| UAPI.md | 9 | 1 | not started | uapi-stream |
| GEN.md, WINSYS.md | 5, 4 | 0 | not started | mesa-gallium |
| COMPILER-UPSTREAM.md, DRIVER-UPSTREAM.md | 2, 2 | 1, 1 | not started | usse-compiler, mesa-gallium |
| NOTES.md, PERF.md, PIGLIT.md, CLEANROOM-COVERAGE.md | 11, 2, 7, 17 | — | internal working notes, stay as they are (decided) | main |
| README.md | 1 | — | not started | main |

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
2026-11-08, branch upstream-ready on um-re.

### Done

| item | state |
|---|---|
| Layout like Mesa's other isaspec users | `isa/` and `pds/` follow `src/etnaviv/isa`: the XML, the `isa.h` the generated decoder includes, custom targets for the decoder (`<name>-isa.c/.h`), the encoder (`-encode.h`) and the enums, a static library and an `idep_`, and `tests/meson.build` |
| Layout like Mesa's csbgen users | `csbgen/` follows `src/imagination/csbgen/rogue`: `<name>.xml` -> `csbgen/<name>.h` through the shared `gen_pack_header.py`, and the per-file loop is the same. `sgx_csb.h` plays the part of the Vulkan driver's `pvr_csb.h` (address type, includes, the pack-in-place macro). The XML files are renamed `vdm.xml`, `ta.xml`, `pbe.xml`, `tex.xml` |
| Tests under `meson test` | `meson test --suite sgx` runs `sgx_isa` (assemble -> encode -> disassemble over three lists; a line's given words must match), `sgx_pds` (the same for PDS, plus the DOUT data words), and the new `sgx_csb` (66 state words the documentation quotes, packed and unpacked). It passes 3/3 on a native host build with `-Dbuild-tests=true`. The csb test found an XML default that was wrong for one T3 form (tex2 clears b23); the test case now sets it. |
| Nothing outside `meson test` in the Mesa tree | `isa/tests/standalone.sh` (a library build without Mesa, which `tools/usc/check_enc.py` and `drm/tools/gen_usse_corpus.py` use) moved to `mesa/tests/isa-standalone/`, which isn't copied into Mesa. Its `.gitignore` went with it |
| Mesa style | one `mesa/src/sgx/.clang-format` for all of `src/sgx` (shared with the compiler): `InheritParentConfig`, `DisableFormat: false`, column limit 80, as `src/asahi` and `src/freedreno`. Every C file here is formatted with it. One macro list, the assembler's flag table, is hand-wrapped between `clang-format off/on`. Comments are in American English (`colour` -> `color`). A `src/sgx/**/*` line in Mesa's `.clang-format-include` belongs to the MR |
| License headers | every source file, XML, meson file, script and test list has `Copyright © 2026 Joseph Telaak` and `SPDX-License-Identifier: MIT` (the test lists as `#` comments). The READMEs carry none, as Mesa's don't |
| A README per tree | `isa/README.md` and `pds/README.md` now describe the tree rather than its history: what it is, the files, the generated files, the tests, the isaspec patch, what isaspec can't express, and who uses it. The answered-gap lists and the old encoder's story stay in `docs/log`. `csbgen/README.md` is new: files, use, conventions, tests |
| No DDK inputs | the build and the tests read only the XML, the test lists and Mesa. The test lists' words are the ones the ISA/PDS/state documentation quotes. Comments no longer name the DDK, a capture rig or USEASM, and the `#i16-fmt` enum nothing used (an unused-variable warning in the generated decoder) is gone |
| Unchanged output | after the restructure, the offline winsys harness (clears, draws, scissors, end-of-tile, over 60 frames) is still byte-identical to um-re 9145c84 (66.9 MB of rings). `mesa/tests/isa-standalone` runs the ISA lists: 317 instructions, 0 failures |
| Warnings | no warnings from the three trees on the host build (clang) or the zig x86_64 build. The PDS library keeps `-Wno-unused-variable` for isaspec's tables of encode-only roots (below) |

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
2. **The isaspec change** (`mesa/patches/0004`) goes upstream first, as two
   commits:
   - `<param>`s on a derived field of bitset type;
   - the encoder's handling of several `<override>`s defining one field.
   - isaspec has no test suite. The SGX545 ISA test is the first user of
     both, so the commits should point at it, or bring a minimal XML case.
3. **The PDS decoder's unused tables.** The DOUT data words are encode-only
   roots, and isaspec emits decode tables nothing uses for them, so
   `-Wno-unused-variable` is needed. Options:
   - isaspec skips decode tables for roots no `#instruction` reaches;
   - the data words move to a second XML that only the encoder reads.
4. **The documentation the comments cite.** `docs/ISA.md`, `docs/PDS.md`
   and `docs/STATE.md` live in sgx545-ce. Upstream needs them either as
   `docs/drivers/sgx/*.rst` in Mesa or behind a stable link from each
   README. The comments cite them by section name.
5. **A disassembler tool.** `etnaviv-disasm` is built with
   `-Dtools=etnaviv`. Ours is `sgx_pds_test -d` and the ISA test's text
   output. A `sgx-disasm` (USSE and PDS) behind `-Dtools=sgx` needs a
   `meson.options` entry.
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
9. **The gallium/meson hooks** (`mesa/patches/0001`, `0002`) become part of
   the driver's MR series. Nothing in these three trees depends on them
   beyond `with_gallium_sgx` entering `src/sgx`.

## Kernel (powervr_sgx)

Owner: powervr_sgx. `drm/powervr_sgx`, the gma500 patch in `drm/gma500`, the
uAPI header `docs/uapi/powervr_sgx_drm.h`. Status as of m2-drm 2026-11-08.

### Design

- [x] **One driver for the SGX5xx family.** `pvrsgx_core.c` has one entry per
  supported core: ID and revision, the features the driver relies on (as
  `SGX_FEATURE_*` in the GPL DDK's `sgxfeaturedefs.h`), the number of BIF
  directory lists, the core clock and the firmware file. A core that isn't
  listed, or lacks a required feature, is refused before any register write.
  The SGX535 (also 16 directory lists, multiple memory contexts, plus a 2D core)
  would be a new entry and a check of the features it lacks.
- [x] **No vendor microkernel.** The driver knows only the open microkernel,
  and loads it only if it declares secure mode (no client directory maps the
  firmware's heaps). The vendor image, its entry programs and patch sites, and
  the shared-kernel-heap mode it needs are an out-of-tree patch
  (`drm/ci/ddk-compat.patch`, about 400 lines) that only the CE5300 test
  kernels apply.
- [x] **Fail closed on unknown hardware.** Core table above; the gma500
  auxiliary-device probe checks the BAR size and parent.
- [ ] **Cedarview entry.** Needs the core revision from the tester's kit v1
  report (`probe_only=1`).
- [x] **Firmware name.** The CE5300 entry names
  `powervr_sgx/sgx545_ukernel.bin`, the name agreed with the firmware section
  (one file per core under the module's directory).
- [ ] **Upstream tree layout.** The uAPI header moves to
  `include/uapi/drm/powervr_sgx_drm.h` (now a symlink under the driver);
  `drm/MAINTAINERS.fragment` and `drm/Documentation/gpu/powervr_sgx/` are
  ready to move.
- [ ] **Debug module parameters.** Reviewers will ask which parameters a user
  needs. Keep: `runtime_pm`, `job_timeout_ms`, `firmware`. Developer-only,
  to move behind debugfs or drop: `idle_kernel_pd`, `probe_only`, `core_clock`,
  `cleanup`, `hwr_check`, `restart_quiesce`, `single_entry`, `snapshot`,
  `snap_gap_ms`, `reap_after_render`. `share_kernel_heaps` goes with DDK
  compat.

### Tools (CI)

`.github/workflows/powervr-sgx.yml` (the EA kernel) and
`.github/workflows/powervr-sgx-upstream.yml` (mainline with the gma500 patch,
both configurations).

- [x] `checkpatch.pl --strict` on every driver file, the tests and the uAPI
  header: clean.
- [x] `kernel-doc -Wall -Werror` on the uAPI header and every driver file.
- [x] sparse (`C=2`), driver and gma500.
- [x] gcc `W=1`, in tree and out of tree; the CE5300 test kernels also build the DDK-compat patch.
- [x] clang `W=1` (`LLVM=1`), upstream configuration.
- [x] smatch (`-p=kernel`).
- [x] coccinelle (`make coccicheck MODE=report`).
- [x] uAPI layout: `docs/uapi/check_layout.c` built `-m32` and `-m64` with
  `-Wpadded -Werror` (one layout, no implicit padding), and its negative case
  must fail.
- [x] KUnit: the firmware file checks, under QEMU.
- [ ] **Runtime checkers on the EA.** The EA workflow's `debug` input builds the
  RAM-boot kernel with lockdep (`PROVE_LOCKING`), KASAN, UBSAN, DMA-API debug,
  kmemleak and debug objects (`drm/ci/debug.fragment`). To run: the uAPI
  suite, the frame, blit, hang and PRIME tests on both microkernels, then
  `dmesg` and `/sys/kernel/debug/kmemleak`. Findings get fixed and listed here.
- [ ] **IGT.** The uAPI tests are IGT-shaped C (`drm/tests`); upstream will
  want them as an IGT `powervr_sgx` test binary.

### Idioms

- [x] `devm_drm_dev_alloc`, `pcim_enable_device`, `pcim_iomap_region`.
- [x] Runtime PM is set up before `drm_dev_register`, and torn down if it fails.
- [x] `drm_sched` (one entity per context, credit 1), `drm_exec` for the
  submit's reservations, `drm_gpuvm` for VMs and their shared reservation.
- [x] **dma-fence signaling rules.** The hardware fence is allocated at
  submit and only initialized in `run_job`; the timeout's devcoredump
  allocates with `GFP_NOWAIT`; completions run inside
  `dma_fence_begin_signalling()`. To confirm under lockdep (above).
- [ ] **`run_job` resumes the GPU.** `pvrsgx_power_get()` can runtime-resume
  from `run_job`, as panthor does. The resume path allocates nothing, but
  the debug run must show lockdep accepts it.
- [x] **Named registers.** Names from the GPL `sgx545defs.h` (soft-reset bits,
  clock-gating fields, BIF, event kick); our names for the event-program
  registers the header leaves out; the vendor init script's unnamed
  registers as one table pointing at `docs/SRVINIT.md`.
- [ ] **Firmware structure layouts.** `pvrsgx_render.c` still writes the
  microkernel's command and render structures by word offset (`cmd[0x1c / 4]`
  and the like, 78 places). They should become C structures with named
  fields, from `docs/UKERNEL.md` and `docs/STATE.md`. This is the largest
  remaining item.
- [ ] **PDS encodings.** `pvrsgx_pds.c` builds instruction words from shifts
  that `docs/PDS.md` explains; named fields would make review easier.
- [x] Fixed in this pass: cleanup took a runtime-PM reference through
  `sdev->pdev`, which is NULL behind gma500.

### Minimal first series (plan, 2026-11-08)

Today the driver is 7,639 lines (`drm/powervr_sgx`, including 640 lines of
stream decoder shared with Mesa and the 168-line KUnit test). The tree keeps
everything, but it is layered so that v1 builds and runs on its own, and each
follow-up is a self-contained addition: its own file(s) plus a few clearly
marked call sites. CI builds v1 alone (the follow-up files removed) as well
as the whole driver.

**v1, about 5,900 lines:** probe and the core table, firmware boot (open
microkernel only, secure mode), MMU and VMs (`drm_gpuvm`, `VM_MAP`/`VM_UNMAP`),
GEM with dma-buf import from other devices (X on simpledrm needs it; mesa-gallium,
2026-11-08), `DEV_QUERY`, contexts, render job submit with syncobjs, the command-stream
and shader validator (the security model), timeout reset and recovery with
guilty/innocent reporting, and the minimum uAPI.

| follow-up | what | lines it takes out of v1 |
|---|---|---|
| F1 transfer | transfer contexts and jobs, their uAPI job type | ~150 |
| F3 runtime PM | autosuspend; v1 keeps the GPU on while the device is open | ~90 |
| F4 devcoredump | the hang dump | ~125 |
| F5 debugfs | `status`, `render`, `render_before` and the snapshot parameters | ~180 |
| F6 tracepoints | `pvrsgx_kick/irq/job_done/flush_imports` | ~100 |
| F7 gma500 host | the auxiliary-device binding (with the gma500 patch) | ~150 |

**Out of the upstream driver entirely:**
- DDK compat (vendor firmware, entry programs, shared kernel heaps): done,
  the tree lost 384 lines to `drm/ci/ddk-compat.patch`, which only the CE5300
  test kernels apply.
- Microkernel cleanup commands (v1 and v2): ~70 lines. The open microkernel
  doesn't use them; freed objects wait in the graveyard until the next
  render.
- Developer parameters `single_entry`, `idle_kernel_pd`, `restart_quiesce`,
  `hwr_check`, `probe_only`, `reap_after_render`, `core_clock`, `cleanup`
  and their code paths: ~60 lines.

**While cutting, in this order:**
1. Delete: DDK compat out to the patch, cleanup commands, developer
   parameters, anything the open microkernel no longer reads (to confirm with
   it: the TA/3D signature lists, HWPerf buffer, `GETMISCINFO` struct-size
   check).
2. Layer: move F1-F7 into their own files behind stubs in their headers.
3. Kernel style:
   - Firmware structures as C structs with kernel-doc and `static_assert`
     offsets, in place of word offsets (`cmd[0x1c / 4]`, 78 sites in
     `pvrsgx_render.c`).
   - Registers generated from the GPL `sgx545defs.h` into `pvrsgx_regs.h`, with
     `BIT()`/`GENMASK()`, only the registers the driver uses.
   - No DDK names (`SGXMKIF_*`, `ui32*`: 17 places).
   - One prefix, `pvrsgx_`: the five `powervr_sgx_stream*` files become
     `pvrsgx_stream*` (shared with Mesa, so agreed with uapi-stream first).
4. Generic: an ISA generation in the core table for the validator's tables;
   no SGX545 or CE5300 constants outside the table and the platform glue.
5. Prove it: the checker CI on v1 and the whole; the lockdep/KASAN/UBSAN run
   on the CE5300 with the uAPI suite and frame tests; then full dEQP through
   Mesa.

**Status (m2-drm 7eaa5f5, 2026-11-08).** The driver is 7,482 lines; the
follow-up files hold 690 of them, leaving 6,792 for v1. That includes the
690-line stream decoder shared with Mesa and 533 lines of firmware-structure
definitions with kernel-doc, which replaced word offsets.

| step | state |
|---|---|
| DDK compat out | done: `drm/ci/ddk-compat.patch`, applied only by the CE5300 test kernels |
| cleanup commands, 7 developer parameters, the render snapshot and last-render debugfs | deleted |
| F1 transfer | own file (`pvrsgx_transfer.c`, 209 lines) |
| F3 runtime PM | own file (`pvrsgx_pm.c`, 126 lines), built with `CONFIG_PM`, stubs otherwise; no `runtime_pm` parameter |
| F4 coredump, F5 debugfs, F6 tracepoints, F7 gma500 | already separate files; their call sites are still in v1 code |
| PRIME import | own file (`pvrsgx_prime.c`), but in v1: X through kmsro on simpledrm imports its scanout buffers |
| firmware structures | done: TA command, dependency block, render details, RT data, PB descriptor, HW render and transfer contexts, transfer command, control block (`pvrsgx_fwif.h`, offsets checked by `static_assert`); no firmware word is written by offset any more |
| DDK names in comments | gone (the uAPI's `DRM_POWERVR_SGX_FW_IF_SGXMKIF_1_7` is left for uapi-stream) |
| generated register header | done: `pvrsgx_regs.h` comes from the GPL `sgx545defs.h` through `drm/tools/gen_regs.py` (CI diffs it); our names for the registers that header leaves out, and the MMU constants, are in `pvrsgx_hw.h` |
| one `pvrsgx_` prefix | waiting on uapi-stream (the shared stream files) |
| ISA generation in the core table | done: the validator picks its USSE form table by the core's ISA |
| runtime checkers on the CE5300 | debug kernel to rebuild on the trimmed driver; waiting for a free box |

**uAPI effect.** v1's header leaves out the transfer context and job types
and their structures (F1 appends them; the enums are append-only). Mesa
creates only render contexts and submits only render jobs, so it needs no
change (mesa-gallium, 2026-11-08). Nothing else in the uAPI changes.

### The series

The m2-drm history is a development log with fixes on fixes; nothing upstream
will see it. The series gets cut from the final tree by area, each patch
building on its own:

1. uAPI header and documentation.
2. Core table, registers, MMU, kernel memory, device bring-up (no firmware).
3. Open-microkernel loader and start, PDS programs.
4. GEM, VMs (`drm_gpuvm`), `VM_MAP`/`VM_UNMAP`.
5. Contexts, scheduler, fences, NULL jobs, `SUBMIT_JOBS`.
6. Render: PB, RT datasets, the TA command; stream validation (D9).
7. Transfer jobs.
8. Hang recovery, devcoredump, runtime PM.
9. debugfs, tracepoints.
10. gma500: the SGX as an auxiliary device; powervr_sgx binds to it.
11. (Not for upstream) DDK compat.

The files already split along these lines; cutting the series will show where
one patch reaches into a later one.
