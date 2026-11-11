# Single-context cores in powervr_sgx (the SGX530)

The design for cores with one memory context, approved 2026-11-11 (with Joseph's D13,
section 2) and implemented as section 8 lists. It covers how the core table expresses
what the SGX530 lacks, and what the VM, the scheduler and the firmware loading do on a core
with one memory context. The facts and their tags are in
[cores/sgx530.md](cores/sgx530.md); the SGX545 model it departs from is
[DRM-DRIVER.md](../DRM-DRIVER.md) section 4 and [UKERNEL.md](../UKERNEL.md), "Point the BIF
at a context". **The SGX545 (CE5300, Cedarview) keeps its behavior exactly.** Every new
field holds the value the code uses today, and every new branch is taken only on a family
that sets the new property.

## 1. What differs, and where it goes

| SGX530 difference | Table entry | What the code does with it |
|---|---|---|
| One memory context: `BIF_DIR_LIST_BASE0` only, no `BIF_BANK*` | `num_dir_lists = 1`, no `MULTIPLE_MEM_CONTEXTS`; `regs.bif_bank0 = regs.bif_bank_set = 0` | `pvrsgx_reset()` writes no bank register; the EDM's list is list 0 (`pvrsgx_dirlist_edm()` already gives `num_dir_lists - 1`); section 2 |
| One event kick (`BRN_22934`, no `MULTI_EVENT_KICK`) | `regs.event_kick2 = 0` | the init kick goes to `EVENT_KICK` (section 4) |
| 28-bit device addresses, 4 KiB pages only | `va_bits = 28`, a per-family `heaps` layout | every `PVRSGX_HEAP_*` use reads the family's layout; section 5 |
| `BRN_28889`: never a BIF invalidate and a data invalidate in one command | `quirks = PVRSGX_QUIRK_BRN_28889` | `pvrsgx_ccb_send()` splits the cache control (section 3) |
| Moved registers: `CLKGATESTATUS` 0x4, `CLKGATECTLOVR` 0x8, `CORE_ID` 0x10, `CORE_REVISION` 0x14, `PDS_INV3` 0xadc | `regs.*`, as for the other cores | no new code |
| Re-laid-out fields: `CLKGATECTL` (2 bits per unit, 2D b0-1 to USE b20-21), `CLKGATESTATUS`, `BIF_FAULT` (b12-27), `BIF_INT_STAT` (fault b0-13), `USE_CODE_BASE` (address b0-23, data master b24-25) | `regs.clkgatectl_*`, `regs.clkgatestatus_*`, `regs.bif_fault_addr`, `regs.bif_int_stat_fault_*`, `regs.use_code_base_dm_shift` | no new code; the shifts and masks come from the table. The 24-bit code-base address field holds any 28-bit address >> 7 |
| The 2D block's reset bit: `SOFT_RESET` b1 is 2D, and every unit above the BIF moves up a bit | `regs.soft_reset_cores` = b1-b6 (2D, DPM, TA, USE, ISP, TSP) | the 2D unit is reset and released with the others |
| No `CLKGATECTL2` | `regs.clkgatectl2 = 0` | the write is skipped for a register that is 0, rather than written to offset 0 |
| PDE layout from a DDK 1.4 header: b1-4 are access bits, not a page size | none: `SGX_MMU_PDE_PAGE_SIZE_4K` is 0, so a PDE is the valid bit and the address on both layouts | no new code; gap 6 decides whether the 530 needs more |

A register a family lacks is 0 in its `regs`, as before. The driver now writes no register
that is 0 (`sgx_wr_opt()`: the bank registers, `CLKGATECTL2`) and reads none (the dump).
Those writes used to be unconditional, and a write to offset 0 on the 530 would have
landed on `CLKGATECTL`.

`PVRSGX_FEATURES_REQUIRED` and `PVRSGX_VA_BITS_REQUIRED` are gone. A family is usable
when it either has `MULTIPLE_MEM_CONTEXTS` and 2 to 16 lists, or is single-context with
exactly one; `va_bits` is 28 or 32 with a heap layout that fits; it has the event PDS
registers; and it has `EVENT_KICK2` exactly when it has `MULTI_EVENT_KICK`.

