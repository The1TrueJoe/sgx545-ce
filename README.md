# sgx545-ce

PowerVR SGX545 kernel driver for the Intel Atom CE5300 family (CE5310 as
fitted to the Control4 EA-1 and EA-3), ported to mainline Linux.

**Status: builds clean and links with zero unresolved symbols against Linux
7.1.8 / i686. It has never been loaded on hardware.** Everything below that
line is verified; everything past it is not.

```
pvrsrvkm.ko   156 KB   alias pci:v00008086d0000089B   depends: (none)
```

---

## Why this exists, and why it isn't a reverse-engineering project

The assumption was that Series5 SGX is a dead end: no Mesa driver has ever
existed for it, mainline's `powervr` DRM is Rogue-only, and Control4 shipped
no source for their `pvrsrvkm.ko`.

That's all true and it doesn't matter, because **the same SGX545 core shipped
in Intel Cedarview** (GMA 3600/3650) — and Intel published the complete GPL v2
kernel services source for that one. It is still downloadable:

```
http://old-releases.ubuntu.com/ubuntu/pool/multiverse/c/cedarview-drm-drivers/
  cedarview-drm-drivers_20120717.orig.tar.gz
    sha256 200b2f6348a8965a1528088f9c9fe249df09d709a401d941e1853be5d99a1a69
  cedarview-drm-drivers_20120717-0ubuntu1.debian.tar.gz
    sha256 4565b6cf2de71e555dd072ad87618e4e9070e340281dc08a079919a96a643fb3
```

The `orig` tarball ships the directories **empty** — the source lives in
`debian/patches/07_kernel-3.2-cdv.diff` (3.47 MB). Apply the quilt series and
`staging/cdv/pvr/` materialises: DDK **1.7.862890**, 78,349 lines, including a
62 KB `sgx545defs.h` register map and Intel's PCI-attach system layer.

The first commit here is that tree, imported verbatim. Everything after it is
ours, so `git log src/` is an honest diff against upstream.

### The three things that made it tractable

**The microkernel is in userspace and unsigned.** `libsrv_init.so` is 9.8 KB of
code and 54 KB of `.rodata` — the SGX EDM microkernel, uploaded by
`/bin/pvrsrvctl` through `SGXDevInitPart2`. A ported kernel driver never has to
contain, sign, or synthesise firmware; it only has to accept the upload.

**Our silicon is an enumerated case.** `sgxerrata.h` handles
`SGX_CORE_REV == 1014`, which is what an EA-3 reports. Cedarview ships 10131.
Both carry the identical single erratum, `FIX_HW_BRN_SAMPLE_CACHE`.

**Control4's binary is the same tree.** Their `pvrsrvkm.ko` is not stripped and
embeds `__FILE__` paths: it was built from 43 source files, and 40 of them are
in Intel's 1.7 tree. The missing three are `refcount.c` (debug no-ops),
`mutex.c` (trivial) and `pvr_sync.c` (Android explicit fences).

---

## What was ported

Full detail is in the commit messages; `git log --oneline src/` is the index.
The shape of it:

