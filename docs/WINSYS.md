# The Mesa winsys: one recorder, two kernel drivers

The gallium driver (`mesa/src/gallium/drivers/sgx545`) sees the winsys only through
`sgxws.h`. Below that, `sgx545_frame.c` records frames the same way for every
kernel driver: rings, PDS programs, USSE finalising, TA state, the VDM stream.
How memory is allocated and how a recorded frame reaches the GPU belongs to a
backend, `struct sgxws_kif` in `sgx545_frame.h`:

| backend | file | kernel driver | who builds the firmware structures | ships in |
|---|---|---|---|---|
| powervr_sgx | `sgx545_powervr_sgx.c` | `powervr_sgx` (`drm/powervr_sgx`, docs/UAPI.md) | the kernel | upstream Mesa (M5); the boot image once powervr_sgx replaces pvrsrvkm |
| services | `sgx545_pvr.c` + `libpvr/` | `pvrsrvkm` (`kernel/`) | the client, as the DDK's GL driver does | a downstream overlay only |

## Selecting the backend

There is nothing to configure: the device decides.

- **The loader.** `mesa/patches/0005` makes the loader's `sgx545` predicate accept
  either kernel driver name (`drmGetVersion`), so EGL, GBM and DRI open the
  `sgx545` gallium driver on `powervr_sgx` and on `pvrsrvkm`.
- **The winsys.** `sgxws_dev_create` reads the same name and creates the matching
  backend. A build without the services backend fails to create a screen on
  `pvrsrvkm` and says so.
- **The node.** powervr_sgx has a render node (`/dev/dri/renderD128`), which EGL's
  surfaceless platform, GBM and DRI3 open. pvrsrvkm has only a primary node,
  which `mesa/patches/0003` lets the surfaceless platform fall back to.
- **One box, either stack.** The EA boots with one of the two kernel drivers. The
  same Mesa build runs on either: on pvrsrvkm the boot images today, on powervr_sgx
  a RAM-booted kernel with the module loaded (`drm/ci/ea-ramboot`).

`sgx545_frame_test -d <node>` runs the winsys without Mesa on either driver. On the EA1 under
powervr_sgx (2026-11-04, kernel c95e32d) it draws the probe's clear and triangle at 720x480,
pixel-identical to `drm/tests/powervr_sgx_frame`, 300 frames at 2.8 ms each.

## Building the backends

- **powervr_sgx** is always built. It needs the uAPI header in Mesa's
  `include/drm-uapi/` and the stream packers `powervr_sgx_kmd_stream.h`, which
  `mesa/build.sh` copies from `drm/powervr_sgx/include/uapi/drm/powervr_sgx_drm.h`
  and generates with `docs/uapi/gen_stream.py user` from
  `docs/uapi/powervr_sgx_kmd_stream.xml`. Upstream, the header comes from drm-next
  (`AGENTS.md` 3.1), and the generator's home is G26 in UAPI.md.
- **services** is built only when `libpvr/` is in the winsys directory, which
  `mesa/build.sh` links in. It is compiled against `kernel/src`'s bridge headers
  with the kernel's defines, into objects of its own, so no other winsys file
  sees them (`AGENTS.md` 3.5). An upstream tree has no `libpvr/`, so its meson
  build has only powervr_sgx.

## What each backend does

| | powervr_sgx | services |
|---|---|---|
| buffers | GEM objects, mapped at addresses the client picks from `HEAP_INFO` (`util_vma_heap` per heap); freed buffers are unmapped once the timeline passes the last job submitted before the free | `ALLOC_DEVICEMEM` in the services heaps, with the DDK's attributes and allocation order |
| USSE code | code objects (D12): the rings keep a CPU copy, and what was written since the last kick goes up with `UPLOAD_CODE` before it, always past what was uploaded before | written through the mapping |
| end-of-tile programs | made once per target into two code pools used in turn; a full pool is replaced by a new code object at the same address once no render runs code from it | the same pools (a pool is rewritten in place) |
| context, PB | `CREATE_CONTEXT` | PB, free list, descriptor, CCB, HW render context written by the client |
| render target | `CREATE_RT_DATASET` | RT data, render details, region headers, tail pointers written by the client |
| a frame | one `SUBMIT_JOBS` render job: a `CMD_RENDER` stream and two `CMD_TA_KICK` streams (the scene, then the terminate words), written by the generated packers | the TA command in the CCB, `SGX_DOKICK` |
| completion | each job signals the next point of one timeline syncobj per device; waits are syncobj waits | the target's sync object, polled |
| shared buffers | dma-buf import and export; a shared buffer is named in each job's BO list (the frame's target as written, sampled ones as read), so the kernel orders the job against its other users (implicit sync) and flushes the CPU cache of an imported one, which the SGX doesn't snoop | not supported (`-ENOTSUP`); the fbdev framebuffer is wrapped user memory |
| scanout (kmsro) | the render target is ours, exported to the display device (simpledrm on the EA, card0; `mesa/patches/0006`) | the fbdev platform |
| debugging aids | `SGX545_DRY_RUN` (a dry-run frame's point is signalled from the CPU) | also `SGX545_TRACE` (pvrtrace) and `SGX545_DEBUG=flushrt` |

Kick numbers stay per context in both, so the driver's fences, the ring slots and
the texture ghosting don't know which backend they run on.
