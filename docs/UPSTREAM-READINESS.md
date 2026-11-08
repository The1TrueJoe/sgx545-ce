# Upstream readiness

What has to be true before the driver stack goes to upstream review. This
covers code quality, not merge requests: none are written yet.

The platform is the Intel Atom CE5300, so code, comments and docs say
"CE5300". The aim is one gallium driver for the SGX5xx family. Core features
come from the kernel (`DEV_QUERY`) and a per-core table, not from SGX545
constants spread through the code.

## Mesa: the gallium driver and winsys

Scope: `mesa/src/gallium/drivers/sgx545` (driver) and
`mesa/src/gallium/winsys/sgx545/drm` (winsys). The compiler
(`mesa/src/sgx545/compiler`) is covered by `docs/COMPILER-UPSTREAM.md`.
`docs/DRIVER-UPSTREAM.md` (2026-11-01) is the earlier hardware-fact audit; its
doc gaps still stand. Status as of 2026-11-08, um-re 0507229 plus mg-perf.

| # | item | status | owner |
|---|---|---|---|
| M1 | upstream build is powervr_sgx only | mostly there | mesa-gallium, uapi-stream |
| M2 | features from DEV_QUERY and a core table | not started | mesa-gallium, m2 |
| M3 | driver name for the family | decision needed | Joseph |
| M4 | clang-format | not started | mesa-gallium |
| M5 | comments and naming | not started | mesa-gallium |
| M6 | debug options and experiment code | partly | mesa-gallium, usse-compiler |
| M7 | tests in Mesa's format (`ci/`) | partly | mesa-gallium |
| M8 | `docs/drivers/sgx545.rst` | not started | mesa-gallium |
| M9 | build inputs from `tools/` | open | usse-compiler |

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
