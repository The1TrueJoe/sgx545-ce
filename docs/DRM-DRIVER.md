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
| `drm_sched`, contexts, `SUBMIT_JOBS` with syncobjs (binary and timeline) and BO refs | written; NULL jobs only |
| `DEV_QUERY` | written |
| render and transfer streams | waiting on the uAPI conversion (D2) |
| firmware half of contexts (HW render/transfer context, ring, PB, RT data) | not started; STATE.md 6 |
| shader validator (D9) | hooks only; design in section 6 |
| runtime PM, devcoredump | not started |

Hardware results are in `docs/log/entries/` (search for `drm-`).

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
| `pvrsgx_validate.c` | D9 hooks |
| `pvrsgx_debugfs.c` | `status`: firmware state, CCB, host control, registers |
| `drm/tests/powervr_sgx_test.c` | uAPI tests, IGT style |
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
   clock / 1000, active power off, `POWMAN_NO_WORK`. Empty kernel CCB.
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

A VM's directory maps only what the client mapped inside its heaps, and (once render
contexts exist) the kernel-owned structures the TA and 3D read under it: PB, free list,
region headers, tail pointers. It never maps the kernel code or kernel data heaps. That is
the fix for P2 and P3 in `docs/UAPI.md` 7.2: a client task that loads from a firmware
address, or a PDS program that names code base 15, faults inside its own VM instead of
reaching the microkernel. The DDK put every kernel heap in every directory.

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
- A NULL job is a `PROCESS_QUEUES` command: no GPU work, ordered with the context's other
  jobs. It exists to test the fence, syncobj and scheduler plumbing before the streams.
- Timeout (2 s): the context is guilty, everything on the GPU fails (`-ETIME` for the
  guilty job, `-ECANCELED` otherwise), and the microkernel is stopped and started from a
  clean control block and an empty CCB. This is the "discard" recovery of UKERNEL.md 5a.
  If it does not start again, the device is lost and every later job fails with `-EIO`.

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

**The rules.**

1. Code objects. `CREATE_BO` gets a `CODE` flag. A code object has no CPU mapping and no
   PRIME export. Its contents arrive through a new upload ioctl that copies from user
   memory into the object and validates it in the kernel. An object can be uploaded once,
   or again only while no VM maps it.
2. `VM_MAP` into a USSE heap accepts only code objects, and a code object can be mapped
   only into a USSE heap (`pvrsgx_validate_vm_map`).
3. Validation scans every 8-byte-aligned word of the uploaded range, not just the paths a
   program's branches reach: a DOUTU can enter at any instruction. A word fails if its
   class is the special class with the "other" sub-class and the op is `ldr` or `str`.
   Anything the decoder does not recognise also fails, so a later ISA discovery cannot
   open a hole. The scan is linear and allocation-free.
4. The GPU must not be able to write a code object once it is validated. Client tasks
   (`st`), the PBE and the TA could all target a USSE heap address. Code pages are mapped
   with the PTE `READONLY` bit. **Whether the BIF enforces it is not established (G3)**;
   experiment E1 below decides it. If it does not, the fallback is to keep code objects
   out of every directory a client write can use, which depends on the requestor banks
   (G2): experiment E2.
5. `pvrsgx_validate_job` checks nothing about code: by rule 2 and section 4 a job can only
   run validated code. It stays as the place where stream fields that name programs
   (pixel-event PDS, background PDS) are checked against the PDS heaps once streams exist.

**Cost.** Mesa uploads a USSE program once per compiled shader, so the scan is off the
draw path. Per-draw PDS programs (in the PDS heaps) are not scanned. The winsys has to
upload code through the ioctl instead of writing a mapped buffer: a change for M3.

**Experiments before the validator ships.**

| | question | test |
|---|---|---|
| E1 | Does a PTE with `READONLY` stop a client task's `st`, and a PBE write? | Map a page read-only in a VM, `st` to it from a fragment task and render into it; read it back from the CPU |
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