The event-PDS registers and `EDM_TASK_REG0/1` (`SGX545_CR_*` in `pvrsgx_hw.h`, measured on
the SGX545, not in any GPL header) are **unknown** on the 530. Until the 530 microkernel
work names them, the family can't start a microkernel, and it isn't usable. The probe then
logs the two ID registers and stops at "this driver can't run an SGX530 yet". Its part
entry stays out of the table until the microkernel starts (porting/kernel.md: "A part
that only gets as far as stage 3 stays out").

## 2. One page directory list: the VM model

On the SGX545 the EDM (the microkernel) translates through list 15, which only ever holds
the firmware's directory, and the TA and 3D through list 0, which holds the job's VM
directory. Client directories hold nothing of the kernel's (secure mode, UAPI.md P2/P3).
On the SGX530 the EDM, TA, 3D and 2D all translate through list 0. So:

1. **The firmware heaps are in every VM's directory.** The kernel code, kernel data and
   sync heaps are whole 4 MiB directory entries. `pvrsgx_device_init()` gives them page
   tables in the firmware directory up front (`pvrsgx_mmu_prealloc()`), and every new VM's
   directory points those entries at the same tables (`pvrsgx_share_fw_heaps()`,
   `pvrsgx_mmu_share()`). The tables are shared, not copied, so a later kernel mapping
   shows in every VM at once, at the same address. A shared entry has no table of the
   VM's own, and the MMU code refuses to allocate one over it. This is the
   `share_kernel_heaps` of the out-of-tree DDK patch, moved into the driver and keyed on
   the core rather than the firmware.
2. **When idle, list 0 holds the firmware directory** (`pvrsgx_idle_mmu()`), because the
   EDM keeps running between jobs. `pvrsgx_reset()` ends with list 0 = firmware directory,
   so every microkernel start (boot, recovery, resume) finds it there. The scratch
   directory that soaks up stale faults during recovery stays unshared: the microkernel
   isn't running then, and a scratch page must not land in the shared tables.
3. **The microkernel switches list 0** (agreed with the SGX530 microkernel session,
   2026-11-11). At a scene's first TA kick, and at a transfer, it writes the job's
   directory (HW context word 2, as on the 545) into `DIR_LIST_BASE0` and pulses
   `INVALDC`. At init it saves what `DIR_LIST_BASE0` holds, the firmware directory, in
   control-block word 0x27. Before it raises `SW_EVENT` with nothing in flight it writes
   that back, with `INVALDC`. It never touches a bank register.
4. **A directory is freed only when list 0 can't hold it.** VM teardown already waits for
   every job of the VM, and the VM's kernel objects keep it until a later render. The
   microkernel's restore can still come after a job's completion shows, so before
   `vm_release()` frees a directory, `vm_pd_unload()` reads `DIR_LIST_BASE0` (only while
   the core is powered) and moves it back to the firmware directory if it still holds
   this one. A suspended core holds nothing: the next start's reset sets list 0.
5. **Per-VM structures are unchanged:** each VM has its own directory, its own 3D
   parameters heap and its PB at the heap base.

**What this does to isolation.** The firmware heaps are in every client's translation, so
P2 and P3 (UAPI.md 7.2) are open on the 530, as they are on the 545 under the DDK
microkernel. On the 545, P2 failed even for pages mapped `EDM_PROTECT`. On the 530 it is
untested (fact-sheet gap 6, extended: a client task loads from an `EDM_PROTECT`
kernel-data page). Worse, the directory addresses the microkernel loads (HW context
word 2, control-block word 0x27) sit in GPU-writable pages, so a client that writes one
can make the GPU translate through any physical memory. On a single-context core,
access to the render node gives access to all of memory.

**Decision D13 (Joseph, 2026-11-11): opt-in.** A single-context core runs only with the
module parameter `powervr_sgx.allow_unisolated=1`.
- Without it, the probe reads `CORE_ID` and `CORE_REVISION`, logs why it refuses the core,
  and stops before any write.
- With it, the probe warns, and `QUIRKS_GET` lists `DRM_POWERVR_SGX_QUIRK_UNISOLATED`
  (an opt-in quirk, not an erratum number), so userspace can tell.
- No `CAP_SYS_ADMIN` gate, which would break ordinary desktop clients.
- No isolation claim for single-context cores anywhere, upstream included.
- Multi-context cores (the 545) never need the parameter and never report the quirk.

A hardening for later, not agreed yet and not needed for the opt-in: move the directory
addresses into a host-written table the GPU sees read-only, with HW contexts naming an
entry rather than an address. That would close the physical-memory hole and leave only
reads and corruption of the microkernel's own state (hangs, early fences).

**Before a second client runs on hardware**, test the restore of point 3 and the check of
point 4 (the coordinator's condition): two VMs, each with jobs, the first one closed while
the second renders. Check that `DIR_LIST_BASE0` is the firmware directory between jobs and
never a freed one.

## 3. BRN_28889

`sdev->fw.cache_control` gathers invalidates for the next command: BIF PT and PD after
every kernel-object create, and DATA after every context release. Both happen together
routinely. With the quirk set, when a command would carry DATA together with PT or PD,
`pvrsgx_ccb_send()` first sends a `PROCESS_QUEUES` command carrying only the BIF bits. It
waits until the CCB read offset passes it (a bounded 500 ms poll under `ccb_lock`, with
no allocation and no fence wait), and then sends the real command with DATA only.
Without the quirk (the 545) the command goes out as before, with the same words. The 530
microkernel handles it on its side too: a command that asks for both gets the BIF part
first, and the data part one event later. The host split makes that path idle, and both
together are harmless.

## 4. Firmware loading and start

- **File:** `powervr_sgx/sgx530_ukernel.bin`, from the 530 microkernel build, per part as
  today (`info.fw_name`), with 0x00010205 as the header's validated core revision. The
  header checks are unchanged: magic, interface version, CRC, entries, patches, and the
  warning for another core revision.
- **Mode:** secure mode (`PVRSGX_UKFW_SECURE`: firmware heaps in no client directory)
  can't hold on the 530. On a single-context core the loader refuses an image that claims
  it, and the 530 image declares features 0. A multi-context core still requires it.
- **Start:** the same sequence, with three per-family differences: no `CLKGATECTL2`
  write; the reset leaves list 0 = firmware directory, with no bank registers; the init
  kick goes to `EVENT_KICK` (`init_kick()`). The host writes `INIT_STATUS` 0 before the
  kick, as today, and the microkernel tells the init kick from a CCB kick by that word.
  `EVENT_KICKER`'s 28-bit field holds the kicker buffer, which is below 256 MiB in the
  530's layout.
- **What the 530 still lacks:** the event-PDS registers and `EDM_TASK_REG0/1`, which
  enter the microkernel (`SGX545_CR_*` on the 545, measured, not in any GPL header), and
  any init-script part 2 the 530 microkernel needs. Until its session names them, the
  family isn't usable (`pvrsgx_core_family_usable()`), and the probe stops after the ID
  read. A part entry, with the revision read on hardware, comes when the microkernel
  starts.
- **Runtime PM:** unchanged. When `sgx_pwrdm` turns off the core loses everything, and
  resume already runs the full start.

## 5. 28-bit addresses: the heap layout

The fixed layout (`pvrsgx_fw.h`, UAPI.md section 5) runs to 4 GiB. It becomes a
`struct pvrsgx_heap_layout` per family. The SGX545 table holds today's values exactly.
For the SGX530, a proposal within 256 MiB, every heap on a 4 MiB directory-entry boundary
so the shared kernel heaps are whole entries:

| heap | base | size | note |
|---|---|---|---|
| (none) | `0x00000000` | 4 MiB | null addresses fault |
| 3D params | `0x00400000` | 16 MiB | PB within 0x10000 pages of the base; `pb_max_pages` 0x800, the free list and the rest sharing the heap |
| TA data | `0x01400000` | 8 MiB | |
| sync | `0x01c00000` | 4 MiB | kernel |
| kernel code | `0x02000000` | 4 MiB | kernel, shared into every VM |
| kernel data | `0x02400000` | 8 MiB | kernel, shared into every VM |
| PDS pixel | `0x02c00000` | 4 MiB | `PDS_EXEC_BASE` |
| PDS vertex | `0x03000000` | 4 MiB | |
| USSE pixel | `0x03400000` | 5 MiB | code bases 0-9; the base unit is gap 9 |
| USSE vertex | `0x03c00000` | 2 MiB | code bases 10-13 |
| general | `0x04000000` | 128 MiB | |
| texture | `0x0c000000` | 64 MiB | |

The microkernel's own code base (15) stays at the kernel code heap. `VM_MAP`, the kernel
object ranges, the code-base programming and `HEAP_INFO` all read the layout. The DMA mask
stays 32 bits: PTEs carry 32-bit physical addresses on both cores. `LIMITS_GET` takes
`pb_max_pages` from the family as well. The other limits stay as on the 545 until the
fact sheet's gap 12 gives the 530's own values.

## 6. The scheduler

One `drm_sched` with a credit limit of 1. The `overlap` parameter, which lets a second
job out so a TA can run under the previous 3D, is ignored on a single-context core: one
directory list can't hold two VMs at once, and the 530 microkernel runs one job at a time
across VMs. Jobs from different VMs serialize, each switch a directory load and a BIF
invalidate. Timeout and recovery are unchanged. Recovery's restart re-runs the reset of
section 2, so list 0 comes back on the firmware directory.

## 7. The uAPI

No change to any ioctl or struct. One new define: `DRM_POWERVR_SGX_QUIRK_UNISOLATED`
(section 2).
- `HEAP_INFO_GET` already reports every heap's base, size, page size, code-base index and
  window. A client that takes its layout from the query, as the uAPI requires, works on
  the 530 unchanged. Mesa must not keep heap addresses as constants (the comments in the
  winsys' `sgx_frame.h` enum list the 545's; that's for the Mesa session).
- `GPU_INFO_GET` returns the 530's `CORE_ID` and `CORE_REVISION`, which is how Mesa picks
  the core.
- `LIMITS_GET` takes `pb_max_pages` from the family (0x800 on the 530), and
  `CREATE_CONTEXT` checks it.
- `QUIRKS_GET` lists `DRM_POWERVR_SGX_QUIRK_UNISOLATED` on a single-context core, and
  nothing on the 545. Neither erratum reaches userspace.

## 8. Where it is implemented

- `pvrsgx_core.c`, `.h`: the family fields (`heaps`, `quirks`, `regs.bif_bank*`,
  `bif_fault_addr`, `use_code_base_dm_shift`), the SGX545 and SGX530 layouts, the usable
  check, `allow_unisolated`.
- `pvrsgx_device.c`: the reset without banks, the opt-in check, the shared firmware heaps.
- `pvrsgx_fw.c`: `clocks_on()`, `init_kick()`, the code bases and arenas from the layout,
  the secure-mode rule.
- `pvrsgx_vm.c`: the client heaps from the layout, sharing at VM creation,
  `vm_pd_unload()`.
- `pvrsgx_ccb.c`: the BRN_28889 split.
- `pvrsgx_job.c`, `pvrsgx_kobj.c`, `pvrsgx_render.c`, `pvrsgx_transfer.c`, `pvrsgx_pds.c`,
  `pvrsgx_dump.c`: the layout, no overlap, no reads of absent registers.
- KUnit: `powervr_sgx_regs` (the 545 writes its bank registers, list 15, `CLKGATECTL2` and
  `EVENT_KICK2`, and the 530 writes none of them and leaves offset 0 alone), and
  `powervr_sgx_core` (the 545's layout is the fixed one, and the 530's fits 28 bits on
  4 MiB boundaries).
- `drm/ci/ddk-compat.patch` is rebased: its sharing now uses the in-tree MMU helpers.

The SGX545 runs the same register writes, CCB words and heap addresses as before. The
545's CI checks that (KUnit, W=1, sparse, checkpatch, kernel-doc, the CE5300 and
Cedarview builds), and `powervr-sgx-omap3630` builds the result for ARM.
