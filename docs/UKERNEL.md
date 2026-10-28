# The SGX545 USSE microkernel

The microkernel (Imagination's "EDM", event-driven microkernel) is the
program the SGX545 runs on its USSE pipelines to manage itself. The host
driver never programs the TA (tiling) or 3D (rasterising) cores directly for
work: it leaves a command in a ring buffer and raises a hardware event, and
the microkernel — running as an ordinary USSE task started by a PDS program —
reads the command, programs the cores through their memory-mapped registers,
waits on the completion events the cores raise, updates the sync objects the
host polls, and raises the host interrupt.

This file is what that program does, learned by disassembling the DDK's
microkernel (`srvinit/fetch-firmware.sh`; the 1.7.862890 build, 53096 bytes)
with `tools/isa/dis.py`, running it through the offline emulator
(`tools/ukernel/`, which replays a captured or generated frame against a
model of device memory and the core registers), and reading the GPL kernel
headers. It is a behavioural specification — state machines, data-structure
offsets, register sequences — written so an open microkernel can be designed
from it. It carries no disassembly listing, no decompiled code and no bytes
of Imagination's. Where the open microkernel in `ukernel/` does something,
this is the behaviour it reproduces; the DDK build's own entry offsets and
magic constants stay in `srvinit` (see `docs/SRVINIT.md`, "What still comes
from Imagination"), because a different microkernel chooses its own.

The companion files are `docs/SRVINIT.md` (how the microkernel is uploaded,
patched and started, and the control structures allocated for it),
`docs/PDS.md` (the PDS programs that launch USSE tasks, including the five
the microkernel is entered through), `docs/ISA.md` (the USSE instruction
set), `docs/STATE.md` (the TA/3D render state the microkernel loads into the
cores) and `docs/TEX.md` (transfers).

## 1. What runs the microkernel: PDS, events and tasks

The USSE does not fetch and run a program on its own. A **PDS program** (a
tiny separate processor, `docs/PDS.md`) issues a `DOUTU` that starts a USSE
**task**: a run of USSE instructions from a code address, with a few input
registers set up by `DOUTA`/`DOUTD` and a temporary-register budget. A task
runs to a program-end instruction (ISA flag f1.17) and stops; it is not a
thread that loops.

The SGX has an **event** mechanism (`EUR_CR_EVENT_*`). Each event source — a
host kick, a core finishing a stage, a timer, an out-of-memory condition —
has a status bit in `EUR_CR_EVENT_STATUS`/`STATUS2` and an enable bit in
`EUR_CR_EVENT_HOST_ENABLE`. When an enabled event is pending, the **EDM**
(the event data master) runs the **event PDS program** whose address the host
put in register 0xa68 at init (`srvinit`'s `pds0`). That program starts a
USSE task at a fixed microkernel entry. So the microkernel is not resident: it
is re-entered, as a fresh short-lived task, every time an event fires. State
that must survive between tasks lives in device memory (the control blocks
below) and in the **secondary attribute** registers (`sa0`..`sa63`), which the
hardware does not clear between tasks.

The host raises its own events through `EUR_CR_EVENT_KICK2` (the
multi-event-kick path this core uses): writing it makes the EDM run the event
program, which is how `SGXInitialise` and every `SGXScheduleCCBCommand` reach
the microkernel.

### The five entry PDS programs

`srvinit` builds five PDS programs and patches the microkernel's own
immediates to point at them (`docs/SRVINIT.md`, "PDS programs"). Each starts
a USSE task at a microkernel code offset, after handing it up to three input
values in `PA0`/`PA1`/`PA2`:

| program | started by | task does |
|---|---|---|
| pds0 | the host event kick (register 0xa68) | the init / kick entry: emits the events that drive the rest |
| pds1 | the microkernel, for CCB work | DMAs the 0xec-byte TA3D control block into `sa0`..`sa63`, then enters the CCB/kick handler |
| pds2 | the microkernel | re-enters without the DMA (state already in `sa`) |
| pds3 | the microkernel, for hardware events | the **event dispatcher**: `PA1` (the event word) selects which handler runs |
| pds4 | the microkernel, for deferred work | a second dispatcher keyed on `PA0` |

pds1 is the one that refreshes the secondary attributes from memory; the
others rely on `sa` already holding the control block. This is why the
microkernel keeps almost all of its live state in the TA3D control block and
reads it as `sa0`..`sa63`: `sa0` is that block's own base address, and the
named fields below are word offsets into it.

### The TA3D control block as secondary attributes

The 0xec-byte block at `hKernelSGXTA3DCtlMemInfo` (`docs/SRVINIT.md`) is DMAed
to `sa0`..`sa3b`. The microkernel reads and writes it constantly; the fields
it uses (word index = `saN`):

| word | meaning (behaviour) |
|---|---|
| 0x00 | the block's own device address (so handlers can address the rest by `sa0 + n`) |
| 0x01 | host control block address |
| 0x02 | kernel CCB control (`PVRSRV_SGX_CCB_CTL`: write/read offsets) address |
| 0x08 | a scheduling/flags word the power and context handlers set bits in |
| 0x09 | capability/mode flags the TA and render handlers test (e.g. bit 0, bit 2) |
| 0x0a | a status word the cleanup and misc handlers accumulate into |
| 0x13 | the HWPerf flags app-hint (0 unless `powervr.ini` sets it); gates the HWPerf paths |
| 0x16 | kernel CCB base address |
| 0x17 | the page directory's device-physical address |
| 0x18..0x37 | per-render-context scratch: the render-in-flight list, PB state, the TA/3D sync counters, saved register-bank values |
| 0x2b | HWPerf circular-buffer address |
| 0x2c, 0x2d | TA and 3D signature-register buffer addresses |

The host control block (word 0x01) is the `SGXMKIF_HOST_CTL` the kernel
polls: `ui32InitStatus`, `ui32PowerStatus`, `ui32CleanupStatus`,
`ui32InterruptFlags`/`ClearFlags`, lockup counters and the assert word
(`sgx_mkif_km.h`). The microkernel writes these; the host reads them in
`PollForValueKM` loops.

## 2. The event dispatcher (pds3)

The event program the EDM runs hands the dispatcher task a packed **event
word** in `PA1` (and copies of related values in `PA0`, `PA2`). The
dispatcher decides what happened and branches to a handler. It is the
microkernel's main loop in the sense that nearly every re-entry passes through
it, but it is still a task that runs once and ends.

The event word is a set of the `EUR_CR_EVENT_STATUS` bits that were pending,
masked to the ones the microkernel acts on. The dispatcher tests them in
priority order and, for most, writes the matching bit back to
`EUR_CR_EVENT_HOST_CLEAR`/`CLEAR2` to acknowledge it before handling it.
The groups, with the status bit each corresponds to:

| event | status bit | handler, in outline |
|---|---|---|
| host kick (a CCB command waiting) | SW_EVENT (0x4000) | read the kernel CCB, dispatch on the command (section 3) |
| TA finished | TA_FINISHED (0x2000) | advance the render's TA state, write the TA status/sync values, maybe start the 3D |
| TA terminate | TA_TERMINATE (0x1000) | end-of-scene bookkeeping on the TA side |
| 3D end of render / pixel BE end | PIXELBE_END_RENDER (0x40000), ISP_END_TILE (0x800000) | finish the render, update the 3D sync objects, raise the host interrupt |
| DPM 3D memory free | DPM_3D_MEM_FREE (0x1) and the other DPM_* bits | free-list maintenance, resume a render that was out of memory |
| DPM out of memory | DPM_OUT_OF_MEMORY_* | grow the parameter buffer or signal the host |
| timer | TIMER (0x20000000) | active-power-management sampling, lockup detection |

The two "other" dispatch values (pds3's `ev_a`/`ev_b`/`ev_c` entries, chosen
by the DMA masks `srvinit` puts in the PDS data) are the sub-handlers for the
end-of-render and memory events; pds4 dispatches the microkernel's own
deferred continuations the same way, keyed on `PA0`.

The microkernel signals the host interrupt by writing SW_EVENT (0x4000) into
`EUR_CR_EVENT_STATUS` itself (it sets the bit rather than clearing it); the
host's ISR sees the master-interrupt bit, reads `ui32InterruptFlags` from the
host control block, and runs its MISR.

## 3. The kernel CCB and the host-kick handlers

The host leaves commands in the **kernel CCB**, an array of 256
`SGXMKIF_COMMAND`s (`sgx_mkif_km.h`): each is `ui32ServiceAddress`,
`ui32CacheControl`, then six data words. `PVRSRV_SGX_CCB_CTL` (a separate
2-word buffer) holds the write offset (advanced by the host in
`SGXScheduleCCBCommand`) and the read offset (advanced by the microkernel).

On a host-kick event the microkernel:

1. reads the CCB control's write offset and compares it with its own read
   offset; if equal, there is nothing to do;
2. reads the command at the read offset;
3. **honours `ui32CacheControl`**: if it asks for a BIF cache invalidate
   (data / page-table / page-directory / SLC bits, `SGXMKIF_CC_INVAL_*`), the
   microkernel pulses the matching invalidate registers and waits for the
   done bits (section 6);
4. **jumps to `ui32ServiceAddress`**. This word is not a general address: the
   host fills it from `aui32HostKickAddr[eCmdType]`, the table of microkernel
   entry instruction-indexes `srvinit` passed in `SGX_BRIDGE_INIT_INFO`
   (`docs/SRVINIT.md`). So the command *type* is encoded as the entry the
   microkernel vectors to. The microkernel turns the index into a jump with
   its link-register mechanism (set the link to the computed instruction, then
   return to it);
5. the handler consumes the command's six data words;
6. advances the read offset (modulo 256) in the CCB control and writes it
   back;
7. loops to step 1 until the CCB is empty, then ends the task.

The command types (`SGXMKIF_CMD_TYPE` in `sgxinfo.h`) and what each handler
does:

### TA kick (type 0, "the kick")

The command's data[1] is the HW render context device address. The handler
drives a **tiling** pass:

- find the render context and its render-in-flight slot; the TA command
  itself sits in the client CCB at an offset the context names, and its
  layout is `docs/STATE.md` section 1;
- if this is the first kick of a new render (the TA command's flags say so),
  program the parameter-buffer registers from the PB descriptor the context
  points at (`EUR_CR_*` 0x600-group: free-list head and limits, thresholds;
  the values come from the PB descriptor words, `docs/STATE.md` section 6),
  set up the region-header and tail-pointer base registers (0x62c/0x634 etc.)
  from the RT-data entry, and run the DPM state/control **load** sequence
  (section 6);
- load the macrotile and screen layout from the TA command's tail
  (`EUR_CR_*` 0x204-0x248: `TE_AA`, `TE_MTILE*`, `TE_SCREEN`, tiles per
  macrotile, the VDM control-stream base, the region-header and
  tail-pointer bases, the two 1e-6 clamp floats, the screen size);
- point the BIF at this context's page directory (write the per-context
  directory-list base from the context's PD address; section 6);
- set the PDS and USE code bases for the context, then start the TA by
  kicking the VDM through its registers;
- record that a TA is in flight in the control block so the completion event
  knows which render to advance.

`run.py --why` on a captured frame shows each of these registers tagged with
the exact TA-command or render-details word it was loaded from; the mapping
is section 7.

### Transfer (type 1) and 2D (type 2)

A transfer command (`SGX_SUBMITTRANSFER`, texture uploads and the swap blit)
runs a **3D-only** render: no TA, no geometry, one background object covering
the destination whose pixel program samples the source (`docs/TEX.md`
section 3). data[1] is the HW transfer context; the command sits in that
context's own CCB. The handler loads the 3D registers from the transfer
command's words (the same 0x400/0x4xx/0x800/0xa5c ISP and PBE registers as a
render's 3D start, section 7) and kicks the 3D core. 2D is unused on this
build (its host-kick entry is 0).

### Power (type 3)

data[1] is `PVRSRV_POWERCMD_POWEROFF` / `IDLE` / `RESUME`. The handler clears
or sets the relevant enable bits, runs the deinit register sequence for a
power-off (zero the clock-gating and event-enable registers,
`docs/SRVINIT.md` "Deinit"), and reports completion by writing the matching
`PVRSRV_USSE_EDM_POWMAN_*` bit into the host control block's
`ui32PowerStatus`, which the host polls in `sgxpower.c`.

### Context suspend (type 4)

data[0] is the HW context address, data[1] `PVRSRV_CTXSUSPCMD_SUSPEND` /
`RESUME`. The handler sets or clears a suspend flag on the context and, on
resume, re-queues its pending work.

### Cleanup (type 5)

data[0] is the cleanup type (`PVRSRV_CLEANUPCMD_RT`/`RC`/`TC`/`2DC`/`PB` —
render target, render context, transfer context, 2D context, parameter
buffer) and data[1] the resource's device address. The handler removes the
resource from the microkernel's lists **only when it is safe** (no render of
that context in flight; the free list not mid-use), unlinking it from the
doubly-linked render-in-flight list kept in the control block and the
context's own structure. It reports completion by setting
`PVRSRV_USSE_EDM_CLEANUPCMD_COMPLETE` in the host control block's
`ui32CleanupStatus`, which `sgxutils.c` polls. If the resource is still busy
the handler leaves the status clear and the host times out and retries — this
is the "Wait for uKernel to clean up" message, and it is why a render-context
cleanup fails on a GPU the DDK left mid-render (`docs/NOTES.md`).

### Get misc info (type 6)

data[1] is the misc-info buffer. The handler writes the core identity and,
when the request word asks for it (`PVRSRV_USSE_MISCINFO_GET_STRUCT_SIZES`),
the microkernel's own `SGX_MISCINFO_STRUCT_SIZES` (the same sizes
`srvinit` passed in `SGX_BRIDGE_INIT_INFO`, so the kernel can check they
agree), then sets `PVRSRV_USSE_MISCINFO_READY`. The core identity comes from
reading `EUR_CR_CORE_ID` and `EUR_CR_CORE_REVISION` and from constants baked
into this build (the DDK version 862890, core 545, revision 1.0.14).

### Process queues (type 7)

A no-op kick whose only purpose is to make the microkernel re-scan its queues;
the handler falls straight through to the common tail.

### Set HWPerf status (type 9), flush PD cache (type 10)

HWPerf: enable or disable the performance-counter sampling and record the
counter group. Flush PD cache: pulse the BIF directory-cache invalidate for
the page directory the command names (used after the host changes a mapping).

All handlers end by jumping to the **common tail**: acknowledge, run any
deferred continuations the handler queued (through pds4), and end the task.

## 4. Init

`SGXInitialise` (`sgxinit.c`) runs the two init scripts `srvinit` built
(`docs/SRVINIT.md`, "Init scripts"), which program the clock gating, event
enables, USE/PDS code bases and the event-program address, then zeroes the
host control block's `ui32InitStatus` and raises the first event kick. The
EDM runs the event program, which starts the microkernel at its init entry
(pds0's target). The init task:

- reads the TA3D control block, host control block and CCB addresses out of
  `sa` (they were DMAed in);
- installs the event-program address and its control words into registers
  `EUR_CR_*` 0xa68/0xa6c;
- **tests the host control block's `ui32InterruptClearFlags` bit 0
  (`PVRSRV_USSE_EDM_INTERRUPT_HWR`)**. On a clean boot that bit is clear and
  the init task does nothing more than the next step. On a *recovery* re-init
  the host set it in `HWRecoveryResetSGX` (`sgxinit.c`), and the init task
  first runs the recovery rescan of section 5a before continuing;
- writes `PVRSRV_USSE_EDM_INIT_COMPLETE` (bit 0) into the host control block's
  `ui32InitStatus` and ends.

So a clean boot's init path is trivial — set one word and end — while a
recovery re-init does real work before it can set `ui32InitStatus`. The host's
`PollForValueKM` on `ui32InitStatus` then succeeds. The offline emulator
reproduces the clean path: after `Machine.init()` the host control block's
first word reads 1 (`tools/ukernel/run.py`, the `init` section). A timeout here
is only logged ("Wait for uKernel initialisation failed"), so a successful
`INITSRV_DISCONNECT` does not prove the microkernel ran — a client must render.

## 5a. Hardware recovery and re-initialisation

When a core locks up, the host's watchdog or MISR calls `HWRecoveryResetSGX`,
which pulses `EUR_CR_SOFT_RESET` and re-runs `SGXInitialise`. **A soft reset
resets only the cores; all of device memory survives** — the host control
block, the TA3D control block (including its render-in-flight list head), the
kernel CCB and its read/write offsets, and every registered render context
with its parameter-buffer free list. `HWRecoveryResetSGX` sets the HWR bit in
`ui32InterruptClearFlags` before re-initialising, so the init task takes the
recovery path above. That path, instead of starting clean:

- **re-scans the surviving kernel CCB** from the read offset to the write
  offset. It does not simply discard what is there; it walks each command and
  rebuilds the microkernel's internal per-context lookup from it;
- **walks the surviving render-in-flight list** (the control block's list
  head) and, for each render still registered, dereferences the render
  context and touches its structures (the context block, its sync and PB
  fields) to re-attach it.

Only after that does it set `ui32InitStatus`.

This is why recovery is fragile, and it matches what the replayer session saw
on hardware (`docs/log/entries/2026-10-28-2210-hwr-workqueues.md`): the first
recovery after a TA lockup succeeded, but a second lockup — after a client
that had locked up and whose `SGXCleanupRequest` had *failed*, leaving its
render context still registered — left `SGXInitialise` failing every time
(error 24, the `ui32InitStatus` poll timing out, 42 attempts). The likely
mechanism: the recovery init path walks straight into the dead client's
still-registered context, whose structures are inconsistent or partly torn
down, and the init task faults or stalls there before it can write
`ui32InitStatus`. The surviving kernel CCB (observed full, write offset 0x2f /
read offset 0x30) feeds the same stale work back in.

An open microkernel can avoid this. On a recovery re-init (the HWR bit set) it
can **discard** the surviving state rather than replay it: set the CCB read
offset equal to the write offset so no stale command is processed, and reset
the render-in-flight list head to empty, accepting that the render that was in
flight is lost — which it is in any case, because its core was reset
mid-render. The clients whose contexts are abandoned will fail their own
waits and tear down through the normal cleanup path once a working microkernel
is back. This is a behaviour choice the open microkernel makes; it is not a
reproduction of the DDK's, which rescans. It needs the same offline and
hardware validation as the rest (a recovery re-init with a non-empty CCB and a
stale render-in-flight entry, run through the emulator, then the wedge test on
hardware).

## 5. Completion, sync objects and the host interrupt

The cores raise events as they finish stages. The microkernel turns those
into the sync-object updates the host and the next client wait on.

- **TA finished.** The handler reads the TA command's status-value list
  (`sCtlTAStatusInfo`, filled by `SGXDoKickKM` in `sgxkick.c`) and writes each
  listed value to its named sync-heap address; the capture shows the first
  status word at `0x2d003040` becoming 0x5c after the first TA, which
  `docs/NOTES.md` already noted. It advances the render's TA-side counters in
  the control block. If the scene's last kick has been seen, it starts the 3D.
- **3D end of render.** The handler walks the render's **HW device sync
  list** (`SGXMKIF_HWDEVICE_SYNC_LIST`, built by `SGXDoKickKM`): for each
  destination sync object it sets `ui32WriteOpsComplete` to the pending value
  the client wrote plus one. This is what makes `eglSwapBuffers`' blit, and
  the next frame's dependency, observe the render as done. It then updates the
  render-in-flight list (removing the finished render) and raises the host
  interrupt (SW_EVENT), with `ui32InterruptFlags` telling the host it was a
  normal completion.
- **DPM 3D memory free.** After a render the parameter-buffer pages it used
  are returned to the free list; the handler runs the DPM free-list store and
  updates the PB descriptor's in-use count.

Because the microkernel owns the sync objects and the free list after the
first kick, a client that submits a render and then does not let it complete
(or whose sync counts are wrong) leaves the 3D waiting forever — the trap
`docs/TEX.md` and `docs/NOTES.md` describe.

## 6. Register sequences the microkernel runs

These are the fixed `EUR_CR_*` sequences the handlers perform, named by what
they accomplish. Offsets are from `sgx545defs.h` where it names them.

**BIF cache invalidate** (honouring a command's `ui32CacheControl`, and after
a page-directory change). Pulse the invalidate request — `EUR_CR_BIF_CTRL`
bit 3 (`INVALDC`) for a data-cache invalidate, or the directory/page-table
request registers `EUR_CR_PDS_INV*` / the 0xa08 USE-cache register — then poll
`EUR_CR_EVENT_STATUS`/`STATUS2` for the completion bits and acknowledge them.
The microkernel waits for a per-unit set of done bits in `EUR_CR_*` 0x138:
roughly 0x27 for the TA-side units and 0x4b for the 3D-side. The emulator
models these as the hardware's answers (`sgxsim.py`, `hw_model`): writing an
invalidate request makes the matching done bits appear, and the acknowledge
register clears them.

**Point the BIF at a context.** Write the context's page-directory address
(shifted) into the EDM directory-list base register and into `BIF_BANK0`'s
EDM index field, so the cores translate through that context's page tables
(mirrors `SGXResetSetupBIFContexts` in `sgxreset.c`).

**Parameter-buffer / DPM setup** (first kick of a render). Load the free-list
head and limit registers (`EUR_CR_*` 0x600, 0x618, 0x61c, 0x648, 0x620-0x668)
from the PB descriptor words, then run the DPM **state load** and **control
load**: write the region-header and tail-pointer bases, pulse
`EUR_CR_*` 0x694 (DPM state) and 0x698 (DPM control) with the load bit, and
wait for the DPM load events in `EUR_CR_EVENT_STATUS` (bits in the 0x20-0x200
range). An OTPM clear (`EUR_CR_*` 0x24c) with its two done bits follows.

**TA start.** After the layout registers (0x204-0x248) and the PDS/USE code
bases, kick the VDM so it begins fetching the control stream.

**3D start.** Load the ISP and PBE registers from the render-details block
(the 0x400/0x414/0x41c/0x42c/0x480-0x4c8/0x800/0xa5c-0xa64 group; the full
list with sources is section 7), set the ISP background depth and the
end-of-tile PDS program, then start the 3D. The microkernel also toggles
`EUR_CR_*` 0x480 bits per the render's sample/visibility flags.

**Deinit / power off.** Zero `EUR_CR_*` 0xa58, the event-program register
0xa68 and its control words, `USE_CODE_BASE_15`, the two event-host-enable
registers and the two clock-gating control registers (`docs/SRVINIT.md`,
"Deinit").

The emulator's register model answers every poll the handlers make
(invalidate-done, DPM-load, OTPM, TPC-flush, event-status clears), so a whole
frame's register programming runs offline without a GPU; that is how section 7
was produced and how an open microkernel is tested before hardware.

## 7. What each core register is loaded from

From `tools/ukernel/run.py --why` on the reference frame (`docs/STATE.md` has
the capture): the provenance of every `EUR_CR_*` the microkernel writes during
a TA start and a 3D start. "cmd+N" is the TA command tail (`docs/STATE.md`
section 1, offsets from the command base), "rd+N" the render-details block
(section 6.11), "pb+N" the PB descriptor, "rtentry+N" the RT-data entry.

**TA start** (first kick):

| register | from | meaning |
|---|---|---|
| 0x204 TE_AA | cmd+0x238 | multisample |
| 0x208 TE_MTILE1 | cmd+0x23c | macrotile X boundaries |
| 0x20c TE_MTILE2 | cmd+0x240 | macrotile Y boundaries |
| 0x210 TE_SCREEN | cmd+0x244 | last tile X/Y |
| 0x214 | cmd+0x248 | tiles per macrotile |
| 0x218 | cmd+0x24c, then DPM bits | TA flags 2 |
| 0x238 | cmd+0x250 | VDM control-stream base (plus a low-bits field) |
| 0x21c | cmd+0x258 | region-header base |
| 0x220 | cmd+0x25c | tail-pointer base |
| 0x240, 0x244 | cmd+0x260, +0x264 | the two 1e-6 clamp floats |
| 0x248 | cmd+0x268 | screen size `(h-1)<<16 \| (w-1)` |
| 0xc90 BIF_TA_REQ_BASE | cmd+0x274 | 3D-params heap base |
| 0x230 | cmd+0x2a4 | RT flag word |
| 0x600-0x668 (PB) | pb+0x10..+0x34 | free-list head, limits, thresholds |
| 0x62c, 0x634 | rtentry+0x14, +0x18 | region-header / tail arrays this render |
| 0x234 | rtentry+0x1c | |
| 0xc84 BIF dir-list | context PD | per-context page directory |

**3D start** (last kick of the scene, and transfers):

| register | from | meaning |
|---|---|---|
| 0xcac BIF_3D_REQ_BASE, 0xcb0 BIF_ZLS_REQ_BASE | rd+0x00, +0x04 | 3D and ZLS request bases |
| 0x400, 0x414, 0x42c, 0x41c, 0x420, 0x424 | rd+0x08..+0x1c | ISP control, depth-bias, flags |
| 0x408 | rd+0x20 | region headers of this render |
| 0x50c | rd+0x28 | region headers of the next render |
| 0x480-0x490 | rd+0x2c..+0x3c | ZLS control and depth/stencil addresses |
| 0x4b8 | rd+0x40 | ISP background depth (1.0) |
| 0x4bc | rd+0x44 | |
| 0x4c4 | rd+0x48 | background object block |
| 0x4c8 | rd+0x50 | RT flag |
| 0x800 | rd+0x58 | |
| 0xa5c, 0xa60, 0xa64 | rd+0x5c, +0x60, +0x64 | pixel-event (end-of-tile) PDS program, its data rows, a flag |
| 0x418 | rd+0x68 | |
| 0x4e8 | rd+0xcc | |
| 0xa74 | rd+0x13c (OR'd with a running value) | a mode word accumulated across kicks |

The microkernel reads these words from the client's render-details and
TA-command structures; the open microkernel must read the same offsets,
because those structures are written by the Mesa driver (`tools/gen`,
`tools/state`), not by the microkernel.

## 8. The offline emulator and how an open microkernel is validated

`tools/ukernel/emu.py` executes the microkernel's instruction subset (moves,
`limm`, bitwise, `test`, the integer multiply-adds, loads/stores to device
memory, `ldr`/`str` to the register file, branches/calls/`savl`/`setl`,
`emit`, program end) against a sparse device-memory model and a register
file; it stops with an error on any instruction it has no semantics for, so a
run that completes has executed real behaviour, not guesses.
`tools/ukernel/sgxsim.py` lays kernel data out as `srvinit` does, models the
five entry PDS programs as behaviour, answers the registers the microkernel
polls as a working core would, and plays the host side (init, CCB kicks,
events). `tools/ukernel/fromtrace.py` loads device memory, HW contexts and
kick parameters from a pvrtrace log — a DDK capture or a `tools/gen`
`frame-fake -t` run, both of which build on any machine.

`run.py FIRMWARE TRACE` runs a whole frame through a microkernel: init, each
TA kick, the TA-finished and 3D-end events, the transfer and its completion.
It prints every register access, every device-memory store and every task,
and with `--why` tags each with the structure word it came from. The DDK
microkernel runs the reference frame and a generated frame to completion this
way, leaving the host control block's init status set and the TA status and
3D sync words written where the capture has them.

The same harness is how `ukernel/` (the open microkernel) is checked before
any hardware run: it must take the same path — read the CCB command, program
the same registers from the same structure words, write the same sync and
status values, raise the host interrupt — for the reference and generated
frames, with the emulator never hitting an unknown instruction and the final
memory matching. Only then does it go to the box `srvinit` loads it on
(step 5 of the task), under the recovery method the main session chooses.

## 9. Open questions

- The exact meaning of the `op0xff`/`op0x100` special instructions the
  handlers run (ISA.md leaves them undecoded); they appear at points where the
  microkernel seems to signal the DMA/event hardware, and the emulator treats
  them as no-ops that log their operands. Whether an open microkernel needs
  them is not yet known.
- The `0x138` invalidate-done bit groups (0x27 TA, 0x4b 3D) are modelled as
  the hardware's answers from what the poll loops wait for; the per-unit
  decomposition is inferred, not confirmed on hardware.
- The precise contents of the per-context scratch area (control block words
  0x18-0x37) across multiple concurrent render contexts — the single-context
  single-render path (es2gears, one Minecraft frame) is covered; multiple
  contexts in flight are not yet exercised.
- The timer / active-power / lockup-detection path is read structurally but
  not run (no timer event in an offline frame).
- The recovery rescan (section 5a) is understood from the init path and the
  replayer's hardware incident, but exactly which dereference in the dead
  context's structures faults or stalls the second re-init is not pinned down.
  The open microkernel's "discard on recovery" approach sidesteps it rather
  than reproducing it, so confirming the DDK's exact failure is useful for the
  write-up but not a prerequisite for the replacement.
