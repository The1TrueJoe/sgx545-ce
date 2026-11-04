# powervr_sgx: the DRM render driver (M2 step 2)

`drm/powervr_sgx/` is the kernel driver `AGENTS.md` section 5 M2 asks for: a DRM render
driver for the SGX545 that boots the GPU from the kernel and runs submitted jobs, in place
of `srvinit` and `pvrsrvkm`. It is written in the upstream layout
(`drivers/gpu/drm/powervr_sgx/`) and builds in tree or out of tree. The uAPI it implements
is `docs/UAPI.md`; its header is `drm/powervr_sgx/include/uapi/drm/powervr_sgx_drm.h`.

It binds the CE5300's GPU (`01:02.0 8086:089b`, core rev 1014). Cedarview (M4) needs the
gma500 auxiliary device first.

## 1. Status

| piece | state |
|---|---|
| PCI probe, BAR0, INTx | written |
| microkernel load (`request_firmware`), patching, entry PDS programs | written; the DDK 1.7.862890 microkernel (D8: SGXMKIF) |
| init sequence (clocks, scripts, reset, BIF, first kick, `INIT_COMPLETE`) | written |
| firmware check (`GETMISCINFO`, struct sizes) | written |
| kernel CCB, SW_EVENT interrupt, completion | written |
| MMU: firmware directory, per-VM directories, idle directory | written |
| GEM shmem, `GET_BO_MMAP_OFFSET`, PRIME import | written |
| VM contexts on `drm_gpuvm`, `VM_MAP`/`VM_UNMAP` | written |
| `drm_sched`, contexts, `SUBMIT_JOBS` with syncobjs (binary and timeline) and BO refs | written; NULL, render and transfer jobs, mixed in one submit with in-submit waits (log `2026-11-03-0530-drm-mixed-submit-3d-dump`) |
| render contexts (HW render context, CCB, PB, free list), RT datasets, render jobs from `CMD_RENDER`/`CMD_TA_KICK` | written; **frames render** at 256x256 and 720x480, one-kick and two-kick scenes, any number of processes, and up to three render contexts in one VM sharing its PB (log `2026-11-03-0355-drm-shared-pb-restart`); the TA-flag 0x10 change was not shown to fix anything |
| `DEV_QUERY` | written |
| transfer jobs | written; **blits pixel-exact** (copy, upload and readback, box-filtered mip; 360 in a row, interleaved with another process's renders; log `2026-11-03-0405-drm-transfers`) |
| shader validator (D9) | written; HW-verified on the EA3: every frame, blit and hang test runs its shaders from validated read-only code objects, and the PBE can't write into one (log `2026-11-03-0730-drm-d9-validator`) |
| devcoredump on a hang | written; HW-verified (log `2026-11-03-0600-drm-rearm-bisect`) |
| runtime PM | on by default (`runtime_pm=0` turns it off); HW-verified on the EA3 and the EA1 (logs `2026-11-03-0630-drm-runtime-pm-ea3`, `2026-11-03-1700-drm-runtime-pm-ea1`); the first try froze the EA1 (log `2026-11-03-0610-drm-runtime-pm-wedge`) |

First frames (2026-11-03, `docs/log/entries/2026-11-03-0210-drm-first-frame.md`): a clear and a
triangle from client streams, pixels exact, 10 of 10 frames at 256x256, completion from the
render-done interrupt.

On the EA1 (2026-11-03, `docs/log/entries/2026-11-03-0100-drm-first-boot.md`): the driver
probes, starts the DDK microkernel from the kernel, passes the `GETMISCINFO` check, answers
DEV_QUERY, runs NULL jobs with fences, unloads and reloads cleanly, and passes all 24 uAPI
tests.

## 2. Layout

| file | does |
|---|---|
| `pvrsgx_drv.c` | `drm_driver`, PCI driver, ioctl table, `DEV_QUERY`, `obj_array` copy rules |
| `pvrsgx_device.c` | kernel-owned memory, reset and BIF set-up, interrupt |
| `pvrsgx_fw.c` | the microkernel: load, patch, PDS programs, control blocks, init scripts, start, stop |
| `pvrsgx_ccb.c` | kernel CCB writer and completion count |
| `pvrsgx_mmu.c` | page directories and tables |
| `pvrsgx_gem.c` | shmem objects |
| `pvrsgx_vm.c` | VM contexts, heaps, `VM_MAP`/`VM_UNMAP` |
| `pvrsgx_job.c` | contexts, scheduler, fences, `SUBMIT_JOBS` |
| `pvrsgx_render.c` | render and transfer contexts, PB, RT datasets, the TA and transfer commands |
| `pvrsgx_validate.c` | D9 hooks |
| `pvrsgx_dump.c` | device state as text: debugfs `status`, the devcoredump on a hang |
| `pvrsgx_debugfs.c` | `status`; `render` (the last render's structures); `render_before` (with `snapshot=1`: as its last kick was written) |
| `drm/tests/powervr_sgx_test.c` | uAPI tests, IGT style: every ioctl, every reserved bit, streams, VAs outside the VM |
| `drm/tests/powervr_sgx_frame.c`, `_blit.c`, `_hang.c` | frames, blits, mixed submits and hang recovery on the GPU |
| `drm/ci/` | the openHC EA kernel with this driver, for CI and RAM boots |

## 3. Bringing the GPU up

The sequence is `SGXInitialise` as the DDK's GPL kernel services run it, with the parts
`srvinit` used to supply now built in the kernel. Every buffer, constant and register
value is the one `docs/SRVINIT.md` lists.

1. Map BAR0. Read `CORE_ID` and `CORE_REVISION`, which are safe on a core in any state.
2. Load the microkernel with `request_firmware("powervr_sgx/sgx545-ukernel-1.7.862890.bin")`.
   It must be 0xcf68 bytes and carry all nine `limm` placeholders.
3. Allocate the kernel data (`0x32000000`) and kernel code (`0x30000000`) arenas, 14 pages
   each, below 4 GiB and write-combined, and map them in the firmware's directory. The
   buffers sit at the offsets SRVINIT.md's allocation table gives, so a trace compares
   with a DDK boot one for one.
4. Fill the TA3D control block, the signature lists, the five PDS programs and the
   microkernel's immediates.
5. Host control block: HWR check rate 10 (1 kHz timer / 100 Hz), timer clock = core
   clock / 1000, active power off, power status 0 (as `SGXPostPowerState` leaves it).
   Empty kernel CCB.
6. `CLKGATECTL` 0xa6aa, `CLKGATECTL2` 0xaaaaaa, script part 1.
7. Reset (section 4).
8. Event kicker 0, script part 2, `INIT_STATUS` 0, write barrier, `EUR_CR_EVENT_KICK2`.
9. Poll `INIT_STATUS` bit 0 for 500 ms.
10. `GETMISCINFO` with `GET_STRUCT_SIZES`: the microkernel's struct sizes must equal the
    ones its build was made with (SRVINIT.md, `SGX_BRIDGE_INIT_INFO`). The DDK version it
    reports is logged.

Stopping is the DDK's power-off: a `POWER` command with `POWEROFF`, a poll for
`POWEROFF_COMPLETE`, a wait for the clocks to gate, the deinit script, and the cores held in
reset (as openHC's `ce5300-sgx-quiesce` does at handoff).

## 4. Address translation

The SGX545 has 16 directory lists. `BIF_BANK0` assigns one to each requestor: the EDM (the
microkernel) gets list 15, and TA, 3D, host and 2D keep list 0.

| list | holds | when |
|---|---|---|
| 15 | the firmware's directory: kernel code and kernel data only | always |
| 0 | an empty directory | idle |
| 0 | a VM's directory | written by the microkernel from the HW render context when a job of that VM runs |

A VM's directory maps what the client mapped inside its heaps, and the kernel-owned
structures the TA and 3D read under it: PB, free list, scratch block, region headers, tail
pointers, background object block.

The 3D parameters heap is per VM, as the DDK's per-context heap: the PB, free list,
scratch block and background blocks of a VM's render contexts are allocated from the
heap base (0x10000000) in that VM's own address space and mapped only in its directory.
Every other kernel object has one device-wide address. The DPM numbers PB pages from the
heap base, so a PB anywhere else breaks: with device-wide 3D-params addresses, the
second process's PB sat at page 0x464 behind the first one's, rendered two frames and
hung on its third (log `2026-11-03-0340-drm-per-vm-pb`). So a VM has one PB: its first
render context creates it at the heap base (the size it asks for wins; later requests
are ignored, as the DDK does), its other render contexts share it, and the VM keeps it
until the client closes the VM. Two and three contexts in one VM, and a context replaced
every frame, render correctly (log `2026-11-03-0355-drm-shared-pb-restart`). The DDK goes
further and shares one PB across the whole device; per VM keeps clients' parameter data
apart.

**The kernel heaps: the design and what the DDK microkernel allows.** The design keeps the
kernel code, kernel data and sync heaps out of every client directory. That is the fix for
P2 and P3 in `docs/UAPI.md` 7.2: a client task that loads from a firmware address, or a PDS
program that names code base 15, faults inside its own VM instead of reaching the
microkernel. **The DDK microkernel does not allow it** (EA1, 2026-11-03, log
`2026-11-03-0135-drm-first-ta`): on a TA kick it writes `BIF_BANK0 = 0x11`, which moves the
EDM *and* the TA onto list 1, loads list 1 with the context's directory (HW render context
word 2), and then fetches its own code at `0x3000b000` through it. With the kernel heaps
absent from the client directory that fetch faults (`BIF_FAULT 0x3000b021`) and the TA
never runs. So with the DDK microkernel the driver points every VM's directory entries for
the three kernel heaps at the firmware directory's own page tables (`share_kernel_heaps`:
-1, the default, does this only when the DDK microkernel is loaded, and warns at load). The heaps are whole 4 MiB directory entries (`pvrsgx_kernel_heaps`), so the
page tables are shared, not copied, and every later kernel mapping shows in every VM. This
reopens P2 and P3 for as long as the DDK microkernel runs; the open microkernel, which
keeps the EDM on list 15, is what closes them. **The isolation the uAPI promises therefore
needs the open microkernel: M1 is a prerequisite for the secure configuration, and the DDK
microkernel is for bring-up and A/B testing only** (`docs/UAPI.md` D8).

Reset (`pvrsgx_reset`) follows the DDK's recovery path on every start: reset everything,
clear the lists, run the cores with list 0 on a scratch directory and give each pending
faulting request a scratch page until none are left, then list 15 = firmware, list 0 =
the empty directory. The module parameter `idle_kernel_pd=1` puts the firmware's
directory on list 0 instead, for debugging.

Entries are 32-bit physical addresses, so every table and every page the GPU sees comes
from below 4 GiB: tables and kernel memory with `__GFP_DMA32`, shmem objects through
their mapping's gfp mask, and a 32-bit DMA mask on the device.

Coherency is not established (UAPI.md G19). Tables and kernel memory are write-combined
in every CPU mapping (`set_pages_array_wc`), shmem objects are mapped write-combined, and
every kick is preceded by a write barrier.

`VM_UNMAP` clears the entries, asks the next kernel CCB command to invalidate the BIF's
directory and table caches (`ui32CacheControl`), and waits for every job submitted to the
VM so far before it lets go of the pages.

## 5. Jobs

- One `drm_sched` scheduler with a credit limit of 1: one job on the GPU at a time, as
  UAPI.md requires of the first driver. One entity per context, with the context's
  priority.
- A job is one kernel CCB command. The microkernel does not interrupt the host for every
  command, so a job is complete when the CCB read offset has passed its command. The
  check runs from the SW_EVENT interrupt thread and from a poll every jiffy while jobs are
  out. Render jobs will complete on the 3D's end-of-render instead (UKERNEL.md 5).
- A transfer job is one `TRANSFER` command (type 1) for the context's HW transfer
  context. The kernel writes the 0x1c0-byte transfer command into the context's ring
  from the `CMD_TRANSFER` stream (UAPI.md 3.5), with one destination sync of its own in
  `SGXMKIF_TRANSFERCMD_SHARED`. The job is done when the microkernel sets that sync's
  write-ops-complete to the pending value plus one, which raises SW_EVENT too. No source
  syncs: ordering against other jobs is the scheduler's.
- A NULL job is a `PROCESS_QUEUES` command: no GPU work, ordered with the context's other
  jobs. It exists to test the fence, syncobj and scheduler plumbing before the streams.
- Timeout (2 s): the context is guilty, everything on the GPU fails (`-ETIME` for the
  guilty job, `-ECANCELED` otherwise), and the microkernel is stopped and started from a
  clean control block and an empty CCB. This is the "discard" recovery of UKERNEL.md 5a.
  If it does not start again, the device is lost and every later job fails with `-EIO`.
  Before the restart the driver does what an unload does: deinit script, cores held in
  reset, bus mastering dropped (`restart_quiesce`). With the per-VM PB, **14 forced
  hangs in a row** (TA page faults from an unmapped vertex address, `SGXGEN_FAULT=1`,
  including back-to-back hangs) **each restarted in place**, and the next frames and the
  uAPI suite passed (log `2026-11-03-0355-drm-shared-pb-restart`). The restarts that failed
  earlier (log `2026-11-03-0300-drm-cleanup`) followed hangs from the misplaced PB; which
  of the quiesce and the PB fix made the difference was not separated.
- Those restarts only ever served new processes. A context that lives through a reset
  hung on its first render after it, and so did a new context in the same VM; the next
  reset then lost the device (log `2026-11-03-0530-drm-mixed-submit-3d-dump`). The cause
  is the VM's PB: the hung TA took pages off its free list and nothing gives them back.
  The PB now remembers the reset count it was set up for, and its first job after a
  reset sets its free list, block list and descriptor up as new. That is the only thing
  a restart has to redo. The HW render and transfer contexts (bit 0 of word 0, which
  the microkernel clears on first use) and the RT datasets carry on as they are: with
  each of them left out in turn the hang test still passes, and without the PB it fails
  (log `2026-11-03-0600-drm-rearm-bisect`). `powervr_sgx_hang` passes 5 forced hangs
  out of 5, with the guilty context refused and idle, new and transfer contexts and a
  mixed submit all right afterwards.
- Runtime PM (on unless `runtime_pm=0`): a job on the GPU holds a runtime PM reference from
  `run_job` until it leaves the active list, and the device autosuspends 500 ms after
  the last one (`power/autosuspend_delay_ms`). The SGX function has no PCI PM
  capability, so the PCI core leaves it in D0. Suspend is `pvrsgx_fw_stop` (the
  microkernel's power-off, the deinit script, the cores held in reset). Resume is the
  restart a reset does, minus its quiesce, because a register read of the gated core
  froze the SoC (log `2026-11-03-0630-drm-runtime-pm-ea3`). Every restart bumps
  `fw_gen`, so the PB is set up again after a resume as after a reset. While the core is
  stopped, nothing reads its registers: the shared-IRQ handler, debugfs `status` and the
  devcoredump check `powered`. A render from suspended takes 13-17 ms on the EA3 and the EA1.
- On a timeout the driver takes a devcoredump before the restart
  (`/sys/class/devcoredump/devcd<N>/data`, freed by writing to it). It holds the status
  and registers, the host control and TA3D control blocks, the kernel CCB, and the hung
  job's command and the structures it names: for a render, the PB, the RT dataset and
  its render details, the region headers and tail pointers. A forced TA page fault's
  dump is in `docs/log/data/2026-11-03-0600-drm-devcoredump.txt` (`BIF_FAULT` holds the
  unmapped vertex address).
- Teardown can send the microkernel's cleanup commands first (`cleanup`, UKERNEL.md 3).
  Off by default: on the DDK microkernel the RT and PB cleanups never complete, and the
  PB one stalls the kernel CCB.

## 6. The shader validator (D9)

**Why.** A client's USSE task can read the core's registers with `ldr` (P1, hardware), and
the page directory cannot stop that. The three register stores tried were dropped, but
others are untested and could reset the core or kick events, so stores are refused too.
The validator's job is that no client USSE instruction that reaches the hardware is an
`ldr` or `str` (ISA.md, special class hw 1f, "other", ops 0xd0/0xd1).

**What can run client USSE code.** A task starts at `USE_CODE_BASE[n] + offset`, where
`n` and the offset come from a DOUTU word. The kernel programs the code bases once:
0-9 cover exactly the USSE pixel heap, 10-13 exactly the USSE vertex heap, 15 the kernel
code heap; 14 is 0. Under a VM's directory, 14 and 15 point at unmapped addresses (section
4). So client USSE code can only ever run from the two USSE heaps of its own VM, whatever
the PDS program, VDM stream or state word says. This is what makes validation feasible:
**the validator only has to guarantee the contents of the USSE heaps.** PDS programs need
no instruction-level check for P1, because every DOUTU target is either in a USSE heap or
faults.

**The rules, as implemented** (`pvrsgx_validate.c`; uAPI in UAPI.md section 2 and D12).

1. Code objects. `CREATE_BO` takes `DRM_POWERVR_SGX_BO_CODE`. A code object has no CPU
   mapping (with `ALLOW_CPU_ACCESS` it is `-EINVAL`) and PRIME export refuses it. Its
   contents arrive through `UPLOAD_CODE`, which copies from user memory and validates
   in the kernel. While a VM maps the object, an upload may only write past the end of
   every earlier one (`-EBUSY` otherwise), so code the GPU may be running never
   changes; the new bytes were zeros, which decode as a harmless `fmad`. Once no VM
   maps it, anywhere again. Uploads and `VM_MAP` serialise on the object's reservation
   lock.
2. `VM_MAP` into a USSE heap accepts only code objects, and a code object maps only into
   a USSE heap (`pvrsgx_validate_vm_map`).
3. Validation scans every 8-byte word of the upload, not just the paths a program's
   branches reach: a DOUTU can enter at any instruction. The table is one (mask,
   match) pair per form, generated from the clean-room isaspec description
   (`mesa/src/sgx545/isa/sgx545.xml`) by `drm/tools/gen_usse_table.py`. That gives 109
   forms, and the generator refuses any two that could match one word. CI checks the
   committed table is current. A word fails if it matches no form (so a later ISA
   discovery can't open a hole), or if it is `ldr` or `str` (P1), `setl` or `savl` (link
   address from a register, so `lapc` could jump anywhere; the GLES compiler never emits
   them), or a `br` whose target, this instruction plus the signed 18-bit offset, is
   outside the object. `ba`, `phas.i` and `lapc` are absolute from the code base the task
   runs from and can only reach the USSE heaps or the unassigned gaps after them (UAPI.md
   section 5), which fault. The scan is linear and allocation-free; on a failure
   nothing is copied and `bad_offset` names the first bad instruction.
4. The GPU can't write a code object once it is validated: code objects are mapped with
   the PTE `READONLY` bit. **The BIF enforces it for a client task's `st` and for the
   PBE** (E1, both halves): each faults instead of landing, and the job is lost to a
   reset, which a client can cause anyway.
5. `pvrsgx_validate_job` checks nothing about code: by rule 2 and section 4 a job can only
   run validated code. PDS programs aren't scanned: they reach USSE code only through
   DOUTU's code base, 0-13 cover exactly the USSE heaps, and 14 and 15 point at nothing
   in a client VM with the open microkernel (under the DDK microkernel base 15 is the
   kernel code heap, P3).

**Cost.** Mesa uploads a USSE program once per compiled shader, so the scan is off the
draw path. Per-draw PDS programs (in the PDS heaps) are not scanned. The winsys has to
upload code through the ioctl instead of writing a mapped buffer: a change for M3.

**Experiments behind the validator.**

| | question | test |
|---|---|---|
| E1 | Does a PTE with `READONLY` stop a client task's `st`, and a PBE write? | **Yes, both, by a page fault.** `st` (EA1, 2026-11-03): a fragment task's `st` to a page the DDK maps `READONLY` raised a BIF fault at that page, did not land, and the stalled requestor was recovered by HWR (log `2026-11-03-0115-e1-pte-readonly`). PBE (EA3, 2026-11-03, `powervr_sgx_hang -E`): a frame whose end-of-tile program points the PBE at a read-only code object faulted at its first page (`BIF_FAULT` 0x37021002), the job was reset, and all 16384 words of the object read back unchanged (log `2026-11-03-0730-drm-d9-validator`) |
| E2 | Which requestor does a USSE instruction fetch use? | Point list 0 and the EDM's list at directories that differ only in the USSE heap, run a task, see which code ran |
| E3 | Do `ldr`/`str` have other encodings (another class, a DRC form) that reach the register file? | ISA.md coverage of the special class; a hardware check that a fuzzed word outside the known forms cannot read `CORE_ID` |

## 7. Testing

`drm/tests/powervr_sgx_test.c` has one subtest per uAPI behaviour, negatives included:
reserved fields, unknown flags and types, unaligned and empty ranges, every kernel range
and heap boundary for `VM_MAP`, handles from another file, short and long query sizes,
bad pointers, syncobjs that do not exist, a NULL job chain across contexts, 600 NULL jobs
(more than the kernel CCB holds), and a job on a context whose VM was destroyed. It
builds as a static binary and runs on an EA. The subtests map one to one onto an IGT
suite.

The test kernel is built in CI (`.github/workflows/powervr-sgx.yml`): kernel.org 7.1.8
with openHC's EA patches, drivers and config (`drm/ci/openhc-ea-kernel.sh`), the vendor
driver out and this one in as a module. It is booted from RAM through a one-shot
`S00aramboot` before anything has touched the GPU, and never flashed.

## 8. Gaps this driver depends on

| gap | blocks |
|---|---|
| G3 MMU permission bits, faults | the validator (E1), `VM_MAP` flags, fault reporting |
| G2 requestor banks | E2 |
| G15 who reads the TA-data and scratch blocks | which kernel structures go in a VM's directory |
| G19 coherency | dropping write-combine |
| G22 which job completed | completion of overlapping jobs |
| G23 power | runtime PM |
| the render and transfer stream format (D2) | `SUBMIT_JOBS` beyond NULL |
