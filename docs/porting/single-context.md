# Single-context cores in powervr_sgx (the SGX530)

A design note, for approval before implementation. It covers how the core table expresses
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
| Re-laid-out fields: `CLKGATECTL` (2 bits per unit, 2D b0-1 to USE b20-21), `CLKGATESTATUS`, `BIF_FAULT` (b12-27), `BIF_INT_STAT` (fault b0-13), `USE_CODE_BASE` (address b0-23, data master b24-25) | `regs.clkgatectl_*`, `regs.clkgatestatus_*`, `regs.bif_fault_addr`, `regs.bif_int_stat_fault_*`, `regs.use_code_base_addr`, `regs.use_code_base_dm_shift` | no new code; the shifts and masks come from the table |
| The 2D block's reset bit: `SOFT_RESET` b1 is 2D, and every unit above the BIF moves up a bit | `regs.soft_reset_cores` = b1-b6 (2D, DPM, TA, USE, ISP, TSP) | the 2D unit is reset and released with the others |
| No `CLKGATECTL2` | `regs.clkgatectl2 = 0` | the write is skipped for a register that is 0, rather than written to offset 0 |
| PDE layout from a DDK 1.4 header: b1-4 are access bits, not a page size | none: `SGX_MMU_PDE_PAGE_SIZE_4K` is 0, so a PDE is the valid bit and the address on both layouts | no new code; gap 6 decides whether the 530 needs more |

A register a family lacks is 0 in its `regs`, as today. The driver writes no register that
is 0 (the bank registers, `CLKGATECTL2`, `EVENT_KICK2`). Today those writes are
unconditional, and a write to offset 0 on the 530 would land on `CLKGATECTL`.

`PVRSGX_FEATURES_REQUIRED` and `PVRSGX_VA_BITS_REQUIRED` go. The usable check becomes:
a family either has `MULTIPLE_MEM_CONTEXTS` and two or more lists, or is single-context
with exactly one; and `va_bits` is 28 or 32 with a heap layout that fits.

The event-PDS registers and `EDM_TASK_REG0/1` (`SGX545_CR_*` in `pvrsgx_hw.h`, measured on
the SGX545, not in any GPL header) are **unknown** on the 530. Until the 530 microkernel
work names them, the family can't start a microkernel, and its part entry stays out of the
table. The probe then stops at "not one this driver knows", after reading the two ID
registers (porting/kernel.md: "A part that only gets as far as stage 3 stays out").

## 2. One page directory list: the VM model

On the SGX545 the EDM (the microkernel) translates through list 15, which only ever holds
the firmware's directory, and the TA and 3D through list 0, which holds the job's VM
directory. Client directories hold nothing of the kernel's (secure mode, UAPI.md P2/P3).
On the SGX530 the EDM, TA, 3D and 2D all translate through list 0. So:

1. **The firmware heaps are in every VM's directory.** The kernel code, kernel data and
   sync heaps are whole 4 MiB directory entries. A VM's directory points those entries at
   the firmware directory's own page tables, so the tables are shared, not copied, and a
   later kernel mapping shows in every VM at once. This is `share_kernel_heaps` from the
   out-of-tree DDK compatibility patch, moved into the driver and keyed on the family
   rather than on the firmware.
2. **When idle, list 0 holds the firmware directory.** The idle directory is the firmware
   directory, as `idle_kernel_pd=1` does today, because the EDM keeps running between
   jobs. `pvrsgx_reset()` ends with list 0 = firmware directory. The scratch directory
   that soaks up faults during recovery shares the firmware heaps too, so the EDM can run
   on it.
3. **The microkernel switches list 0.** At a job's start it loads list 0 with the job's
   directory, from the read-only directory table below, and invalidates the BIF directory
   cache. At the job's end, before it reports completion, it loads list 0 with the
   firmware directory again, from the same table. The 545's microkernel never
   has to restore list 0; the 530's must, or a freed directory stays live under a running
   EDM.
4. **A directory is freed only after its VM's last job has completed.** VM teardown
   already waits for every job of the VM. With point 3, that is enough.
5. **Per-VM structures are unchanged:** each VM has its own directory, its own 3D
   parameters heap and its PB at the heap base.

**What this does to isolation.** The firmware heaps are in every client's translation, so
P2 and P3 (UAPI.md 7.2) are open on the 530, as they are on the 545 under the DDK
microkernel. On the 545, P2 failed even for pages mapped `EDM_PROTECT`. On the 530 it is
untested, and it is the first MMU test (fact-sheet gap 6, extended): a client task loads
from an `EDM_PROTECT` kernel-data page. If the 530 enforces it, P2 closes for data. Until
then the design limits the damage:
- **The microkernel image is read-only** in every directory. The firmware arena already
  is: the EDM only fetches it, and E1 shows `READONLY` is enforced for stores and the PBE.
  The kernel objects in the code range (HW render and transfer contexts) are not, since
  the microkernel writes them (it clears word 0 bit 0 on first use).
- **Nothing the microkernel turns into a physical address is GPU-writable.** On the 545
  the directory address is HW context word 2 (`page_dir`), in a page the microkernel
  writes. On the 530 the directory addresses go to a host-written table that the GPU sees
  read-only: one entry per VM, and the firmware directory's. A HW context names its VM's
  entry, not an address. The microkernel's own state (CCB read offset, sync words,
  status, HW context flags) stays in the writable ranges. Without this a client could
  write a directory address and translate through any physical memory. This is an
  interface change for the 530 microkernel only, to agree with its session. The 545 keeps
  word 2.
