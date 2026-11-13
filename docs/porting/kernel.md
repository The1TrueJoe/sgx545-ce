# Bringing up a PowerVR SGX core: the kernel side

This guide is how to bring `powervr_sgx` up on an SGX core or board it doesn't run on
yet. It follows the order the SGX545 came up in on the Intel Atom CE5300 and Cedarview.
Every stage says what to establish, how to show it, what done looks like, and the traps
we hit. The design is in [DRM-DRIVER.md](../DRM-DRIVER.md), the uAPI in
[UAPI.md](../UAPI.md), the microkernel in [UKERNEL.md](../UKERNEL.md), and the story with
its evidence in [docs/log](../log/BUILDLOG.md). This guide links to them rather than
repeating them.

Every method here is black-box: hardware tests, register reads, captures and the GPL
headers. Boot test kernels from RAM, and never flash a kernel that is still being
brought up: a wedged SGX can hang the bus, and a cold boot is the only way back.

## 0. What a core needs from you

A new core is a new `struct pvrsgx_core_family` in `drm/powervr_sgx/pvrsgx_core.c`. A new
part of a known core is only a new `struct pvrsgx_core_info`.

| Field | What it is | Where the value comes from |
|---|---|---|
| `family.core` | the core number the bus glue passes (545) | the SoC's documentation, the DT compatible |
| `family.core_id` | expected `CORE_ID` | read back on the hardware (stage 3) |
| `family.regs` | registers whose offset or fields differ between cores | the GPL DDK's `sgx5xxdefs.h` for that core, `CORE_ID` first |
| `family.features` | `PVRSGX_FEATURE_*` | `sgxfeaturedefs.h` for the core. Without `MULTIPLE_MEM_CONTEXTS` the core is single-context ([single-context.md](single-context.md)) |
| `family.isa` | the USSE instruction set, for the validator | USSE1 or USSE2 (543/544/554) |
| `family.num_dir_lists` | BIF directory lists | `SGX_FEATURE_BIF_NUM_DIRLISTS` (16 on 535/545, 8 on 543/544; 1 on the single-context 530) |
| `family.va_bits` | device-virtual address bits | `SGX_FEATURE_ADDRESS_SPACE_SIZE`: 28 or 32 |
| `family.heaps` | the device-virtual layout | within `va_bits`; the kernel heaps on 4 MiB boundaries for a single-context core |
| `family.quirks` | `PVRSGX_QUIRK_*` | `sgxerrata.h`'s `FIX_HW_BRN_*` that the kernel works around |
| `family.init_regs` | init script part 2 | [SRVINIT.md](../SRVINIT.md), "Init scripts"; captured from a working boot |
| `info.core_rev` | expected `CORE_REVISION` | read back on the hardware |
| `info.clock_hz` | core clock, for the microkernel's timer | the platform's clock tree; the order of magnitude is enough |
| `info.fw_name` | `powervr_sgx/sgx<core>_ukernel.bin` | the open microkernel built for the core ([UKERNEL.md 4a](../UKERNEL.md#4a-the-firmware-file)) |

The driver refuses a core whose family is missing or isn't usable
(`pvrsgx_core_family_usable()`: directory lists, address bits, a layout that fits, the
event PDS registers, the kick registers matching the features), and a single-context
core without `allow_unisolated=1`, before it writes a register. A listed family it can't
run still has its ID and revision read and logged. `KUnit powervr_sgx_core` checks
the table: run `tools/testing/kunit/kunit.py run
--kunitconfig=drivers/gpu/drm/powervr_sgx/tests/.kunitconfig`.

What is not per-core yet, so a new core may need code as well as a table entry: the
shader code-base windows (535 and 540 have no `USE_CODE_BASE` registers), the open
microkernel's command constants, and the validator's USSE2 tables. The heap layout, a
single-kick start (no `EVENT_KICK2`) and a single memory context are per-core now.

## Building the module for a board

Build against the board's own kernel: the same source, the same config (its
`/proc/config.gz`), **and the same compiler**. With `MODVERSIONS` each imported symbol
carries a genksyms CRC, and those move with compiler features as well as with the
config: gcc 15 sets `CC_HAS_COUNTED_BY` and other options that change the type
expansions genksyms hashes. Modules from Ubuntu's gcc 13 against our OMAP3630 test
board's exact config were refused ("disagrees about version of symbol
dev_driver_string") by its gcc 15.3 kernel. Read the board's `CONFIG_CC_VERSION_TEXT`
first, build with that compiler version, and check the `CONFIG_CC_HAS_*` lines against the
board's config before loading (`drm/ci/omap3630-kernel.sh`, `powervr-sgx-omap3630.yml`).
The DRM helper modules the driver needs (`drm_exec`, `drm_gpuvm`, `gpu-sched`,
`drm_shmem_helper`) come from the same build when the board's kernel doesn't have them.
A refused helper is harmless: load them first, and `powervr_sgx` itself only if they
all went in.

## 1. Publish the device

Establish: the driver binds, with the register window mapped, the interrupt routed, and
the bus telling it which core to expect. Even `CORE_ID` moves between cores (0x10 on 535,
0x1c on 545, 0x20 on 540/544), so the bus must name the core.

Three patterns:

- **The SGX is a PCI function of its own** (CE5300, `8086:089b`): an entry in
  `pvrsgx_pci_ids` with the core number as `driver_data`.
- **The SGX shares a PCI function with a display engine** (Cedarview, behind `gma500`):
  the display driver publishes an auxiliary device, `gma500_gfx.sgx`
  (`include/drm/gma500_sgx.h`), with the mapped register window, the core number and an
  interrupt hand-over hook. While a handler is set, the display driver writes no SGX
  register. Each chip says in its `psb_ops.sgx` whether it has one; see
  [drm/gma500/README.md](../../drm/gma500/README.md). Another display driver follows
  the same shape.
- **A device tree SoC** (OMAP, Allwinner, Ingenic): the upstream binding
  `Documentation/devicetree/bindings/gpu/img,powervr-sgx.yaml` already describes the
  node (`reg`, one interrupt, `core`/`mem`/`sys` clocks, a power domain), with
  compatibles `img,powervr-sgx530`, `-sgx540` and `-sgx544`. The platform driver is
  `pvrsgx_of.c` (`CONFIG_DRM_POWERVR_SGX_OF`); its `of_device_id` table carries the core
  number in `.data`. It uses the binding as it is upstream, with no additions: it matches
  the generic compatible (`img,powervr-sgx530`), so every SoC listed with it is covered,
  and the probe tells revisions apart by reading `CORE_REVISION`. An OMAP3430 (rev 121)
  is identified and refused until it has a part entry. It maps only the first `PVRSGX_REGS_MIN` bytes of `reg`, and it
  handles the TI layout only: no clocks on the GPU node, a ti-sysc parent that clocks
  and powers the module whenever the GPU device is runtime-resumed. A part with its own
  `clocks` or `resets` (Allwinner, Ingenic) is refused until the driver enables them. An
  SGX535 has no compatible in that binding yet; it needs an addition to the binding.

  What the binding says about the other parts, for the RFC (checked 2026-11-11 against
  mainline's `img,powervr-sgx.yaml`):
  - It requires only `compatible`, `reg` and `interrupts`, and forbids anything else
    (`additionalProperties: false`). There is no `resets` property for any part, so the
    driver's refusal of one can't trigger on a valid tree.
  - TI parts may not have `clocks` or `power-domains` on the GPU node, except the AM6548,
    which requires `power-domains`. The OMAP3630 case (a ti-sysc parent) is the one the
    driver handles. An AM6548 has a power domain and no ti-sysc parent, so today's probe
    would refuse it ("no runtime-PM parent"); it needs the power domain as its runtime-PM
    path.
  - Allwinner A31 (`core`, `mem`) and Ingenic JZ4780 (one clock) require `clocks` and
    `clock-names`, from `core`, `mem`, `sys`. Supporting them means enabling those clocks
    in the driver's runtime PM. The binding needs no change for that.

Done: `dmesg` shows the driver probing the device. A device whose bus names no known core
is refused with "not an SGX core this driver knows" before any register access.

## 2. Power and clocks

Establish: the core's power island and clocks are up before the first register read,
and stay up while the driver runs. On a PCI part, runtime PM goes through the parent; on
Cedarview, holding the auxiliary device awake keeps gma500, and so the graphics island,
awake.

Show it: a `CORE_ID` read that returns the same value boot after boot (stage 3).

Traps:
- The graphics island took longer than gma500's 50 us to come up: "GPU: power management
  timed out" at every boot (gma500 [0003](../../drm/gma500/0003-drm-gma500-Wait-longer-for-the-Cedarview-graphics-island.patch)).
- A register read on a core whose clocks are gated can hang the bus. Read only
  `CLKGATECTL`, `CLKGATESTATUS`, `SOFT_RESET`, `EVENT_STATUS`, `EVENT_HOST_ENABLE`,
  `BIF_CTRL`, `CORE_ID` and `CORE_REVISION` before the reset sequence has run
  (`pvrsgx_regs.h`).

## 3. Read the core ID and revision

Establish: `CORE_ID` and `CORE_REVISION` at the family's offsets, and a matching table
entry.

Show it: `dmesg | grep "SGX core id"`. An unknown revision logs both values and asks
for them to be reported. That message is the first evidence for a new part.

Done: the probe logs the part's name from the table.

## 4. MMU and BIF

Establish: the reset sequence, with every directory list cleared, list 0 on a scratch
directory that absorbs faults left over from before, and the microkernel's directory on
the last list (`pvrsgx_reset()`, [DRM-DRIVER.md 4](../DRM-DRIVER.md#4-address-translation)).
Page tables are 2-level, 4 KiB, with 32-bit entries on every SGX5xx without the 36-bit
MMU.

Show it: the `BIF_*` and directory-list lines in debugfs `status`
(`/sys/kernel/debug/dri/<minor>/status`) once the microkernel runs; a fault prints the
requestor, the type and how each directory translates the address.

Check the PTE flags early: a store to a `READONLY` page must fault, since the shader
validator relies on it ([log](../log/entries/2026-11-03-0115-e1-pte-readonly.md)).

## 5. Load and start the microkernel

Establish: the firmware file loads and passes its header checks, the driver fills its
data structures, runs the init script, kicks `EVENT_KICK2`, and `INIT_STATUS` bit 0
comes up within 500 ms ([UKERNEL.md 4](../UKERNEL.md#4-init),
[SRVINIT.md](../SRVINIT.md), `pvrsgx_fw_start()`). Only an image that declares secure mode loads.

Show it: `dmesg | grep "microkernel started"`; debugfs `ctlblk` for the host control
block. A failed start logs the init status, `EVENT_STATUS` and the BIF fault registers.

Traps:
- The init script is per-core and partly unnamed; capture it from a working boot and
  keep its order ([SRVINIT.md](../SRVINIT.md)).
- A microkernel that does nothing between two kicks looks hung to the host watchdog,
  whose reset wipes the TA's DPM state; `EVENT_TIMER` keeps it running
  ([log](../log/entries/2026-11-03-2030-ukernel-event-timer-fixes-frame1.md)).
- Per-scene setup runs once per scene, not once per TA kick, or the second kick hangs
  ([log](../log/entries/2026-11-03-0130-ukernel-first-kick-gate.md)).

## 6. The first kick and the first frame

Establish: a NULL job completes, then a TA kick, then a whole frame with the right
pixels.

Show it, in order:
```
powervr_sgx_test submit-null-job
powervr_sgx_frame -n 1 -v
powervr_sgx_frame -n 30 -k 1
powervr_sgx_blit -n 10
```
`powervr_sgx_frame` checks every pixel against what the frame should contain;
`powervr_sgx_blit` checks every destination byte.

Done: both report every frame or round right; `dmesg` has no "timed out".

Trap: the TA and 3D reach the hardware but the 3D hangs while state words are wrong; the
log from [the first TA](../log/entries/2026-11-03-0135-drm-first-ta.md) to
[the first frame](../log/entries/2026-11-03-0210-drm-first-frame.md) is the path.

## 7. Interrupts and job completion

Establish: the microkernel raises `SW_EVENT`, the driver acknowledges it, and a finished
job signals its fence in order. Behind a display driver the hard handler runs from the
display driver's; the bottom half is a work item.

Show it: the IRQ count in debugfs `status` rises with each job;
`powervr_sgx_frame -n 100 -q 4` (four jobs queued) completes every frame.

Trap: the microkernel also interrupts at TA and 3D ends; waking the thread for each of
those costs time. Wake only for a finished job or a lockup report.

## 8. Reset and recovery

Establish: a hung job times out (`job_timeout_ms`, 2 s), the GPU resets, the guilty
context is refused, others keep working, and a devcoredump is taken.

Show it:
```
powervr_sgx_hang -n 5
cat /sys/class/devcoredump/devcd*/data
```
Done: every round recovers, the reset count rises by one per hang, the dump has the
registers, the control blocks and the fault lookup.

Traps:
- Blame: with two jobs on the GPU, a reset must mark the hung context's own second job
  guilty too, not innocent (commit 57cfae8).
- A context that was idle during a reset must render again afterwards: the restart
  re-arms what the microkernel held for it ([log](../log/entries/2026-11-03-0550-drm-reset-rearm.md),
  [UKERNEL.md 5a](../UKERNEL.md#5a-hardware-recovery-and-re-initialization)).

## 9. Runtime PM

Establish: the core powers off after 500 ms idle (the microkernel's power-off, the clocks
gated, cores in reset) and comes back for the next job.

Show it: debugfs `status` says `powered: 0` after 500 ms idle; then
```
for i in $(seq 20); do powervr_sgx_frame -n 1; sleep 1; done
powervr_sgx_hang -n 3
rmmod powervr_sgx
```
with every frame right from suspended, the hang rounds back to suspended afterwards, and
`rmmod` clean while suspended ([log](../log/entries/2026-11-03-1700-drm-runtime-pm-ce5310.md)).

Traps:
- The first runtime-PM run froze the board: a register read on a suspended core
  ([log](../log/entries/2026-11-03-0610-drm-runtime-pm-wedge.md)). Read registers only
  while powered (debugfs, the shared IRQ, devcoredump).
- A resume after a clean stop must not run the hang quiesce: its register read froze the
  board on the first resume (commit 1d170e9).
- No runtime suspend while a render context exists (commit ce42391): a restart between
  a client's renders loses state the microkernel keeps.
- A device-tree core is runtime-active straight after its probe, so it autosuspends
  500 ms later. That suspend is the microkernel's power-off, sent through the CCB: stage 6
  work, long before stage 5 has passed. A single-context core (the SGX530) holds a
  runtime-PM reference from probe to remove (`pvrsgx_power_init`) until this stage is
  shown on it. The SGX545 paths are unchanged: PCI keeps runtime PM forbidden until
  probe ends, and gma500's SGX is multi-context.

## 10. Several clients

Establish: each client has its own VM and page directory, contexts in one VM share a
parameter buffer, and one client can't see another's memory.

Show it:
```
powervr_sgx_test
powervr_sgx_frame -c 3 -n 30
powervr_sgx_blit -2
powervr_sgx_prime -a
powervr_sgx_prime -b
```
`powervr_sgx_test` runs every uAPI subtest, negatives included ([DRM-DRIVER.md 7](../DRM-DRIVER.md#7-testing)).

Traps:
- One PB per VM: the DPM numbers PB pages from the heap base, so contexts share their
  VM's ([log](../log/entries/2026-11-03-0355-drm-shared-pb-restart.md)).
- A released context's addresses come back for the next client; invalidate the GPU's data
  caches with the next command, or it reads the last client's programs (commit 8da962c,
  `powervr_sgx_blit -2`).
- The SGX doesn't snoop CPU caches: flush the imports a job lists before it runs and
  after it is done ([log](../log/entries/2026-11-04-0100-drm-prime-ce5310.md)).
- Unbind must free what freeing releases: a freed object kept its VM, the VM its PB, and
  `drm_mm` wasn't clean at `rmmod` (commit 4ae6750, KUnit `powervr_sgx_kobj`).
- A VM's mappings must go when its handle and its last context are gone, once its jobs
  are done; waiting for a later render kept every exited client's buffers and ran the
  board out of memory (commit 0f4d7d3, uAPI subtest `vm-release-frees`).

## 11. The display side

The SGX renders; another driver scans out. Buffers cross as dma-bufs.

Traps:
- An exporter's pin and unpin run with the reservation lock held; gma500 took it again
  and X hung on its first import (gma500 0002).
- An importer may `dma_buf_mmap()` a dumb buffer that was never `MAP_DUMB`ed; the exporter
  must create the mmap offset at export (gma500 0002).
- A sysfb display (simpledrm, efidrm) copies damage in DIRTYFB with a blocking commit that
  waits for the render; X's main thread stalled behind deep render queues
  ([drm/sysfb/0001](../../drm/sysfb/0001-drm-sysfb-Copy-damage-outside-the-DIRTYFB-ioctl.patch)).

## What a new core's entry needs before it is accepted

Send these with the table entry, from the hardware itself:

1. `dmesg` with the `SGX core id ... revision ...` line and the part's name.
2. `microkernel started`, and nothing from the driver at warning level or worse.
3. `powervr_sgx_test` passing in full.
4. `powervr_sgx_frame -n 30` and `powervr_sgx_blit -n 10` all right.
5. `powervr_sgx_hang -n 5` recovering every round, with its devcoredumps.
6. The board and kernel version, and the `CONFIG_DRM_POWERVR_SGX*` options.

A part that only gets as far as stage 3 stays out of the table: report its ID and revision
in an issue. A core whose microkernel doesn't start yet stays out too.
