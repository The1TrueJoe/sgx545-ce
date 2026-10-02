# sgx545-ce

PowerVR **SGX545** kernel driver (`pvrsrvkm`) for the Intel Atom CE5300/CE5310
(as fitted to the Control4 EA-1 / EA-3) and Cedarview (GMA 3600/3650) — the same
SGX545 core. Ported to modern Linux from Intel's GPL DDK **1.7.862890**.

> **Status:** builds clean and links with zero unresolved symbols against Linux
> 7.1.8 / i686. **Not yet loaded on hardware.**

`src/` is Intel's DDK imported verbatim as the first commit, so `git log src/`
is an honest diff against upstream. Everything else here is ours.

## What we changed from the DDK

| Area | Change |
|---|---|
| Removed API | `__devinit*`, `MODULE_SUPPORTED_DEVICE`, `asm/system.h`, `drmP.h`, `ioremap_nocache`, `drm_mmap`, `in_irq` |
| Signatures | `get_user_pages`, `access_ok`, `class_create`, `vmf_insert_mixed`, `__vmalloc` |
| Renames | `mmap_sem`→`mmap_lock`, `del_timer_sync`→`timer_delete_sync`, `page_cache_release`→`put_page`, `VM_RESERVED`→`VM_DONTEXPAND\|VM_DONTDUMP` |
| Structural | procfs → `proc_ops` + `pde_data`; timers → `timer_setup`; page walk → `follow_pfnmap`; 5-level page tables |
| DRM | rebuilt on `drm_dev_alloc`/`drm_dev_register`; PCI driver registered separately; ioctls rebased on `DRM_COMMAND_BASE`; `pvr_drm.c` rewritten for modern DRM |
| Write-combine | command-buffer pages are `alloc` + `vmap`-ed WC (post-5.8 `__vmalloc` dropped its `pgprot` arg, which never worked for vmalloc anyway) |
| Deleted | hand-rolled PAT probing — `pgprot_writecombine()` does it |

Only two were more than mechanical: the write-combined command-buffer path, and
keeping `SUPPORT_DRI_DRM=1` (both candidate userspace blob sets call `drmOpen`).
The commit messages have the detail.

## Build

**In-tree.** Put this directory at `drivers/gpu/drm/sgx545ce/`, add it to the
parent `drivers/gpu/drm/Kconfig` (`source "drivers/gpu/drm/sgx545ce/Kconfig"`)
and `Makefile` (`obj-$(CONFIG_SGX545_CE) += sgx545ce/`), then set
`CONFIG_SGX545_CE=m` (or `=y`). `depends on X86 && DRM`.

**Out-of-tree** (fast iteration):

```bash
make                       # build against /lib/modules/$(uname -r)/build
make KERNELDIR=/path/to/src
make image && make shell   # dev container: debian + i686 cross gcc + linux 7.1.8
```

**Knobs:** `SGX_CORE_REV` (default `1014`, an EA-3; Cedarview is `10131` — same
errata) · `PVR_MODNAME` (default `pvrsrvkm`, the name userspace `drmOpen()`s).
Module params: `sgx_core_clock`, `sgx_apm`, `need_sample_cache_workaround`.

`tools/sgxregs.sh` decodes the SGX identity registers (is the core clocked? is
the register window where we think it is?) — run it before loading anything.

## Userspace

Not in this repo: the EGL/GLES blobs + `pvrsrvctl` (which uploads the SGX
microkernel) are proprietary — Intel/Imagination, binary-redistributable with
notice, no reverse engineering. Use the matched **DDK 1.7** set (the `sgx545-um`
package, or the published Cedarview tarball). Its version must match the driver's
`SGX_CORE_REV` or `pvrsrvctl` refuses the upload.

## Licensing

`src/` is GPL-2.0 (Imagination Technologies, as published by Intel). The
userspace blobs are not GPL and are not included here.