- **What stays open:** a client can read every firmware structure (other clients'
  commands, not their memory), and can corrupt the microkernel's writable state: a hang
  (bounded by the job timeout) or a fence signalled early (the CCB read offset). The second
  breaks a "must not" of UAPI.md 7. **Decision for Joseph (D13):** accept this on the 530,
  with a `drm_warn` at probe and a threat-model note in UAPI.md. The alternative is to
  limit the render node to `CAP_SYS_ADMIN` on single-context cores. Either way, the 530 is
  not claimed to isolate clients upstream.

## 3. BRN_28889

`sdev->fw.cache_control` gathers invalidates for the next command: BIF PT and PD after
every kernel-object create, and DATA after every context release. Both happen together
routinely. With the quirk set, when a command would carry DATA together with PT or PD,
`pvrsgx_ccb_send()` first sends a `PROCESS_QUEUES` command carrying only the BIF bits. It
waits until the CCB read offset passes it (the same bounded 500 ms poll it uses for space,
under `ccb_lock`, with no allocation and no fence wait), and then sends the real command
with DATA only. Without the quirk (the 545) the path is the one-line send it is today. The
microkernel follows the same rule when it switches list 0 and a command also asks for a
data invalidate: the BIF invalidate first, wait, then the data invalidate.

## 4. Firmware loading and start

- **File:** `powervr_sgx/sgx530_ukernel.bin`, from the 530 microkernel build, per part as
  today (`info.fw_name`). The header checks are unchanged: magic, interface version, CRC,
  entries, patches, and the warning for another core revision.
- **Mode bit:** secure mode (`PVRSGX_UKFW_SECURE`: firmware heaps in no client directory)
  can't hold on the 530. A new header feature, `PVRSGX_UKFW_SINGLE_CONTEXT`, means the
  image runs its EDM on list 0, restores the firmware directory after every job (2.3),
  takes physical addresses only from read-only pages (2), and inits on `EVENT_KICK`. A
  single-context family requires it, and a multi-context family refuses it. The bit
  number is to agree with the 530 microkernel session.
- **Start:** the same sequence, with three per-family differences: no `CLKGATECTL2`
  write; the reset leaves list 0 = firmware directory; the init kick goes to `EVENT_KICK`.
  The host writes `INIT_STATUS` 0 before the kick, as today, and the microkernel tells
  the init kick from a CCB kick by its own state. The init script part 2 for the 530 has no
  vendor capture. It is whatever the open 530 microkernel needs, delivered as the family's
  `init_regs`, and empty until then.
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
| 3D params | `0x00400000` | 16 MiB | PB within 0x10000 pages of the base; `pb_max_pages` 0x1000 |
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

No change. One `drm_sched` with a credit limit of 1 (2 with `overlap`). The microkernel
overlaps a TA only with the 3D of the same context, so only one VM's directory is ever in
use, which is what a single list allows. Jobs from different VMs serialize in the
microkernel, each switch a directory load and a BIF invalidate. Timeout and recovery are
unchanged. Recovery's restart re-runs the reset of section 2, so list 0 comes back on the
firmware directory.

## 7. The uAPI

**No change** to any ioctl, struct or flag:
- `HEAP_INFO_GET` already reports every heap's base, size, page size, code-base index and
  window. A client that takes its layout from the query, as the uAPI requires, works on
  the 530 unchanged. Mesa must not keep heap addresses as constants (the comments in the
  winsys' `sgx_frame.h` enum list the 545's; that's for the Mesa session).
- `GPU_INFO_GET` returns the 530's `CORE_ID` and `CORE_REVISION`, which is how Mesa picks
  the core.
- `LIMITS_GET` values differ per family, which the query exists for.
- `QUIRKS_GET` stays empty. Neither erratum reaches userspace.

What changes is a promise, not the ABI: UAPI.md section 7 gets a single-context paragraph
(section 2 above) and D13.

## 8. Implementation steps

1. The table: the 530's `regs` (K facts), `num_dir_lists = 1`, `va_bits = 28`, the quirk,
   the heap layout; the new `regs` fields for the 545 at today's values; the usable check.
   KUnit: the 545 entry's values are unchanged, and the 530 entry is single-context with
   one list and 28 bits.
2. Writes skipped for absent registers (`bif_bank*`, `clkgatectl2`, `event_kick2`). The init
   kick on `EVENT_KICK` when `event_kick2` is 0.
3. Heaps through `sdev->family->heaps` in `pvrsgx_fw.c`, `pvrsgx_kobj.c`, `pvrsgx_vm.c`,
   `pvrsgx_render.c` and `pvrsgx_drv.c`.
4. Single-context MMU: shared firmware PDEs in every new VM directory and in the scratch
   directory; idle = firmware directory; the reset without banks; the read-only directory
   table, with a slot per VM, once its format is agreed.
5. `BRN_28889` in `pvrsgx_ccb_send()`.
6. `PVRSGX_UKFW_SINGLE_CONTEXT` in the loader.
7. Docs: DRM-DRIVER.md section 4, UAPI.md section 7 and D13, the fact sheet.

The 545's KUnit, W=1, sparse, checkpatch and kernel-doc checks stay green on x86. The
powervr-sgx-omap3630 workflow builds the result for ARM. On hardware, nothing past the ID
read runs until a 530 microkernel exists and its part entry is added.
