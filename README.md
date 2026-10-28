# sgx545-ce

PowerVR **SGX545** kernel driver (`pvrsrvkm`) for the Intel Atom CE5300/CE5310
(as fitted to the Control4 EA-1 / EA-3) and Cedarview (GMA 3600/3650) — the same
SGX545 core. Ported to modern Linux from Intel's GPL DDK **1.7.862890**.

> **Status:** runs on hardware. On a Control4 EA-1 the SGX545 renders through this
> driver to the panel — a GLES triangle, then a full WPE WebKit page — with the
> i386 DDK userspace driving the x86_64 kernel over the compat layer below.

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

## 32-bit userspace on a 64-bit kernel

The only SGX545 DDK that exists is **i386** (Intel's Cedarview 1.7), but the EA
runs an **x86_64** kernel. So the driver carries a compat layer that lets the
32-bit DDK talk to the 64-bit `pvrsrvkm` over IA32 emulation:

- **Pointer widths.** `IMG_UINTPTR_T` is widened to 64-bit on LP64 (it was only
  widened for `_WIN64`), so the handle table and per-process hashes stop
  truncating kernel pointers. Device handles are index-based, so the outer bridge
  package's two user pointers and the handles translate losslessly 4↔8.
- **Per-bridge marshalling.** `in_compat_syscall()` path in the bridge dispatch
  re-stamps the ioctl's size field and, for each bridge a 32-bit caller uses,
  expands the IN struct and compacts the OUT struct between the i686 and kernel
  layouts (handles 4→8 with 8-byte alignment, raw pointers zeroed, arrays strode).
  Covers the services/SGX-init bridges, `SGXDoKick` (render), and
  `SGXSubmitTransfer` (#96) — the WSEGL framebuffer blit that puts a frame on the
  panel.

Two bugs this surfaced, both fixed: the HW-context page-directory address was
copied `sizeof(IMG_DEV_PHYADDR)` (8 on LP64) into a 4-byte slot, overflowing a
zero into the next field and faulting the GPU at device VA 0 on every kick; and
`SUBMITTRANSFER` had no compat entry, so the blit returned `-22` and nothing ever
reached the framebuffer.

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
notice, no reverse engineering. Use the matched **DDK 1.7** set:
`cedarview-graphics-drivers_20120717.orig.tar.gz` from Ubuntu's archive,
<https://old-releases.ubuntu.com/ubuntu/pool/multiverse/c/cedarview-graphics-drivers/>
(the `.dsc` beside it is PGP-signed and carries the hash). Its version must match
the driver's `SGX_CORE_REV` or `pvrsrvctl` refuses the upload.

## Licensing

`src/` is GPL-2.0 (Imagination Technologies, as published by Intel). The
userspace blobs are not GPL and are not included here.