| Area | Change |
|---|---|
| Removed API | `__devinit` family, `MODULE_SUPPORTED_DEVICE`, `asm/system.h`, `drmP.h`, `stdarg.h`/`stddef.h`, `ioremap_nocache`, `drm_mmap`, `in_irq` |
| Signature churn | `get_user_pages`, `access_ok`, `class_create`, `vmf_insert_mixed`, `__vmalloc` |
| Renames | `mmap_sem`→`mmap_lock`, `del_timer_sync`→`timer_delete_sync`, `page_cache_release`→`put_page`, `VM_RESERVED`→`VM_DONTEXPAND\|VM_DONTDUMP` |
| Structural | procfs → post-3.10 (`proc_ops` + `pde_data`), timers → `timer_setup`, page walk → `follow_pfnmap`, five-level page tables |
| DRM | `drm_driver` rebuilt around `drm_dev_alloc`/`drm_dev_register`; the PCI driver registered separately; ioctl numbers rebased on `DRM_COMMAND_BASE` |
| Deleted | hand-rolled PAT probing (the kernel's `pgprot_writecombine` does it) |

Two were more than mechanical:

**Write-combined allocations.** `__vmalloc()` lost its `pgprot` argument in 5.8
because it never worked — vmalloc leaves the direct-map alias cached, so asking
for write-combining produced two mappings of one page with different memory
types. `set_memory_wc()` isn't the replacement either; it does `__pa()` on its
argument, meaningless for a vmalloc pointer. The pages are now allocated and
`vmap()`ed with the protection we want. This is the command-buffer path, where
the GPU reads what the CPU just wrote, so a stale cached alias would look like
the core executing old commands.

**DRM is not optional.** It looked droppable — the DDK has a plain-chardev path
and dropping DRM would have cut the port from ~558 files to ~43. But both
candidate userspace blob sets link `libdrm` and call `drmOpen`, and Control4's
own module contains `pvr_drm.c` and `sPVRDrmIoctls`. The stock system's
`/dev/dri/card0` is that node. So `SUPPORT_DRI_DRM=1` stays, and `pvr_drm.c`
was rewritten for modern DRM instead — 479 lines, a small surface.

---

## Building

```bash
make image     # dev container: debian + i686 cross gcc + linux 7.1.8
make           # build pvrsrvkm.ko
make errors    # build and summarise failures by file and kind
```

The container cross-compiles from arm64 rather than emulating amd64. To check
symbols for real you need a kernel with a `Module.symvers`:

```bash
docker build -f dev/Dockerfile.full -t sgx545-dev-full .
docker run --rm -v "$PWD:/src" sgx545-dev-full make -C /k/linux M=/src modules
```

### Build knobs

| Knob | Default | Notes |
|---|---|---|
| `SGX_CORE_REV` | `1014` | What an EA-3 reports. Cedarview is `10131`; the errata are identical. |
| `PVR_MODNAME` | `pvrsrvkm` | The name userspace passes to `drmOpen()`. Control4's blobs carry this string literally; an unmodified DDK 1.7 build registers `PowerVR`. |
| `CONFIG_SGX545_CE` | `m` | The Buildroot package sets `y` — openHC builds `CONFIG_MODULES=n`. |

### Module parameters

| Parameter | Default | Notes |
|---|---|---|
| `sgx_core_clock` | 400000000 | Watchdog timing only; does not clock the GPU. No published figure exists for this part — Cedarview's SGX545 ran at 400 MHz. Worth measuring. |
| `sgx_apm` | 0 | Active power management, off until bring-up is stable. |
| `need_sample_cache_workaround` | per `SGX_CORE_REV` | Runtime half of `FIX_HW_BRN_SAMPLE_CACHE`. |

---

## Using it from openHC

Add as a submodule under `packages/`, where `board/external.mk`'s
`packages/*/*.mk` glob will find it:

```bash
git submodule add https://github.com/openHC/sgx545-ce packages/sgx545-ce
```

Then in `board/Config.in`:

```
source "$BR2_EXTERNAL_OPENHC_PATH/../packages/sgx545-ce/Config.in"
```

and add `BR2_PACKAGE_SGX545_CE=y` plus the contents of `linux.fragment` to the
board's kernel config. The package installs the source into
`drivers/gpu/drm/sgx545ce/` and appends one `obj-y` line, the same shape as
openHC's own `OHC_KERNEL_DRIVERS_HOOK`.

---

## On the bench

`tools/sgxregs.sh` is the first thing to run, before loading anything:

```
sgxregs                 # decode the identity registers
sgxregs --raw           # plus the first 64 words of the register block
```

It finds the SGX by PCI id, reads BAR0 out of sysfs, and decodes
`EUR_CR_CORE_ID` and `EUR_CR_CORE_REVISION`. It answers the cheapest question
worth asking — is the block clocked, and is the register window where we think
it is — in about a minute. If the core is power-gated at boot, every later
symptom looks like a driver bug and isn't.

`tools/test-sgxregs.sh` self-checks its arithmetic against a mocked sysfs and
devmem, so the decode can be trusted without hardware.

### Known unknowns

These cannot be settled by reading, only by trying:

1. **Is the register block at BAR0+0?** Every other Intel SGX part puts it at
   `+0x40000` or `+0x80000`; this one is its own PCI function. Established by
   disassembling Control4's module — its `SysLocateDevices` stores
   `OSPCIAddrRangeStart(hSGXPCI, 0)` straight into `sRegsSysPBase` with no add,
   where Cedarview's adds `SGX_REGS_OFFSET`. Confident, not proven.
   `sgxregs.sh` answers it.

2. **Which userspace?** Cedarview's DDK 1.7 blobs are a matched pair with this
   kernel source, i686, same core, and include `libpvrPVR2D_LINUXFBWSEGL.so` —
   EGL straight onto `/dev/fb0`, no X, which suits openHC better than
   Control4's DRI/GDL-only set. But their microkernel is built for core rev
   10131, and they open the DRM device by bus id rather than by name.
   Control4's DDK 1.12 blobs are known to work on this exact board but will
   fail this driver's DDK version check.

3. **The core clock.** Affects watchdog timeouts only, but a wrong value makes
   hardware-recovery fire at the wrong time and that is a confusing way to
   debug.

---

## Licensing

`src/` is GPL v2 — Imagination Technologies, `gpl-support@imgtec.com`, as
published by Intel. Redistribute and modify freely.

The userspace blobs are **not** in this repository and are not GPL. Intel's
`license.txt` permits redistribution in binary form with notice but forbids
reverse engineering them. Using them as-is is fine; disassembling them is not.
