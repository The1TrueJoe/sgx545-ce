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

## Mesa pipe driver and compiler


Scope: `mesa/src/gallium/drivers/sgx545` (the pipe driver), the gallium half
of patch 0001, 0002 and 0004, by mesa-gallium. The winsys, the EGL platform
and the loader/kmsro patches are uapi-stream's section below; `sgxws.h` is the
seam, kept stable during the pass. Items M1, M4 (winsys files) and M9 sit at
the seam and are listed here for completeness. The compiler
(`mesa/src/sgx545/compiler`) is covered by `docs/COMPILER-UPSTREAM.md`.
`docs/DRIVER-UPSTREAM.md` (2026-11-01) is the earlier hardware-fact audit; its
doc gaps still stand. Status as of 2026-11-08, um-re 0507229 plus mg-perf.

| # | item | status | owner |
|---|---|---|---|
| M1 | upstream build is powervr_sgx only | mostly there | uapi-stream |
| M2 | features from DEV_QUERY and a core table | not started | mesa-gallium (table), uapi-stream (queries) |
| M3 | driver name for the family | decision needed | Joseph |
| M4 | clang-format | not started | mesa-gallium (driver), uapi-stream (winsys) |
| M5 | comments and naming | driver done (mg-perf b42aa0d) | mesa-gallium |
| M6 | debug options and experiment code | partly | mesa-gallium, usse-compiler |
| M7 | tests in Mesa's format (`ci/`) | partly (ddk files moved, mg-perf 1b86487) | mesa-gallium |
| M8 | `docs/drivers/sgx545.rst` | not started | mesa-gallium |
| M9 | build inputs from `tools/` | open | uapi-stream, usse-compiler |

### M1. The upstream build is powervr_sgx only

- **Done.**
  - The winsys reaches the kernel through a backend ops table (`struct
    sgxws_kif`): `sgx545_powervr_sgx.c` for powervr_sgx, `sgx545_pvr.c` for
    pvrsrvkm.
  - The driver includes only `sgxws.h`. Its meson dependency has no kernel
    include path.
  - `sgx545_frame.c` has no services structure layouts left.
- **To do.**
  - Make the pvrsrvkm backend an explicit meson option,
    `-Dgallium-sgx545-pvrsrvkm=disabled` by default and downstream-only. Today
    the build switches on whether `libpvr/pvr.c` exists (`fs.exists`), while
    `sgx545_frame.h` already talks of a meson option.
  - `sgx545_pvr.c`, `libpvr/` and the `include/sgx545-kernel` link are built
    from the GPL kernel's headers. They stay out of the upstream tree: an
    downstream patch on top, deleted by removing two files and one meson block.
  - Backend-specific debug knobs move into that backend: `flushrt`, which
    sends `SGX_FLUSH_HW_RENDER_TARGET`.
  - `SGX545_TRACE`/`SGX545_DRY_RUN` write a pvrtrace log for
    `tools/decode`. It is a development aid either way: decide whether it
    stays as a documented debug option or becomes downstream-only.
  - `resource_from_user_memory` (the fbdev framebuffer, services only) and
    the EGL fbdev platform stay downstream.

### M2. Features from DEV_QUERY and a core table

The driver hard-codes SGX545 values. `sgx545_screen.c` has:

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

`sgx545` names one core. A family driver would be `sgx`: `drivers/sgx`,
`winsys/sgx`, `SGX_DEBUG`, `sgx_dri.so`. Renaming touches every file, the
loader patch (0005), kmsro (0006), the downstream packaging and every session's
scripts. It is cheap once, now, and expensive after upstream review starts.

### M4. clang-format

Mesa's base style is `src/.clang-format`, which has `DisableFormat: true`.
Each driver opts in with its own `.clang-format`. The PowerVR code
(`src/imagination`) and most gallium drivers use 80 columns; d3d12 uses 120.

- Ours: `BasedOnStyle: InheritParentConfig`, `DisableFormat: false`,
  `ColumnLimit: 80`, in `drivers/sgx545` and `winsys/sgx545/drm`.
- Measured on mg-perf: 2,195 of 9,811 lines change at 80 columns, 974 at
  120.
- It is one mechanical commit with nothing else in it, done when no other
  session has winsys edits in flight (uapi-stream) to avoid conflicts.
- `ForEachMacros` comes from the parent (`u_foreach_bit` etc.).

### M5. Comments and naming

- About 50 comments in the driver, winsys and compiler name the boot
  image's application, the bench box or game titles (the Minecraft overlay,
  "EA1"). Rewrite each to say what GL usage triggers the behaviour and which
  test covers it (dEQP/piglit name), and move the history to `docs/log`.
- "as the DDK does" comments: cite the `docs/` section instead
  (DRIVER-UPSTREAM.md §2).
- The platform is "CE5300" everywhere.

### M6. Debug options and experiment code

