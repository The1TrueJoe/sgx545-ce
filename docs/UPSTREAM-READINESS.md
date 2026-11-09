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
    `-Dgallium-sgx545-pvrsrvkm=disabled` by default and openHC-only. Today
    the build switches on whether `libpvr/pvr.c` exists (`fs.exists`), while
    `sgx545_frame.h` already talks of a meson option.
  - `sgx545_pvr.c`, `libpvr/` and the `include/sgx545-kernel` link are built
    from the GPL kernel's headers. They stay out of the upstream tree: an
    openHC patch on top, deleted by removing two files and one meson block.
  - Backend-specific debug knobs move into that backend: `flushrt`, which
    sends `SGX_FLUSH_HW_RENDER_TARGET`.
  - `SGX545_TRACE`/`SGX545_DRY_RUN` write a pvrtrace log for
    `tools/decode`. It is a development aid either way: decide whether it
    stays as a documented debug option or becomes openHC-only.
  - `resource_from_user_memory` (the fbdev framebuffer, services only) and
    the EGL fbdev platform stay openHC's.

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
loader patch (0005), kmsro (0006), the openHC packaging and every session's
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

- About 50 comments in the driver, winsys and compiler name the openHC
  application, the bench box or game titles ("the openhc-minecraft overlay",
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

File: powervr/sgx545-openhc-ukernel.bin
Version: 1.0
Source: <source-repository-url>/ukernel   (ukernel.py + build.py; the DRM
        driver's packaging tool applies the header/feature bits)
License: MIT.  See LICENSE.<id> for details.
```

Open before this is real: the `powervr/` subdirectory name and the exact file
name are the DRM driver's to settle (it owns the `firmware=` request path); the
MIT text must be added to linux-firmware's licence set; `Source` needs the
public repository URL. The source and the built binary are versioned together
by the manifest `version` + `build_id`.

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
- "openHC's microkernel" becomes "the open microkernel";
- IP addresses, box workspaces and product names go.

Long bring-up narratives move to a dated `docs/log/entries/` file, with a
pointer left in their place.

Remaining box-specific references, counted on 2026-11-08 by
`grep -E 'Control4|\bEA[0-9]\b|EA-[0-9]|HC-800|10\.0\.0\.|openHC|Bench plug'`.
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
- **Firmware file names follow the per-core naming** (dcd50df).
  - The build writes `<core>-ukernel-openhc.bin`, now
    `sgx545-ukernel-openhc.bin`. SRVINIT.md's `srvinit -o` example uses it.
  - The driver requests the packaged image as
    `powervr_sgx/sgx545-openhc-ukernel.bin`; UKERNEL.md §4a uses it.
  - Both match the code (`ukernel/build.py`, `pvrsgx_fw.c`), so no rename was
    needed.
  - The WHENCE draft's `powervr/` directory differs from the driver's
    `powervr_sgx/`. The firmware section already lists it as open.

Later and optional: the inline hardware-provenance comments in
`ukernel/ukernel.py` (see the firmware section).

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
- [x] **Cedarview entry.** SGX545 1.0.13 (0x0001000d), from the first tester
  report (an N2600): same features, ISA and microkernel as the CE5300.
- [x] **Firmware name.** The CE5300 entry names
  `powervr_sgx/sgx545_ukernel.bin`, the name agreed with the firmware section
  (one file per core under the module's directory).
- [ ] **Upstream tree layout.** The uAPI header moves to
  `include/uapi/drm/powervr_sgx_drm.h` (now a symlink under the driver);
  `drm/MAINTAINERS.fragment` and `drm/Documentation/gpu/powervr_sgx/` are
  ready to move.
- [x] **Module parameters.** Down to `job_timeout_ms` and `reap_after_render`
  (the second waits on the graveyard policy below).
- [ ] **One parameter buffer per device (cross-client exposure).** Every
  render context of every client shares the device's PB, mapped at the same
  3D-params address in every VM (2c1f6ac), as the DDK shares one PB
  descriptor (`pb.c`, `SGXFindSharedPBDescKM`). The DPM's free-list state is
  one set of registers, and the open microkernel, like the DDK's, doesn't
  switch it between PBs; two PBs ran each other's lists off their ends (bug
  B). The cost: client A's GPU work can address client B's binned parameter
  data, a cross-process read of geometry. Userspace can't reach the PB any
  other way: the 3D-params range is outside every client heap, so `VM_MAP`
  refuses it and no CPU mapping exists. Reviewers will ask. The follow-up is
  a PB per client with the microkernel storing and loading the DPM free-list
  state on a PB change (ta3d-state's rules, `UKERNEL.md` e2d08c2); the
  kernel side is the per-VM PB this replaced.
- [ ] **Graveyard policy.** Released firmware objects are kept until the next
  restart unless `reap_after_render`; to settle with open-ukernel whether a
  later render's completion is enough, then drop the parameter.

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
  Single-process tests: clean (3ca68ab; DMA segment size fixed). Open: a
  three-process soak took the box's userspace down (log 2026-11-08-2300).
- [ ] **IGT.** The uAPI tests are IGT-shaped C (`drm/tests`); upstream will
  want them as an IGT `powervr_sgx` test binary.

### Idioms

- [x] `devm_drm_dev_alloc`, `pcim_enable_device`, `pcim_iomap_region`.
- [x] Runtime PM is set up before `drm_dev_register`, and torn down if it fails.
- [x] `drm_sched` (one entity per context, credit 1), `drm_exec` for the
  submit's reservations, `drm_gpuvm` for VMs and their shared reservation.
- [x] **dma-fence signalling rules.** The hardware fence is allocated at
  submit and only initialised in `run_job`; the timeout's devcoredump
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

**Status (m2-drm a8dff09, 2026-11-08).** The driver is 7,458 lines; the
follow-up files hold 690, leaving about 6,800 for v1. That is above the
5,900 first estimated: v1 keeps dma-buf import (X needs it), and it carries
533 lines of firmware-structure definitions with kernel-doc, which replaced
word offsets, plus the 690-line stream decoder shared with Mesa. The v1
patch series also carries the 873-line uAPI header and 239 lines of
Documentation.

| step | state |
|---|---|
| DDK compat out | done: `drm/ci/ddk-compat.patch`, applied only by the CE5300 test kernels |
| cleanup commands, 8 developer parameters, the render snapshot and last-render debugfs, the signature lists, HWPerf buffer and `GETMISCINFO` check the open microkernel never reads | deleted |
| F1 transfer | own file (`pvrsgx_transfer.c`, 209 lines) |
| F3 runtime PM | own file (`pvrsgx_pm.c`, 126 lines), built with `CONFIG_PM`, stubs otherwise; no `runtime_pm` parameter |
| F4 coredump, F5 debugfs, F6 tracepoints, F7 gma500 | already separate files; their call sites are still in v1 code |
| PRIME import | own file (`pvrsgx_prime.c`), but in v1: X through kmsro on simpledrm imports its scanout buffers |
| firmware structures | done: TA command, dependency block, render details, RT data, PB descriptor, HW render and transfer contexts, transfer command, control block (`pvrsgx_fwif.h`, offsets checked by `static_assert`); no firmware word is written by offset any more |
| DDK names in comments | gone (the uAPI's `DRM_POWERVR_SGX_FW_IF_SGXMKIF_1_7` is left for uapi-stream) |
| generated register header | done: `pvrsgx_regs.h` comes from the GPL `sgx545defs.h` through `drm/tools/gen_regs.py` (CI diffs it); our names for the registers that header leaves out, and the MMU constants, are in `pvrsgx_hw.h` |
| one `pvrsgx_` prefix | done (the shared stream files are now `pvrsgx_stream*`) |
| ISA generation in the core table | done: the validator picks its USSE form table by the core's ISA |
| runtime checkers on the CE5300 | single-process tests clean under lockdep, KASAN, UBSAN, DMA-API debug and kmemleak (3ca68ab, one finding fixed); a three-process soak lost the box's userspace, cause open |

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
