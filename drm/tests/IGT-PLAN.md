# IGT plan for powervr_sgx (not ported yet)

New DRM drivers are expected to have IGT coverage for their uAPI. Our tests are
standalone C in IGT's shape (`drm/tests`). This is how they map onto
igt-gpu-tools, checked against its tree of 2026-11-10 (dbd6adf), whose
`tests/imagination/` (the Rogue driver, `pvr_*`) is the closest model: our uAPI
follows the same rules.

## Where it goes

| IGT path | What | From |
|---|---|---|
| `lib/drmtest.[ch]` | `DRIVER_POWERVR_SGX`, `{ DRIVER_POWERVR_SGX, "powervr_sgx" }` in `modules[]`, `is_powervr_sgx_device()`, `igt_require_powervr_sgx()`, `chipset_to_str()` | `docs/new_driver.md` |
| `include/drm-uapi/powervr_sgx_drm.h` | the uAPI header | the kernel's `headers_install`, once the uAPI is merged; a local copy until then |
| `lib/igt_powervr_sgx.[ch]` | ioctl wrappers with expected-errno variants (create BO, mmap offset, VM context, `VM_MAP`/`VM_UNMAP`, contexts, RT datasets, `SUBMIT_JOBS`, `UPLOAD_CODE`, `DEV_QUERY`), and a heap allocator as `igt_pvr` has | the helpers at the top of `powervr_sgx_test.c` |
| `lib/powervr_sgx/` | the frame and blit builders the GPU tests draw with: `sgxgen.[ch]`, `usse_progs.h`, `sgxtex.[ch]` (3,445 lines, MIT) | `tools/gen`, `tools/tex` |
| `tests/powervr_sgx/` | one binary per area, below; its `meson.build` like `tests/imagination/meson.build`, installed to `libexecdir/powervr_sgx` | `drm/tests` |

Two details found while checking:
- `__is_device()` reads at most 11 characters of the driver name, and `powervr_sgx` is
  exactly 11, so the exact match works. It doesn't collide with Rogue's `powervr`.
- `sgxgen.[ch]` and `sgxtex.[ch]` have no SPDX line yet. They need `MIT` and the copyright
  line before they can move.

The structured testplan comments (`docs/test_documentation.md`) are used for Xe and i915;
`tests/imagination` uses `IGT_TEST_DESCRIPTION` and `igt_describe()` only, and so do we.

## The tests

Subtest names stay as they are now, so a result maps back to the standalone test.

### uAPI, no GPU work beyond NULL jobs (from `powervr_sgx_test`, 40 subtests)

| IGT binary | Subtests |
|---|---|
| `powervr_sgx_dev_query` | `dev-query-gpu-info`, `dev-query-short-size`, `dev-query-limits-quirks`, `dev-query-heap-info`, `dev-query-invalid` |
| `powervr_sgx_gem` | `create-bo`, `create-bo-invalid`, `mmap-bo`, `mmap-bo-invalid`, `code-bo`, `prime` |
| `powervr_sgx_vm` | `vm-create-destroy`, `vm-map-unmap`, `vm-map-overlap`, `vm-map-kernel-ranges`, `vm-map-invalid`, `vm-handles-per-file`, `vm-release-frees` |
| `powervr_sgx_context` | `context-create-destroy`, `context-invalid`, `context-state-invalid`, `rt-dataset-create-destroy`, `rt-dataset-invalid` |
| `powervr_sgx_submit` | `submit-null-job`, `-timeline`, `-chain`, `-many`, `-bo-ref`, `-vm-destroyed`; `submit-invalid`, `submit-other-file`, `submit-other-vm`, `submit-wait-unsignalled`, `submit-render-invalid`, `submit-transfer-invalid` |
| `powervr_sgx_upload_code` | `upload-code-corpus`, `upload-code-reject`, `upload-code-invalid`, `upload-code-mapped` |
| `powervr_sgx_reserved` | `reserved-bits` (every reserved field and padding of every ioctl) |

The port is mechanical: `igt_main`, an `igt_fixture` that opens the render node with
`drm_open_driver_render(DRIVER_POWERVR_SGX)`, one `igt_describe()` + `igt_subtest()`
per entry, and `igt_assert_eq()` in place of our checks. `upload-code-corpus` needs the
generated corpus header (`drm/tests/usse_corpus.h`) next to the test.

### On the GPU (from `powervr_sgx_frame`, `_blit`, `_hang`, `_prime`)

| IGT binary | Subtests | From |
|---|---|---|
| `powervr_sgx_render` | `frame-one-kick`, `frame-two-kicks`, `frames-sizes`, `new-rt-dataset-each-frame`, `contexts-share-pb` (3 contexts), `context-replaced`, `queue-depth` (4 in one submit) | `powervr_sgx_frame` and its options |
| `powervr_sgx_transfer` | `copy`, `upload-readback`, `box-mip`, `texture-heap-source`, `second-client` (the released-context invalidate) | `powervr_sgx_blit` and `-T`, `-2` |
| `powervr_sgx_reset` | `mixed-submit`, `hang-recovery` (guilty and innocent contexts, the reset count, a devcoredump), `pbe-to-code-object-faults` | `powervr_sgx_hang` and `-E` |
| `powervr_sgx_pm` | `autosuspend`, `render-from-suspended`, `blit-from-suspended` | `powervr_sgx_hang`'s idle part |
| `powervr_sgx_prime` | `render-into-kms-dumb`, `kms-imports-render-target` | `powervr_sgx_prime -a`, `-b` |

`powervr_sgx_prime` needs a KMS device next to the GPU (simpledrm on the CE5300, gma500 on
Cedarview): it opens it with `drm_open_driver_master(DRIVER_ANY)` and skips without one. It
reads the scanout back through the KMS device's dumb mapping, not `/dev/mem` as now.
The device only probes once its microkernel has started, so the GPU binaries need nothing
beyond `igt_require_powervr_sgx()`.
`powervr_sgx_pb` is a setup tool, not a test, and stays out.

## Order

1. The lib changes, the header copy and the seven uAPI binaries. They need nothing from
   `tools/`.
2. `lib/powervr_sgx/` (with SPDX lines added first), then the GPU binaries.
3. Run them under `igt_runner` on a CE5300 board and a Cedarview machine. The standalone
   tests keep running in our CI until IGT carries the same coverage.
4. To igt-dev once the kernel uAPI is accepted: IGT takes uAPI headers from the kernel
   tree, so the series can go as an RFC next to the kernel RFC before that, not as a merge.