- **Already right.** The driver's knobs are `debug_get_option` tables
  (`SGX545_DEBUG`, `SGX545_DUMP_CODE`, `SGX545_SLOTS`, …), with no raw
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
  - `SGX545_FRAME_GAP_US`;
  - `SGX545_SLOTS`: pick the default and drop the knob.
- **The compiler** has raw `getenv`: `SGX_DISABLE`, `SGX_F16_CONST`,
  `SGX_F16_ITER`, `SGX_PRINT_PRE_I2F`. Route them through `debug_get_option`
  in a named table (usse-compiler).

### M7. Tests in Mesa's format

- **There.** `drivers/sgx545/ci/` holds `sgx545-{fails,flakes,skips}.txt`
  (dEQP-GLES2) and `sgx545-piglit-{fails,flakes,skips}.txt`, in deqp-runner's
  format.
- **To do.**
  - Move `ddk-*.txt` (the DDK comparison) out of the driver's `ci/` to
    `mesa/tests/deqp/ddk/`, where a copy already is.
  - Add a deqp-runner suite file (`deqp-sgx545.toml`) and the piglit profile
    the way `ci/` dirs of other gallium drivers have them. Mesa CI runs them
    only once a farm has the hardware, but the files go in with the driver.
  - Refresh the expectations from the full runs on mg-perf (2026-11-08,
    EA3): dEQP 17,165 tests, 0 wedges; piglit in progress.

### M8. `docs/drivers/sgx545.rst`

Write it for Mesa's docs tree. It covers:

- supported hardware (SGX545 on the CE5300; the family plan);
- the kernel driver it needs;
- GL/GLES versions and extensions;
- the debug options table;
- known limitations: no 3D textures, NPOT cube restrictions, one render
  target, no MSAA;
- how to run the tests.

### M9. Build inputs from `tools/`

`mesa/build.sh` generates `sgx545_usse_progs.h` with `tools/gen/mkusse.py`.
Upstream, the fixed programs are assembled by the compiler's encoder at build
time (DRIVER-UPSTREAM.md §5.2, COMPILER-UPSTREAM.md).

## Firmware (the open SGX microkernel, `ukernel/`)

The microkernel is MIT-licensed source (`ukernel/ukernel.py`) assembled to a
small binary a loader runs on the SGX core. Upstreaming has two destinations:
the **source** (its own repository) and the **built binary** (linux-firmware,
loaded by the DRM driver).

| item | state |
|---|---|
| Reproducible build | **Done.** Pure-CPython assembler, no timestamps/paths/randomness; byte-identical `.bin`+`.json` on any machine, any CPython ≥ 3.7. One command: `ukernel/build.sh`. |
| Pinned toolchain | **Done (documented).** Only CPython ≥ 3.7; no external packages. Recorded in `ukernel/README.md` → Building. |
| MIT licence | **Done.** `ukernel/LICENSE` + an `SPDX-License-Identifier: MIT` header on every source file. |
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

File: powervr/sgx545-ukernel.bin
File: powervr_sgx/sgx545_ukernel.bin
Version: 1.0
Source: <source-repository-url>/ukernel   (ukernel.py + build.py; the DRM
        driver's packaging tool applies the header/feature bits)
License: MIT.  See LICENSE.<id> for details.
```

Naming (no project name upstream; per core; the driver's own subdirectory):
`powervr_sgx/<core>_ukernel.bin`, so `powervr_sgx/sgx545_ukernel.bin` here and
`powervr_sgx/sgx540_ukernel.bin` for another core. The directory matches the DRM
driver's module/request path (`powervr_sgx/`, not `powervr/`). build.py emits
`<core>_ukernel.bin`; the driver's `firmware=` request and its packaging tool
must use the same name — kept in sync across build.py, the driver, the docs and
the test scripts. Open before this is real: the MIT text must be added to
linux-firmware's licence set, and `Source` needs the public repository URL. The
source and the built binary are versioned together by `version` + `build_id`.

### Not a blocker, noted
- The microkernel source carries inline hardware-provenance comments (which
  behaviours were confirmed on which bring-up box). These are useful engineering
  history; a final upstream-source pass can thin them or move the box-specific
  ones to `docs/log/`, but they do not affect the binary and are low priority.

## Mesa winsys and platform

(uapi-stream)

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
  - The build writes `<core>_ukernel.bin`, i.e. `sgx545_ukernel.bin`, plus
    `.bin.pat` and `.json`. SRVINIT.md's `srvinit -o` example uses it.
  - The driver requests `powervr_sgx/sgx545_ukernel.bin`, the same name in
    the driver's directory; UKERNEL.md §4a uses it.
  - The DDK image keeps its own name, `sgx545-ukernel-1.7.862890.bin`.

Later and optional: the inline hardware-provenance comments in
`ukernel/ukernel.py` (see the firmware section).
