# The SGX545 USSE microkernel

The microkernel (Imagination's "EDM", event-driven microkernel) is the
program the SGX545 runs on its USSE pipelines to manage itself. The host
driver never programs the TA (tiling) or 3D (rasterizing) cores directly for
work: it leaves a command in a ring buffer and raises a hardware event, and
the microkernel — running as an ordinary USSE task started by a PDS program —
reads the command, programs the cores through their memory-mapped registers,
waits on the completion events the cores raise, updates the sync objects the
host polls, and raises the host interrupt.

This file is a behavioral specification of that program — its state machines,
data-structure offsets and register sequences — written so an open microkernel
can be designed from it and validated in the offline emulator (`tools/ukernel/`,
which replays a captured or generated frame against a model of device memory
and the core registers). Where the open microkernel in `ukernel/` does
something, this is the behavior it reproduces; the build-specific entry
offsets and magic constants of the reference firmware stay in `srvinit` (see
`docs/SRVINIT.md`), because a different microkernel chooses its own.

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

A task writes its own secondary attributes only once the launching PDS has set
up the `sa` context. The DDK's event program is a bare `DOUTU` (pds0) that
enters the microkernel; the microkernel then **emits** to trigger a second PDS
program (pds1) whose `DOUTD` DMAs the control block into `sa` and re-enters it,
and only there does the DDK microkernel write/read `sa` (e.g. `mov sa9`). In
the initial bare-`DOUTU` context, before that emit/DMA, writing `sa` faults
(hardware-confirmed 2026-11-02: an init entry that wrote `sa1` directly in the
bare context stalled). So a microkernel that skips the emit/DMA must not touch
`sa`; the open microkernel instead carries the control-block base in
a reserved temp from the loader-patched `limm` (`srvinit`'s `-o`) and derives
the host-control / CCB pointers from it.

The host raises its own events through the kick registers: writing one makes
the EDM run the event program. `SGXInitialise` writes `EUR_CR_EVENT_KICK2`
(0xae8; SGX545 has `SGX_FEATURE_MULTI_EVENT_KICK`), and every
`SGXScheduleCCBCommand` writes `EUR_CR_EVENT_KICK` (0xac8), section 3.

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

| word | meaning (behavior) |
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

Its words, for this build's configuration (`SUPPORT_HW_RECOVERY` on; no
EDM breakpoints, no `FIX_HW_BRN_28889`, no extended perf counters):

| word | field | who writes it |
|---|---|---|
| 0 | `ui32InitStatus`: INIT_COMPLETE = b0 | microkernel |
| 1 | `ui32PowerStatus`: IDLE_COMPLETE b2, POWEROFF_COMPLETE b3, POWEROFF_RESTART_IMMEDIATE b4, NO_WORK b5 | microkernel |
| 2 | `ui32CleanupStatus`: CLEANUPCMD_COMPLETE b0 (open microkernel: BUSY b1, DONE b2; section 3) | microkernel; the host clears it before a cleanup |
| 3 | `ui32uKernelDetectedLockups` | microkernel |
| 4 | `ui32HostDetectedLockups` | host |
| 5 | `ui32HWRecoverySampleRate`: timer ticks per lockup check, uKernelFreq / HWRecoveryFreq = 1000 / 100 = 10 | host |
| 6 | `ui32uKernelTimerClock`: core clock / uKernelFreq = 400 MHz / 1000 = 400000 (0x61a80); the microkernel loads it into `EUR_CR_EVENT_TIMER` | host |
| 7 | `ui32ActivePowManSampleRate`: idle ticks before an active-power request, uKernelFreq × 5 ms / 1000 + 1 = 6 | host |
| 8 | `ui32InterruptFlags`: HWR b0 (a lockup the microkernel detected), ACTIVE_POWER b1 (idle, the host may power off) | microkernel |
| 9 | `ui32InterruptClearFlags`: the same bits. The host acts on a flag only while its clear bit is 0, and sets the clear bit while it handles the request (`sgxinit.c` HWR, `sgxutils.c` active power) | host |
| 10 | `ui32BPSetClearSignal` | |
| 11 | `ui32NumActivePowerEvents` | microkernel |
| 12 | `ui32TimeWraps` | microkernel |
| 13 | `ui32HostClock`: the host's clock in µs, written at start | host |
| 14 | `ui32AssertFail` | microkernel |
| 15 | `ui32PerfGroup` (the HWPerf counter group). The open microkernel reuses the word as flags (powervr_sgx's `HOSTCTL_FLAGS`), b0 = `HOSTCTL_HOST_BGBLOCK`: when it is set, the host writes the background block at submit and `do_3d` skips its pulse (um-re f36ddd2) | host |
| 16 | `ui32BRN31939Mem` (present because the build is for core revision 1013, which carries `FIX_HW_BRN_31939`; the host never touches it). The open microkernel reuses the word for its cleanup v2 sequence echo (section 3) | microkernel |
| 17 | `ui32OpenCLDelayCount` | |

The timer values come from `sgxpower.c` with the CE5300 `sysconfig.c` constants:
`SYS_SGX_PDS_TIMER_FREQ` 1000 Hz, `SYS_SGX_HWRECOVERY_TIMEOUT_FREQ` 100 Hz and
`SYS_SGX_ACTIVE_POWER_LATENCY_MS` 5. The core clock is the one value that
comes from the platform (`sgx_core_clock`); the Cedarview `unified`
sysconfig and powervr_sgx both assume 400 MHz. The DDK microkernel's 1 ms
wake-ups in a CE5300 register sample are 1.2 M TSC cycles apart, which is
consistent with 400 MHz if the TSC runs at 1.2 GHz. Gap: the TSC rate is
unmeasured, so the real core clock is not yet confirmed.

## 2. The event dispatcher (pds3)

The event program the EDM runs hands the dispatcher task a packed **event
word** in `PA1` (and copies of related values in `PA0`, `PA2`). The
dispatcher decides what happened and branches to a handler. It is the
microkernel's main loop in the sense that nearly every re-entry passes through
it, but it is still a task that runs once and ends.

The event word is a set of the `EUR_CR_EVENT_STATUS`/`STATUS2` bits that were
pending, masked to the ones the microkernel acts on. The dispatcher tests them
in priority order and writes the matching bit back to
`EUR_CR_EVENT_HOST_CLEAR`/`CLEAR2` to acknowledge it before handling it.
Every bit that triggers the event program must be acknowledged, or it
re-triggers it forever:
- A pending **DCU-invalidate** in `EUR_CR_EVENT_STATUS2` (`dcu_invalid`,
  0x400) is left set by the kernel's `SysFinalise`. A microkernel that reads
  only `EUR_CR_EVENT_STATUS` storms on it, which starves the host kick and
  locks the core (hardware-confirmed 2026-11-02). The open microkernel
  acknowledges it up front.
- The **TA-finish** (`ta_finish`, 0x2000) and **3D-end** (`pbe_end_render`
  0x40000, `isp_end_tile` 0x800000) bits in `EUR_CR_EVENT_STATUS` are
  acknowledged up front in the open microkernel's dispatcher. Without that,
  `ta_finished` re-triggered forever and the next kick's host command was
  never processed (hardware-confirmed 2026-11-03: `EVENT_STATUS` stuck at
  `0x2000`).
- The DDK acks these in its handlers (`str 0x134 = 0x2000` / `0x40000`).

**The host kick is not an `EUR_CR_EVENT_STATUS` bit.** The host adds a command
to the kernel CCB, advances the CCB write offset, and rings the
**`EUR_CR_EVENT_KICK`** doorbell (`sgxutils.c SGXScheduleCCBCommand`); the
doorbell runs the event program but sets no status bit. So the microkernel
detects a waiting command by the kernel CCB **write offset ≠ read offset**, not
by a status bit — the open microkernel's dispatch falls through to the `kick` handler when no
hardware-event bit is set, and `kick` returns at once if the CCB is empty.
`SW_EVENT` (0x4000) in `EUR_CR_EVENT_STATUS` is the *opposite* direction: the
microkernel sets it (via `raise_host_irq`) to signal the host, and the host's
ISR clears it. Gating the host kick on `SW_EVENT` never fires: `GETMISCINFO`
sits unconsumed with `EVENT_STATUS = 0` (hardware-confirmed 2026-11-02). With
the CCB-offset test, `srvinit` returns rc=0, GETMISCINFO validates and the
kernel CCB reads `WO == RO`. The kernel CCB base is control-block word 0x16
(§4).
The groups, with the status bit each corresponds to:

| event | status bit | handler, in outline |
|---|---|---|
| host kick (a CCB command waiting) | none — `EUR_CR_EVENT_KICK` doorbell; detect via CCB write ≠ read offset | read the kernel CCB, dispatch on the command (section 3) |
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

The host rings the doorbell for each command:
1. It adds 1 to the 32-bit **event kicker** word, `(kicker + 1) & 0xff`.
   `EUR_CR_EVENT_KICKER` (0xac4, address in b4-31) points at that word, and
   the host zeroes it at start.
2. It writes 1 (`NOW`, b0) to `EUR_CR_EVENT_KICK` (0xac8)
   (`sgxutils.c SGXScheduleCCBCommand`).

`EUR_CR_EVENT_KICK2` (0xae8) takes its place for CCB commands only on core
revision 100 (`FIX_HW_BRN_26620`) with a system cache. The init kick in
`SGXInitialise` uses KICK2, because SGX545 has
`SGX_FEATURE_MULTI_EVENT_KICK`.

`ui32CacheControl` bits (`SGXMKIF_CC_*`): INVAL_BIF_PT 0x1, INVAL_BIF_PD 0x2,
INVAL_BIF_SL 0x4, INVAL_DATA 0x8. The host ORs in what it owes since its
last command (`sPDContext.ui32CacheControl`, cleared once sent). That makes
the first command after a page-directory change carry 0x2, which is
SRVINIT.md's `ui32CacheControl` 2.

On a host-kick event the microkernel:

1. reads the CCB control's write offset and compares it with its own read
   offset; if equal, there is nothing to do;
2. reads the command at the read offset;
3. **honors `ui32CacheControl`** (command word 1). This runs here, for every
   command, before the dispatch (pc 0x0098-0x00c2):
   - INVAL_BIF_PD (0x2): pulse `BIF_CTRL` = old | 8 (INVALDC), then the old
     value;
   - else INVAL_BIF_PT (0x1): pulse `BIF_CTRL` = old | 4 (FLUSH), then the old
     value. INVALDC already covers the page tables;
   - INVAL_DATA (0x8): 0x804 |= 0x10000000 (MADD), wait for `EVENT_STATUS`
     b26 (MADD_CACHE_INVALCOMPLETE), and ack it;
   - INVAL_SL (0x4) is not tested: this core has no system-level cache.
   The BIF pulses don't wait. The microkernel leaves the word as it is; the
   host clears its own copy once the command is sent;
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
context's own CCB (found through the same CCB/read-offset words as a render
context). The handler points the BIF at the context, invalidates the 3D-side
caches, programs twenty ISP/PBE registers from the transfer command's words
0..15 and kicks the 3D core. The open microkernel implements this
(`ukernel/` `k_transfer`), and `run_ours.py` confirms all twenty registers
equal the DDK's over a generated transfer frame (`tools/gen` `frame-fake`'s
`blitest`, which builds every transfer word from `sgxtex`). The command's
word → register map:

| reg | cmd word | reg | cmd word |
|---|---|---|---|
| 0x4c4 | 0 | 0xa5c | 9 |
| 0x4bc | 1 | 0xa60 | 10 |
| 0x4b8 | 2 | 0xa64 | 11 |
| 0x404 | 3 | 0x40c | 12 (first tile) |
| 0x408 | 4 (depth/stencil) | 0x410 | 13 (last tile) |
| 0x414 | 5 | 0x400 | 14 |
| 0xcac | 6 | 0xab8 | 15 |
| 0x42c | 7 | 0x148 | 16 |
| 0x4c8 | 8 | | |

with 0x480/0x63c = 0 and 0x43c = 1 as fixed constants. Word 12 (first tile) is
0 for a full-surface blit but not for a sub-rectangle, so it is command-sourced,
not a constant (confirmed against a capture with the textures session; word 4,
the depth/stencil block, and word 16 are 0 in every capture seen). The
transfer completes through the same 3D-end event as a render; the microkernel
tells the two apart by a transfer-in-flight marker (control block +0x54) and
routes a transfer to `xfer_done`. That handler writes the status value at
command word 0x16c to the address at word 0x168 (in `SGXMKIF_TRANSFERCMD_SHARED`
at +0xac, that is `sCtlStatusInfo[0]`; the count is at 0x164), clears the transfer context's in-flight fields (+0x14, +0xb0), advances
the context's CCB past the 0x1c0-byte command, and raises the host interrupt.
The transfer also updates two sync-heap objects named by the kick's
sync-handle list (not the shared command); reproducing those is the remaining
increment.

**The rest of the DDK's transfer start** (pc 0x1363-0x1424):
- **The BIF switch.** pc 0x17ec loads the transfer context's PD into its
  directory list. When that PD differs from the one loaded, it pulses
  `BIF_CTRL` INVALDC (pc 0x181e/0x181f). Without the pulse, the BIF's cached
  directory entries still belong to the previous client, and a transfer
  reads that client's memory through the same device addresses.
- **Three optional register groups.** Each is gated on a command word being
  nonzero:
  - word 98 (+0x188): words 17-27 go to 0x820-0x848;
  - word 99 (+0x18c): words 28-30 go to 0x860-0x868;
  - word 100 (+0x190): words 31-40 go to 0xa9c, 0xa84-0xa98 and 0xb24-0xb2c.
  These are the registers a render loads from render details +0x6c..+0xc8.
- **Cache invalidates, after the registers are programmed:** 0xad4 = 1,
  0xae0 = 1, 0x43c = 1, then 0x804 |= 0x10000000 with a wait for
  `EVENT_STATUS` b26, then 0xa08 = 1, then a wait for 0x138 & 0x4b.

**When the DDK starts a transfer.**
- A host kick only queues it.
- The 3D scheduler (pc 0x1035-0x1047) starts one only while the 3D is free,
  testing the sa9 3D-busy bits.
- It then walks the transfer queue (pc 0x12dc-0x1351) and skips a transfer
  whose sync objects still have ops pending.
- The only DPM register a transfer writes is 0x63c = 0 (pc 0x13c4): no DPM
  clear, store or load, and no free-list load.
- A transfer started under a running render's 3D would therefore disarm that
  render's page release, and its pages would never return to the free list.
  So a microkernel must hold a transfer while a 3D is in flight. 2D is unused on this build (its host-kick entry is 0).

### Power (type 3)

data[1] is `PVRSRV_POWERCMD_POWEROFF` (1) / `IDLE` (2) / `RESUME` (3). The handler clears
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

data[0] is the cleanup type (`PVRSRV_CLEANUPCMD_RT` 1 / `RC` 2 / `TC` 3 /
`2DC` 4 / `PB` 5 — render target, render context, transfer context, 2D
context, parameter buffer) and data[1] the resource's device address. The handler removes the
resource from the microkernel's lists **only when it is safe** (no render of
that context in flight; the free list not mid-use), unlinking it from the
doubly-linked render-in-flight list kept in the control block and the
context's own structure. It reports completion by setting
`PVRSRV_USSE_EDM_CLEANUPCMD_COMPLETE` in the host control block's
`ui32CleanupStatus`, which `sgxutils.c` polls. If the resource is still busy
the handler leaves the status clear and the host times out and retries — this
is the "Wait for uKernel to clean up" message, and it is why a render-context
cleanup fails on a GPU the DDK left mid-render (`docs/NOTES.md`).

#### What a kernel driver needs from it (2026-11-03)

`powervr_sgx` sent the three cleanups the DDK sends, in its order (RT dataset at
RT-dataset destroy, then render context, then PB descriptor at context
destroy), each after the context's last render had completed (render-done
SW_EVENT, fences signaled), on the DDK microkernel
(`docs/log/entries/2026-11-03-0300-drm-cleanup.md`):

| cleanup | data[1] | result |
|---|---|---|
| RT (1) | HW RT dataset | consumed; `CLEANUPCMD_COMPLETE` not set within 5 s |
| RC (2) | HW render context | no timeout, but possibly the RT's late completion: one status bit serves all three |
| PB (5) | PB descriptor | **not consumed**: the kernel CCB read offset stops at it, so every later command waits behind it until the host's job timeout resets the GPU |

After ten frames the PB descriptor reads 0x14 = 0x19, 0x18 = `04210019`, 0x20 =
0x19, 0x24 = 1 at 720x480, and a DDK frame leaves exactly the same words
(`docs/log/data/2026-11-03-0320-ddk-after-render.txt`). That accounting is
normal and does not explain the timeouts [G]. (The second-context hang it was
first blamed for was the PB's address: log `2026-11-03-0340-drm-per-vm-pb`.)

What the open microkernel must do (the list for `ukernel/`):

- once a context's last render has signaled render-done, treat its RT
  dataset, render context and PB as idle: cleanup of an idle resource
  completes at once;
- always consume a cleanup command; report busy and done as different status
  values instead of holding the CCB, so the host can retry or give up without
  a reset;
- keep a separate completion per command (or echo data[1] into the status), so
  a late completion can't be taken for the next one;
- leave nothing of a cleaned-up context where the next context's renders read
  it (the in-flight slot +0x50, the RT-data entry state).

#### Cleanup v2 (open microkernel, feature bit 0 of the firmware header)

Agreed between `powervr_sgx` and `ukernel/` (2026-11-04). The type values and
data[1] are as above. In addition:

- data[2] is a nonzero sequence number the host picks. The host sends one
  cleanup at a time, and only after every job of the resource's context has
  signaled. It clears `ui32CleanupStatus` before each send.
- The microkernel always consumes the command (advances the read offset), so
  later commands never wait behind a cleanup.
- It writes the outcome once, when it processes the command:
  `COMPLETE | DONE` (bits 0 and 2) when the resource is gone and nothing will
  read it again, or `COMPLETE | BUSY` (bits 0 and 1) while a render or page
  release that uses it is still in flight. Then it echoes data[2] into host
  control +0x40 (word 16).
- On `BUSY` the host sends the same command again after about 1 ms. After 1 s
  it gives up and keeps the memory until the next firmware restart. It never
  resets for a cleanup.
- The microkernel keeps a bound of its own as a backstop.

`powervr_sgx` shares one PB per VM between its render contexts, and sends the
PB cleanup (5) when the VM's PB goes, after its contexts' render-context
cleanups (2). A render-context cleanup must therefore not free the PB.

Without the feature bit the host speaks the contract above this heading: one
command, `COMPLETE` polled for 5 s, memory kept on a timeout.

### Get misc info (type 6)

data[1] is the misc-info buffer. The handler writes the core identity and,
when the request word asks for it (`PVRSRV_USSE_MISCINFO_GET_STRUCT_SIZES`),
the microkernel's own `SGX_MISCINFO_STRUCT_SIZES` (the same sizes
`srvinit` passed in `SGX_BRIDGE_INIT_INFO`, so the kernel can check they
agree), then sets `PVRSRV_USSE_MISCINFO_READY`. The buffer is
`PVRSRV_SGX_MISCINFO_INFO`:

| word | field |
|---|---|
| 0 | `ui32MiscInfoFlags`: the request (GET_STRUCT_SIZES 0x2) in, READY (0x1) out |
| 1 | `ui32CoreRev` |
| 2 | `ui32CoreID` |
| 3 | `ui32DDKVersion` |
| 4 | `ui32DDKBuild` |
| 5, 6 | `ui32CoreIdSW`, `ui32CoreRevSW` |
| 7 | `ui32BuildOptions` |
| 8-20 | the 13 `SGX_MISCINFO_STRUCT_SIZES` words, in `srvinit`'s order (no 2D-hardware sizes on this core) |

The core identity comes from
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
is only logged ("Wait for uKernel initialization failed"), so a successful
`INITSRV_DISCONNECT` does not prove the microkernel ran — a client must render.

**Hardware status (2026-11-02).** The open microkernel's clean-boot init path
runs on hardware, the host `GETMISCINFO` kick is serviced (kernel CCB
`WO == RO`), and the idle active-power signal keeps the host watchdog quiet
(0 lockups over 60 s idle). The rules these runs established are in §2 (the
DCU-invalidate and TA/3D acks; the host kick found by the CCB offsets) and §4
(`SA_KCCB` at control-block word 0x16). The bring-up history is in
`docs/log/entries/2026-11-08-ukernel-bringup-history.md`.

## 4a. The firmware file

`powervr_sgx` loads the microkernel with `request_firmware()`. It tries
`powervr_sgx/sgx_ukernel.bin` first, then the DDK image under its own
name. A file that starts with the magic below is an open microkernel and is
loaded from its header alone. Anything else must be the one DDK build the
driver knows (size and patch placeholders checked).

All words are little-endian u32. Offsets are from the start of the file.

| offset | field | meaning |
|---|---|---|
| 0x00 | magic | `SGX5` `UKRN` (0x35584753, 0x4e524b55) |
| 0x08 | header_size | bytes before the image; a multiple of 8 |
| 0x0c | interface | host interface version, major << 16 \| minor. 1.0: SGXMKIF as in §3 (the DDK 1.7 command set and host control block) |
| 0x10 | features | bit 0: cleanup v2 (§3, Cleanup). Bit 1: secure mode, the microkernel needs no kernel heap in a client page directory. Other bits must be 0 |
| 0x14 | image_size | bytes of USSE code after the header; a multiple of 8 |
| 0x18 | image_crc32 | CRC-32 (IEEE, as `crc32_le(~0, ...) ^ ~0`) of the image |
| 0x1c | n_entries | entries in the entry table |
| 0x20 | entries_off | offset of the entry table |
| 0x24 | n_patches | entries in the patch table |
| 0x28 | patches_off | offset of the patch table |
| 0x2c | core_rev | SGX core revision it was validated on (`SGX_CR_CORE_REVISION`), 0 for any |
| 0x30 | reserved | 4 words, 0 |
| 0x40 | build_id | 32 bytes, NUL-padded text (for example `git describe`) |

The entry table holds `{ u32 program, u32 offset }` pairs. `program` names the
PDS program the driver builds to enter the microkernel. 0 is the event
program, a bare `DOUTU` with bit 27 to `offset`. 1 to 4 are reserved for
programs with the DDK's roles (§1, "The five entry PDS programs"). The open
microkernel uses only 0. `offset` is in bytes from the start of the image.

The patch table holds `{ u32 offset, u32 value }` pairs. Each one names an
`limm` placeholder at `offset` in the image (the two-word form with
`0x000dbeef` in its low 21 bits). The driver writes into it the value
`value` names:

| value | written |
|---|---|
| 1 | device address of the TA3D control block |
| 2 | device address of the host control block |
| 3 | device address of the kernel CCB control (write and read offsets) |
| 4 | device address of the kernel CCB |
| 5 | device address of the misc-info buffer |

Host-kick table: identity. The `ServiceAddress` of a command is its type.

Host control word 15 (+0x3c) carries flags for the open microkernel only. The driver
never sets it for the DDK image.

| bit | meaning |
|---|---|
| 0 | the host has already written the background object's three PDS words (secondary, middle, primary, at the RT entry's background block +0x00/+0x04/+0x08) for every render; skip the 3D start's copy and its EDM-on-the-job's-directory window |

The driver refuses a file whose sizes or offsets run past its end, whose CRC
doesn't match, whose interface major it doesn't know, or that sets an unknown
feature bit or table value.

## 5a. Hardware recovery and re-initialization

When a core locks up, the host's watchdog or MISR calls `HWRecoveryResetSGX`,
which pulses `EUR_CR_SOFT_RESET` and re-runs `SGXInitialise`. **A soft reset
resets only the cores; all of device memory survives** — the host control
block, the TA3D control block (including its render-in-flight list head), the
kernel CCB and its read/write offsets, and every registered render context
with its parameter-buffer free list. `HWRecoveryResetSGX` sets the HWR bit in
`ui32InterruptClearFlags` before re-initializing, so the init task takes the
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
is back. This is a behavior choice the open microkernel makes; it is not a
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
  normal completion. The render details carry the destination sync objects at
  words 0x34 and 0x90 (a color and a depth destination for a one-context
  frame); their ops-pending word is cleared as the 3D drains.
- **DPM 3D memory free.** The hardware has already returned the render's
  parameter-buffer pages to the free list: the 3D start armed the release
  (0x63c = 3, section 6), and 0x724 counts down while the 3D runs. The DDK's
  handler writes **no DPM register and no PB-descriptor word**. In every
  trace its only register write is the ack (`EVENT_HOST_CLEAR` = 0x1, pc
  0x0e1e), then the rendezvous (pc 0x0eb4), then the completion below and
  SW_EVENT. It waits on nothing. The only PB-descriptor words the DDK writes
  during a frame:
  - +0x14 = the high half of 0x724 (the TA's allocated pages) at every TA
    finish (pc 0x0656-0x065c), a snapshot, not a running count;
  - +0x18..+0x24 = the live counters, by `save_ta_state` before each 3D
    start.

  Per scene the DDK also runs the DPM **clear** at the kick (pc
  0x0a1d-0x0a4a):
  - 0x694 = 0x698 = `2 | i<<3` for each RT data entry i up to entry word 10;
  - it waits for `EVENT_STATUS` & 0x240 (STATE_CLEAR b6, CONTROL_CLEAR b9)
    and acks 0x134 = 0x240;
  - a store/load follows only when `0x740 & 0x1ff` is nonzero, and 0x740
    reads 0 throughout a CE5300 register sample.

  The TA and 3D free-list **loads** run once per PB, not per frame (TA3D
  +0xb8/+0xbc markers; one each over the two-frame trace).

  The completion also **tears the render down**:
  the render-details state word (+0x2a4) moves to freed (2), its active flag
  (+0x12c) and write-ops word (+0x108) clear, the RT-data entry is released
  (its render back-pointer +0x4 → 0, state +0x34 → idle 0x10, and the RT-data
  completion word → 1), and the context's in-flight render words (+0x28, +0x2c,
  +0x3c) and the control block's in-flight slot (+0x50) empty. do_ta/do_3d set
  those markers at kick and 3D-start; the teardown reverses them.

The open microkernel (`ukernel/`) reproduces this exactly: `run_ours.py`'s
`[done]` check confirms every one of the DDK's completion memory writes
(sync-object clears plus the full slot free) over a frame, and `test_emu.py`'s
`render teardown` guards it with no trace. Because one render context runs at
a time and completes before the next kick, the **render-in-flight list is a
single slot** (control block +0x50), cleared rather than unlinked — but it
sits at the DDK's own field offsets, so the DDK microkernel stays a drop-in
A/B reference (`docs/UKERNEL-DECISIONS.md`) and the cleanup/power handlers read
the same slot to tell whether a render is running.

Because the microkernel owns the sync objects and the free list after the
first kick, a client that submits a render and then does not let it complete
(or whose sync counts are wrong) leaves the 3D waiting forever — the trap
`docs/TEX.md` and `docs/NOTES.md` describe.

### Where the DDK microkernel raises the host interrupt

Each site writes `EUR_CR_EVENT_STATUS` (0x12c) = SW_EVENT (0x4000)
(`ddk-uk.bin`). A normal completion sets no `ui32InterruptFlags` bit: only HWR
and ACTIVE_POWER use the flags.

| pc | path | runs on |
|---|---|---|
| 0x066b | TA finished, after the TA status values are written | TA_FINISHED (0x2000), **every** TA, including the kicks that don't end the scene |
| 0x086c | the kick path, after its status-value copy | a host kick |
| 0x0fce | render completion | the second of PIXELBE_END_RENDER (0x40000; handler 0x0ebb) and DPM_3D_MEM_FREE (0x1; handler 0x0eb4). The 3D start arms both halves in sa9 (b4, b5). With render details +0xd4 b3 it arms only the 3D end, and that completes alone |
| 0x14f7 | transfer completion (0x1461, taken at the 3D end when sa9 b2 marks a transfer) | PIXELBE_END_RENDER alone: a transfer doesn't wait for a DPM free |
| 0x0397, 0x04d4 | cleanup completion | the cleanup command, or its retry |

**No completion waits for the timer.** The only wait inside a completion
handler is the ISP clock poll in the 3D-end handler (pc 0x0ec3-0x0ec6):
- it runs when `CLKGATECTL & 3 == 2` (the ISP clock on demand);
- it spins on `CLKGATESTATUS` (0x008) b0 until it reads 0.

The DDK's timer path does only active-power sampling and the lockup sample.
It never completes work and never reads a CCB.

A CE5300 register sample of the DDK microkernel (2026-11-04) shows how long
completion takes. The measure is the time from the end of the hardware work
(`CLKGATESTATUS` back to idle) to the SW_EVENT edge:
- TA: 13 k TSC cycles (median), 42 k at most;
- 3D: 70 k TSC cycles (median), 77 k at most.

That is tens of µs at the EA's TSC rate, so any latency of milliseconds is
elsewhere: in the host's completion path or in the GPU work itself.

The open microkernel differs (`ukernel/`, um-re 1486d98):
- it raises no host interrupt at TA finish;
- a kick it defers while a TA runs is relaunched only from `ta_finished`;
- `render_complete` and `xfer_done` end without re-entering `kick`. So a
  command left in the CCB while the render ran (a deferred cleanup, say)
  waits for the next 1 kHz tick (`tmr_tick_check`, WO != RO), up to 1 ms. The
  timer can't be stopped meanwhile: `k_power` is itself a CCB command and
  runs only after what is queued ahead of it.

These cost throughput, not correctness: powervr_sgx's uAPI tests (39/39) and
multi-process renders pass without them.

### Out of memory: the DDK's parameter-buffer exhaustion path

The DDK never asks the host for more pages: no SW_EVENT and no host-control
write in this path, and the PB stays the size it was created with (requested
+ 0x22 reserve pages, `docs/STATE.md` §6). It handles exhaustion inside the
microkernel.

**Thresholds.** The TA load programs four thresholds from the PB descriptor:
- 0x620 = TA (+0x28, pages + 0x10);
- 0x624 = ZLS (+0x2c, pages − 0x22);
- 0x628 = global (`(+0x30) & 0xffff | 0x10000`, pages − 0x22);
- 0x668 = PDS (+0x34).

DPM_OUT_OF_MEMORY_* fires at a threshold, while 0x22 pages are still free.

**The handler.** The event word bits 0x100000 and 0x200000 (the pds3 word
for MT and GBL) reach pc 0x0ad1. It acks `EVENT_HOST_CLEAR` = 0x6 (MT and
GBL together) and records which one fired in a sa8 flag. It then runs 0x0b50
(the same entry from pds4 at 0x0af0). Run in the emulator with an injected
event (`tools/ukernel/run.py`'s `Runner.event`), it does this:

1. `rtdata` +0xc += 1 (an out-of-memory count), the RT data entry's state
   word (+0x34) |= 0x80, TA3D +0xdc = 0;
2. saves 0x628 in TA3D +0x84;
3. pauses the TA: `0xa74` |= 1 (pc 0x1765);
4. 0x628 = `(0x72c >> 16) | 0x10000`: the global threshold becomes the pages
   in use now;
5. reloads the TA free-list state from the live counters (pc 0x1676):
   0x61c = 0x700, 0x648 = 0x710 << 16, 0x638 from 0x724/0x72c,
   0x684 = 1, then waits for `EVENT_STATUS2` b0 and acks it;
6. resumes: `0xa74` &= ~1 (pc 0x1773), then 0x69c = 1.

Other branches depend on registers the emulator can't model (0x734 & 3 at
pc 0x0b90, and a sa8 flag from an earlier pass). They:
- raise 0x624 by 32 and 0x628 by 16 (pc 0x0bd6-0x0be9);
- run an OTPM clear (0x24c = 1, then wait for and ack 0x100000);
- write 0x69c = 4 and set `0xa74` b0 (pc 0x0bf6-0x0c22);
- schedule a 3D: `sa9` |= 0x40 | 0x4000, the "out-of-memory render" flags
  that the render completion clears at pc 0x157f, where it also writes
  0x69c = 1.

So the DDK renders the partly binned scene to free pages, then resumes the
TA. What 0x69c's values 1 and 4 command is G.

**The partial render** [D, code; not run on hardware]. The handler first
tests the TA's render context +0x28 (pc 0x0b46). The 3D start writes that
word and the DPM free clears it, so it is nonzero only while that context
has a render on the 3D:
- nonzero: the handler sets sa9 0x40 | 0x4000 and waits for that render's
  pages;
- 0 (scenes run one at a time): the steps below.

1. **Stop the TA.** Write 0x24c = 1 and wait for `EVENT_STATUS` 0x100000.
   Save 0x720 (its bits 5-13 are the TA's layer) and the counters in TA3D
   words 35-40. Then write 0x69c = 4 and pause the TA (0xa74 |= 1).
2. **TA_TERMINATE.** The hardware raises `EVENT_STATUS` b12. The dispatcher
   acks 0x1000 (pc 0x0add) and enters pc 0x0c23, the partial-render 3D start.
   It defers if the 3D is busy.
3. **The partial-render 3D start** (pc 0x0c30-0x0d75) runs on the open
   scene's render details and RT entry, with these differences:
   - the layer index is the TA's layer from step 1, not render details +0x108;
   - region headers (0x408) are offset by layer × RT entry word 11, and the
     ZLS addresses by layer × render details +0x118/+0x11c;
   - ZLS follows the 3D flags: b8 ORs 0x22000 into 0x480, b7 ORs 0x44000 |
     0x80000, and a layer already stored ORs 0x8000. With neither flag,
     0x480 &= 0xfff99ff9.
   - **The region-header redirect** (pc 0x0d7b-0x0d8d): **0x50c = 0x408**,
     so the "next render" region headers are this render's. The 3D resets
     the lists it renders, and the TA resumes into empty ones. Without this,
     the scene's final render walks the same lists again and the DPM frees
     their pages twice: the high half of 0x724 underflows (0xedf9 on a CE5300
     board, 2026-11-08). On the other path 0x408 is first offset by
     (0x720 & 0xf) × 0xc0 × RT entry word 2.
   - **The continuation's background** (pc 0x0d92-0x0d9b): the RT entry's
     background block (word 4) gets render details +0x12c and +0x130, then
     `[rd+0x114][layer]`. The final render then reloads the partial render's
     color instead of clearing it.
   - The DPM state is moved before the register program (pc 0x0c40-0x0c93),
     TA slot 0 and 3D slot 1:
     1. 0x630 = 2, 0x62c = RT entry word 5, 0x65c = 1;
     2. 0x694 = 1, wait for 0x10; 0x694 = 2, wait for 0x40;
     3. 0x630 = 3, 0x674 = RT entry word 5, 0x694 = 4, wait for 0x20;
     4. 0x630 = 2, 0x65c = 0.
     The 3D renders the copy in slot 1. An occupied other slot is evicted
     first with the full store (pc 0x1778). There is no TPC flush, and
     0x658 = 1.
4. **At its end** (pc 0x0df7): 0x69c = 1, and the layer word gets b1. The TA
   continues, and the scene's last kick renders it normally.

**On hardware** (DDK microkernel, Minecraft, regsample 2026-11-09, with no
recoveries). The `PBSize` apphint (1 MiB, and again 4 MiB) makes the client
allocate a per-context PB from the "Percontext 3DParameters" heap instead of
using the device's shared one. Its thresholds come out the same at both sizes
(TA 0x72, global 0x40). A size of 0x422 pages failed to allocate. The pattern
below does not depend on the PB's size:
- A partial render frees only about 20% of the pages: the TA pauses at the
  global threshold (0x40 there) and resumes at 51-54.
- The resumed TA allocates up to the threshold again within about 50 µs and
  raises another OOM.
- Partial renders alternate with a "stage 1" response (pc 0x0b97-0x0bac)
  that has no render:
  - pause the TA and let 0x700 settle;
  - save 0x628, then set **0x628 = (0x72c >> 16) | 0x10000**;
  - reload the TA free list from the live counters;
  - unpause, set the stage-1 flag, and write 0x69c = 1.
- The next OOM with that flag set undoes it (pc 0x0bad): it puts 0x628
  back and reloads again. Then it picks between a partial render and the
  rare +16 grant (pc 0x0bd3: 0x628 += 16, 0x624 += 32, a reload). If
  0x734 b0 is set, stage 1 is skipped.
- Over the run's 27,319 pauses:
  - 16,211 partial renders;
  - 7,643 stage-1 raises;
  - 729 +16 grants;
  - 1,963 pause-only.
- Partial render → stage 1 happens 7,189 times, stage 1 → partial render
  6,297.
- The TA free list is reloaded about once per partial render: 0x61c
  changes 16,274 times. That reload keeps the free list in step with the
  DPM after its state has been moved.
- During the render, 0x50c = 0x408, 0x63c = 3 and 0x630 = 2. 0x630 reads
  back. Other scenes use both slots meanwhile (0x630 = 4, 5, 7).

**Layer words.** RT entry word 12 points at one u32 per layer; RT entry
word 10 is the count. A scene's first kick clears them (pc 0x087e,
0x0a80). A render's completion sets b0 (pc 0x0f8f), and a partial render's
end sets b1 (pc 0x0dff). The normal 3D start adds the ZLS load bits when b1
is set (pc 0x1269-0x1288). The ZLS words store or load nothing without 3D
flags b7 | b8.

A parameter-buffer fault at `0x1ffff000` (the 3D-params heap base + page
0xffff) is the free list's end-of-list sentinel: the TA allocated past the
last page because nothing stopped it at the out-of-memory event.

Gaps:
- the hardware-side sequence for 0x734 and the second pass;
- where DPM_OUT_OF_MEMORY_ZLS (b25) and DPM_REACHED_MEM_THRESH (b3) are
  acked: both are enabled (`0xa58` = 0x2204b00f), but no ack of either was
  found outside a combined mask.

None of the DDK traces has an out-of-memory event; their scenes are small.

The thresholds are signals, not stops. Nothing in the DDK masks the page
index or programs a hard limit at the free list's end (0x648 holds the
initial `(last page - 1)<<16`, 0x61c the head). The microkernel's handler
pauses the TA (`0xa74` b0) while the 0x22 reserve pages last. Without a
handler the DPM keeps allocating, and it consumes the end-of-list value
0xffff as a page index.

**BIF faults.** The DDK microkernel never reads or writes `BIF_INT_STAT`
(0xc04) or `BIF_FAULT` (0xc08). It writes `BIF_CTRL` only to pulse an
invalidate: INVALDC (8) or FLUSH (4), then the old value back, at pc 0x158a
(for a command's cache control) and pc 0x181a (after loading a directory
list at 0xc34 + 4n). Fault recovery is the host's: `sgxreset.c` drains
faults by mapping each faulting page to a dummy page under a reset PD and
acking the event status. The PAUSE | CLEAR_FAULT pulse in `SGXReset` is
compiled only with `FIX_HW_BRN_23944`, which the SGX545 errata don't define.

## 6. Register sequences the microkernel runs

These are the fixed `EUR_CR_*` sequences the handlers perform, named by what
they accomplish. Offsets are from `sgx545defs.h` where it names them.

### SGX545 core revisions: 1.0.13 (Cedarview) and 1.0.14 (CE5300)

Both cores report `CORE_ID` 0x0115. `CORE_REVISION` reads 0x0001000d (1.0.13)
on Cedarview and 0x0001000e (1.0.14) on the CE5300. What differs, from the
GPL kernel headers and the DDK microkernel:

**Errata.** The kernel's errata table (`sgxerrata.h`) lists four SGX545
builds:

| `SGX_CORE_REV` | errata |
|---|---|
| 1013 | `FIX_HW_BRN_31939`, `FIX_HW_BRN_SAMPLE_CACHE` |
| 10131 | `FIX_HW_BRN_SAMPLE_CACHE` |
| 1014 | `FIX_HW_BRN_SAMPLE_CACHE` |
| 10141 | `FIX_HW_BRN_SAMPLE_CACHE` |

- `CORE_REVISION` can't tell 1013 from 10131: both read 1.0.13. Cedarview
  drivers were built as 10131.
- **No `SGX_FEATURE_*` is keyed on the revision** (`sgxfeaturedefs.h`), so
  the two cores have the same features, ISA and state words.

**`FIX_HW_BRN_SAMPLE_CACHE` applies to every revision listed.** It adds a
separate texture heap at a high address (`SGX_TEXTURE_HEAP_ID` 14,
`SGX_TEXTURE_HEAP_BASE_REV1`; the 0xf0000000 heap in this document's
captures). The heap id is part of the user/kernel ABI.

Intel's Cedarview kernel made the workaround a runtime choice by probing
the host bridge's revision, since some Cedarview parts don't need it. That
probe is not in this tree, so which Cedarview steppings are exempt is a gap.
Using the separate texture heap on all of them is always safe.

**`FIX_HW_BRN_31939` (build 1013 only) is a firmware TA watchdog.**
1. It adds host-control word 16 (`ui32BRN31939Mem`, §1).
2. The DDK microkernel, which is built with the workaround, sets that word
   to 500 at the kick of a TA whose TA flags have b12 (0x1000) set (pc
   0x0ac8). It clears it at that TA's finish (pc 0x06b3).
3. Every lockup sample (each `ui32HWRecoverySampleRate` ticks) it counts the
   word down (pc 0x04b0). At 0 it runs the lockup check (pc 0x02d3),
   about 5 s after the kick.
4. No client in any capture sets TA flags b12, so the watchdog never runs.

**The microkernel does not branch on the revision.** The DDK microkernel
reads `CORE_REVISION` and `CORE_ID` only to report them in GETMISCINFO
(words 1-2, pc 0x0174). It reports its own build constants in words 5-6:
`ui32CoreIdSW` 545 and `ui32CoreRevSW` 0x1000d. So it is a 1.0.13 build,
and the DDK kernel accepts it on a 1.0.14 core only through a
revision-exception pair (`sgxinit.c`, 0x1000e/0x1000d).

**What a driver and an open microkernel need for 1.0.13:**
- one core-table entry covering revisions 0x1000d and 0x1000e;
- the same heaps, the texture heap included;
- the same host-control layout;
- nothing revision-specific in the microkernel. An open microkernel that
  doesn't implement the BRN 31939 watchdog loses nothing while no client
  sets TA flags b12.

[D, from the kernel headers and the DDK microkernel's code.]

**First run on a Cedarview part (N2600, 1.0.13, host bridge 8086:0bf1 rev 04,
2026-11-08).**
- The open microkernel in secure mode boots and renders 10/10 frames.
- The first transfer job hangs: the 3D was triggered (`EVENT_STATUS2` 0x40)
  but never completed. `BIF_INT_STAT` shows only FLUSH_COMPLETE (b19), so
  the MMU did not fault.
- The DDK's transfer path (the 3D end with sa9 b2, pc 0x1461, completing at
  pc 0x14f7) has no revision branch. The only revision-gated microkernel
  code is the BRN 31939 TA watchdog. So the microkernel holds no per-revision
  transfer difference.
- **Lead: `FIX_HW_BRN_SAMPLE_CACHE`.**
  - That transfer was the first job on the part to sample a texture. The
    frame and uAPI tests sample none.
  - Its source lay in the general heap, not the 0xf0000000 texture heap.
  - Intel probed the host bridge revision because only some Cedarview
    steppings need the separate texture heap; which ones is the gap above.
- **The test:** the same blit with its source in the texture heap. That is
  safe on a CE5300, which samples from that heap anyway.

### The register map

The core registers the microkernel and a kernel driver use, from
`sgx545defs.h` (the GPL kernel's SGX545 header), with what a CE5300 shows.

| reg | name | fields |
|---|---|---|
| 0x000 | `CLKGATECTL` | 2-bit modes per unit: ISP b0-1, ISP2 b2-3, TSP b4-5, TE b6-7, MTE b8-9, DPM b10-11, VDM b12-13, PDS0 b14-15; AUTO_MAN_REG b24. Mode 2 is clock on demand (the 3D-end ISP poll tests `& 3 == 2`) |
| 0x004 | `CLKGATECTL2` | the same for PBE b0-1, CACHEL2, UCACHEL2, USE0, ITR0, TEX0, MADD0, USE1, ITR1, TEX1, MADD1, PDS1 b22-23 |
| 0x008 | `CLKGATESTATUS` | 1 per running clock: ISP b0, ISP2 b1, TSP b2, TE b3, MTE b4, DPM b5, VDM b6, PDS0 b7, PBE b8, then the CLKGATECTL2 units from b9. An idle CE5300 reads 0x20 (only the DPM's), so "all gated" is `& ~0x20 == 0` |
| 0x01c | `CORE_ID` | CONFIG b0-15, ID b16-31 |
| 0x020 | `CORE_REVISION` | MAINTENANCE b0-7, MINOR b8-15, MAJOR b16-23, DESIGNER b24-31 (the CE5300's 1.0.14 reads 0x1000e) |
| 0x080 | `SOFT_RESET` | BIF b0, DPM b1, TA b2, USE b3, ISP b4, TSP b5; 1 holds the unit in reset, 0 releases |
| 0x110 | `EVENT_HOST_ENABLE2` | enables for `EVENT_STATUS2` |
| 0x114 | `EVENT_HOST_CLEAR2` | write 1s to clear `EVENT_STATUS2` bits |
| 0x118 | `EVENT_STATUS2` | DPM_TA_FREE_LOAD b0, DPM_3D_FREE_LOAD b1, DPM_HOST_FREE_LOAD b2, DPM_DHOST_FREE_LOAD b3, TRIG_DL b5, TRIG_3D b6, TRIG_TA b7, GSG_LOADED b8, GSG_FLUSHED b9, DCU_INVALCOMPLETE b10, OTPM_FLUSHED_INV b11, OTPM_MEM_CLEARED b12, VDM_TASK_KICKED b13, VDM_CONTEXT_LOAD b14, MTE_STATE_FLUSHED b15 |
| 0x12c | `EVENT_STATUS` | DPM_3D_MEM_FREE b0, DPM_OUT_OF_MEMORY_MT b1, _GBL b2, DPM_REACHED_MEM_THRESH b3, DPM_STATE_STORE b4, _LOAD b5, _CLEAR b6, DPM_CONTROL_STORE b7, _LOAD b8, _CLEAR b9, TPC_FLUSH b10, TPC_CLEAR b11, TA_TERMINATE b12, TA_FINISHED b13, SW_EVENT b14, BREAKPOINT b15, ISP_VISIBILITY_FAIL b16, PIXELBE_END_RENDER b18, OTPM_FLUSHED b19, OTPM_INV b20, ISP2_ZLS_CSW_FINISHED b21, DPM_INITEND b22, ISP_END_TILE b23, DPM_TA_MEM_FREE b24, DPM_OUT_OF_MEMORY_ZLS b25, MADD_CACHE_INVALCOMPLETE b26, TA_DPM_FAULT b28, TIMER b29, MASTER_INTERRUPT b31 |
| 0x130 | `EVENT_HOST_ENABLE` | enables for `EVENT_STATUS` |
| 0x134 | `EVENT_HOST_CLEAR` | write 1s to clear `EVENT_STATUS` bits, including MASTER_INTERRUPT b31 and SW_EVENT b14 (what the host's ISR writes) |
| 0x144 | `TIMER` | a free-running 32-bit counter |
| 0xac4 | `EVENT_KICKER` | address of the event-kicker word, b4-31 (section 3) |
| 0xac8 | `EVENT_KICK` | write NOW (b0) = 1: a CCB command is waiting |
| 0xacc | `EVENT_TIMER` | VALUE b0-23 (the period, host control word 6), ENABLE b24 |
| 0xae8 | `EVENT_KICK2` | write NOW (b0) = 1: the init kick |
| 0xc00 | `BIF_CTRL` | NOREORDER b0, PAUSE b1, FLUSH b2, INVALDC b3 (pulse 1 then 0), CLEAR_FAULT b4, MMU_BYPASS_* b8-15 (CACHE, VDM, TE, PBE, TSPP, ISP, USE, HOST) |
| 0xc04 | `BIF_INT_STAT` | FAULT_REQ b0-15 (the requestors that faulted), FAULT_TYPE b16-18, FLUSH_COMPLETE b19 |
| 0xc08 | `BIF_FAULT` | CID b0-3 (the requestor's context: faults by the EDM on list 15 read 0xf, 2026-11-03), SB b4-8, ADDR b12-31 (the faulting page) |
| 0xc38 + 4(n - 1) | `BIF_DIR_LIST_BASE1`..`15` | the page directory's physical address for list n (list 15 = 0xc70). ADDR b8-31; SGX545 has no 36-bit MMU, so the value is the page-aligned physical address itself, unshifted (`SGX_MMU_PDE_ADDR_ALIGNSHIFT` 0, as `sgxreset.c` writes it) |
| 0xc74 | `BIF_BANK_SET` | SELECT_2D b0, SELECT_3D b2-3, SELECT_HOST b4, SELECT_TA b6-7, SELECT_EDM b8, SELECT_DPM_LSS b9; the DDK's reset writes 0 |
| 0xc78 | `BIF_BANK0` | directory-list index per requestor: EDM b0-3, TA b4-7, HOST b8-11, 3D b12-15, 2D b16-19 |
| 0xc84 | `BIF_DIR_LIST_BASE0` | list 0, same format |
| 0xca8 | `BIF_MEM_REQ_STAT` | READS b0-10: memory reads outstanding |

The DDK's reset (`sgxreset.c`) zeroes `BIF_CTRL`, `BIF_BANK_SET`, `BIF_BANK0` and all 16
list bases, then sets `BIF_BANK0` EDM = `SGX_BIF_DIR_LIST_INDEX_EDM` (15 =
`SGX_FEATURE_BIF_NUM_DIRLISTS` - 1) and writes the kernel PD into list 15.
Gap: which registers are safe to read on a core that hasn't been reset is
in the kernel driver's notes (DRM-DRIVER.md), not here.

**BIF cache invalidate** (honoring a command's `ui32CacheControl`, and after
a page-directory change). Pulse the invalidate request — `EUR_CR_BIF_CTRL`
bit 3 (`INVALDC`) for a data-cache invalidate, or the directory/page-table
request registers `EUR_CR_PDS_INV*` / the 0xa08 USE-cache register — then poll
`EUR_CR_EVENT_STATUS`/`STATUS2` for the completion bits and acknowledge them.
The microkernel waits for a per-unit set of done bits in `EUR_CR_*` 0x138:
roughly 0x27 for the TA-side units and 0x4b for the 3D-side. The emulator
models these as the hardware's answers (`sgxsim.py`, `hw_model`): writing an
invalidate request makes the matching done bits appear, and the acknowledge
register clears them.

**Point the BIF at a context — and keep the EDM on its own directory list.**
Load the context's page-directory address into **directory-list 0**
(`EUR_CR_BIF_DIR_LIST_BASE0 = 0xc84`) and route the **TA/3D** requestors to list
0 through `EUR_CR_BIF_BANK0 = 0xc78 = 0x0f` (b0-3 = EDM → list **15**, b4-7 = TA →
list 0), so the cores that run the client's geometry/pixels translate through the
client's page tables.

**Hard design rule: the EDM (microkernel) stays on directory-list 15, with the
firmware/kernel page directory, and is NEVER pointed at a client PD.** Only the
TA and 3D requestors are moved onto the client's list. This is what lets the EDM
fetch its own code and the kernel heaps (CCB, control block, kernel-data) through
the kernel PD while the *client* PD contains only the client's own mappings — so
the open DRM driver does not have to map kernel memory into every client address
space (UAPI P2/P3). The DDK does the opposite: its TA kick writes `BIF_BANK0 =
0x11` (EDM **and** TA both on list 1), loads list 1 with the context's PD, and
then fetches the microkernel's own code through that client PD — which forces
every client PD to carry the kernel heaps. The open microkernel does not copy that; keeping the
EDM isolated on list 15 is a deliberate divergence, verified on hardware (the TA
reads its command through list 0 with `BIF_FAULT = 0`). Do not share the EDM's
dir-list with a client requestor.

**How the DDK reaches client memory from the EDM.** It never uses a second
bank or a separate data requestor. A subroutine (pc 0x1864) copies the TA's
nibble (r16 = 0) or the 3D's (r16 = 1) of `BIF_BANK0` into the EDM nibble,
and r16 = 2 puts the EDM back on list 15. Its callers bracket every EDM
access to client memory:

| set (pc) | restore (pc) | list | what runs inside | when (DDK traces) |
|---|---|---|---|---|
| 0x128b | 0x129b | 3D | the background-block write ([0..2]) | **every 3D start** |
| 0x0941 | 0x094d | TA | the PB setup (0x170d) and the TA free-list load (0x16a2). They read the PB descriptor and TA3D words (kernel data) and write DPM registers. Client memory is touched only when PB descriptor +0x00 b0 is set: then 0x170d walks the PB block list and rewrites the free list | the first kick after the PB changes |
| 0x190c | 0x1918 | TA | `save_ta_state` (0x168d), then the same PB setup and TA load | a PB switch mid-scene; not in any trace |
| 0x0835 | 0x0843 | TA | zeroes the RT data entry's tail-pointer array (entry words 8/9) | a kick path; not in any trace |
| 0x0d90 | 0x0da0 | 3D | a second copy of the background-block write, on another 3D-start path | not in any trace |
| 0x1234 | 0x123a | 3D | copies a 2-word pair indexed by the pass cursor (+0x108) | multi-pass renders only; not in any trace |
| 0x13ae | 0x13be | 3D | writes (address, value) pairs from +0x1a0 (count at +0x19c of the same object) | only with a nonzero count; not in any trace |
| 0x1517 | 0x1527 | 3D | the same pair loop, on the transfer side | only with a nonzero count; not in the trace's transfer |
| 0x0ed5 | 0x0f34 | 3D | a 3D-end branch taken when the ISP clock check fails | not in any trace |

In all four DDK microkernel traces (one-, two- and four-frame runs) only two
windows run. 0x128b runs at every 3D start. 0x0941 runs once, on the first
kick of a new PB.

The open microkernel opens none of these windows: its EDM stays on list 15.
Its one exception was the background-block pulse (`BIF_BANK0` = 0x1011 in
`do_3d`), and with `HOSTCTL_HOST_BGBLOCK` set the host writes that
block instead. Everything else the EDM reads or writes is in kernel code,
kernel data or the sync heap, all mapped in the kernel PD.

The background-block write at 3D start (pc 0x128d-0x1296; `docs/STATE.md`
§6) is the 0x128b window, and it also reads render details +0x130/+0x134/+0x114
inside it. While a window is open, the EDM fetches its instructions and reads
kernel data through the client's page directory. The DDK therefore needs the
kernel code and kernel-data heaps mapped in every client PD; it has no mode
that works without them.

`EUR_CR_BIF_BANK1` (0xc7c, the same nibbles) and `BIF_BANK_SET`
SELECT_EDM (b8) exist, but the DDK never writes BANK1. It touches only
BANK_SET b9 (SELECT_DPM_LSS) around the DPM store and load (pc 0x0c6e,
0x1778, 0x17be). The header doesn't separate the EDM's instruction fetch from
its data accesses in either bank. [D, code and trace]

**Parameter-buffer / DPM setup.** `pb[n]` is word n of the PB descriptor
(`docs/STATE.md` section 6).

- **The TA free-list load** (pc 0x16a2-0x16d3) runs only when the PB changes.
  The microkernel remembers the loaded PB in TA3D +0xb8 (TA) and +0xbc (3D),
  and compares it at the kick (pc 0x091f-0x092e). It writes:
  - 0x618 = `pb[4] & ~0xf`, 0x61c = `pb[6]`, 0x648 = `pb[7]`;
  - 0x638 = `pb[8] | pb[9] << 16`, 0x620 = `pb[10]`, 0x668 = `pb[13]`;
  - 0x624 = `pb[11]`, 0x628 = `(pb[12] & 0xffff) | 0x10000`;
  - 0x664 = a mask computed from TA3D +0xb4/+0xb8/+0xbc;
  - then 0x684 = 1, and waits for `EVENT_STATUS2` b0.
- **The 3D free-list load** (pc 0x16e9) runs the same way. It writes:
  - 0x600 = `pb[4] & ~0xf`, 0x604 = `pb[6]`, 0x64c = `pb[7]`;
  - 0x660 = `pb[8] | pb[9] << 16`;
  - 0x664 = the mask (5 on a first frame);
  - then 0x680 = 1, and waits for `EVENT_STATUS2` b1.
- **Saving the TA's counters.** Before a 3D start the microkernel saves the
  live counters into the descriptor (`save_ta_state`, pc 0x168d-0x16a1):
  - `pb[6]` = 0x700, `pb[7]` = 0x710 << 16;
  - `pb[8]` = the high half of 0x724, `pb[9]` = the high half of 0x72c.
- **Switching PBs** (two clients, each with its own PB). The TA and 3D
  markers differ in the normal course of things: from a new PB's first kick
  until that scene's 3D start, +0xb8 holds the new PB and +0xbc the old one.
  Each side hands over the live counters before it loads, so a descriptor is
  current whenever it is reloaded. Below, P is the PB being loaded, T the TA
  marker and D the 3D marker.
  - **At a kick on a new PB** (pc 0x091f-0x0949, when T != P):
    1. if T != 0 and T != D, the TA state is saved into T's descriptor
       (pc 0x168d);
    2. if P == D, the 3D is paused (pc 0x15bc) and the 3D state is saved into
       P's descriptor (pc 0x16d4);
    3. the PB setup (pc 0x170d) and the TA load (pc 0x16a2) follow.
  - **At a 3D start on a new PB** (pc 0x118d-0x11a7, when D != P):
    1. if D != 0 and D != T, the 3D state is saved into D's descriptor
       (pc 0x16d4);
    2. if P == T, the TA is paused (pc 0x15bc) and the TA state is saved into
       P's descriptor (pc 0x168d);
    3. the 3D load (pc 0x16e9) follows.
  - **The two saves.** The 3D save writes pb[6] = 0x70c, pb[7] = 0x71c << 16,
    pb[8] = 0x728 & 0xffff and pb[9] = 0x730 & 0xffff: the 3D-side
    counterparts of the TA's 0x700/0x710/0x724/0x72c. The 3D state includes
    the pages that render freed, so a PB reloaded without it loses one
    render's pages at every switch.
  - **One shared PB** for every client, as the DDK kernel gives
    (`SGXFindSharedPBDescKM`), makes all of this inert after the first
    frame: T == D == P. Only the power-off save, the PB's own cleanup and a
    reset remain live.
    - After a reset the markers are 0, so the first kick loads from the
      descriptor alone. The host must re-arm it first: +0x18 = +0x08,
      +0x1c = +0x0c (not shifted again), +0x20 = +0x24 = 0, with the free
      list rebuilt.
    - Proven on a CE5300 board (2026-11-08): two forced hangs recovered, then
      200 clean frames. 0x61c stayed at its initial value, 0x700 never reached
      0xffff, and the two halves of 0x724 always matched.
  - **0x664.** Both loads write a mask of which markers differ:
    `(+0xb4 != +0xbc) | (+0xb4 != +0xb8) << 1 | (+0xb8 != +0xbc) << 2`. It is
    not a constant.
- **Arming the page release.** The 3D start writes 0x63c = 3 (0 with render
  details +0xd4 b3). The pages the TA allocated are then freed by the
  hardware while the 3D runs: a CE5300 register sample shows 0x724 counting
  down from 0x190019 during the 3D, and DPM_3D_MEM_FREE follows. Nothing is
  written after the 3D end to start it.
- **DPM state/control (0x694/0x698) take a mode:** 1 store (waits for
  `EVENT_STATUS` 0x90), 2 clear (waits for 0x240), 4 load (waits for 0x20 /
  0x100100).
  - A fresh scene runs the **clear** (pc 0x0a1d-0x0a4a).
  - The load (pc 0x17be) only resumes a stored scene (kick pc 0x09f7-0x0a0d;
    3D start pc 0x1166-0x1181, after the store at 0x1778).
  - A CE5300 register sample shows 0x694 never at 4 over a run of frames.
- **The OTPM clear** (0x24c), with its two done bits, follows.
- **Two scenes on one PB: two DPM context slots.** A client that renders to
  an FBO and then samples it can have two scenes open on the same PB; the
  2026-11-08 `fbo64` capture does it. `tools/ukernel/interleave.py` replays
  two surfaces of `tools/gen`'s v2s trace through a microkernel in two
  orders, and the DDK microkernel's register and memory writes in both are
  below. The TA3D control block holds three words: +0x50 is slot 0's RT data
  entry, +0x54 slot 1's, and +0x64 the TA's current entry.

  **At every kick** (pc 0x08f8). If the TA's current entry is set and is not
  this kick's, the DDK stores that scene's DPM state first and frees its slot:
  1. The TA's slot is read from `0x630` b2, and that slot's word is cleared.
  2. The BIF is pointed at the scene's context.
  3. The DPM state is stored (pc 0x1778), into the scene's own RT data entry:
     - `0x62c` (slot 0) or `0x674` (slot 1) = entry word 5, `0x694` = 1
       (state store), then wait for `EVENT_STATUS` 0x10 and ack it;
     - `0x634` = entry word 6, `0x698` = 1 (control store);
     - `0x234`, `0x24c` = 4, `0x220`, `0x224` (OTPM and TPC flushes), then
       wait for `EVENT_STATUS` 0x480 and `EVENT_STATUS2` 0x800 and ack them;
     - `0x698` = 4.

  So the TA holds one live DPM context at a time.

  **Then the slot pick** (pc 0x09b7). The entry's own slot if it has one,
  else a free slot (slot 0 first), else the slot the 3D is not using,
  `!(0x630 b1)`. Then `0x630 b2` = the slot, the slot word = the entry, and
  +0x64 = the entry.

  **A first kick** (TA flags b0 or b3). `0x630 b0` = the slot. The entry's
  word 5 goes to `0x62c` (slot 0) or `0x674` (slot 1), and word 6 to `0x634`.
  Then the DPM clear: `0x694` = `0x698` = 2, then wait for and ack 0x240.

  **A later kick whose entry was stored** (pc 0x17be). The same registers
  from the entry's words, then `0x694` = `0x698` = 4 (load), before the TA
  continues.

  **3D start.** `0x630 b1` = the scene's slot, and +0x64 is cleared. At the
  render's end, the slot holding the entry is cleared (pc 0x0fbe-0x0fc5).

  The two orders, as the DDK ran them (`0x694` mode, then `0x630`):

  | order | A first | B first | A last | B last |
  |---|---|---|---|---|
  | interleave: B starts while A is open, not rendered | clear; 0 | store A, then clear B in slot 0; 0 | store B, load A; 0 | load B; 0 |
  | overlap: B starts while A's 3D runs | clear; 0 | clear B in slot 1, 0x674; 5 | (3D start A, b1 = 0) | 5, then 7 at B's 3D start |

  - In the overlap order no store or load is needed. Slot 1 exists so that
    the TA can bin a new scene while the 3D renders the other one.
  - The TA and 3D free-list loads stay per PB in both orders.

  [D: the DDK microkernel in the emulator, with OTPM_FLUSHED_INV modeled;
  the interleave itself is from the capture. On hardware the DDK Minecraft
  register sample read 0x740 = 0 throughout. 0x630 read 0 in a register
  sample across an FBO interleave, which may mean 0x630 does not read back.]

All [D] from the DDK microkernel's code and trace; the release behavior is
from the hardware register sample.

**TA start.** The first kick of a scene (TA flags b0) pulses `SOFT_RESET`'s
TA bit (0x80 = 4, then 0; pc 0x091d). The kick's cache bits follow (TA
flags b4-b6, `docs/STATE.md` section 1). After the layout registers
(0x204-0x218 and 0x238/0x23c, loaded from cmd+0x238..+0x254) and the PDS/USE code
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
run that completes has executed real behavior, not guesses.
`tools/ukernel/sgxsim.py` lays kernel data out as `srvinit` does, models the
five entry PDS programs as behavior, answers the registers the microkernel
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
- The `0x138` invalidate-done bit groups (0x27 TA, 0x4b 3D) are modeled as
  the hardware's answers from what the poll loops wait for; the per-unit
  decomposition is inferred, not confirmed on hardware.
- The precise contents of the per-context scratch area (control block words
  0x18-0x37) across multiple concurrent render contexts — the single-context
  single-render path (es2gears, one Minecraft frame) is covered.
  - Two render contexts that share one PB render 10/10 on the open
    microkernel under powervr_sgx (2026-11-04).
  - Under the DDK microkernel, a second HW render context with a PB of its
    own (two pipe contexts under services) made renders slow and then locked
    up. Placing the PB away from the 3D-params heap base was found to cause
    one such hang (UAPI.md G15).
  - Gap: whether a second, correctly placed PB per context works on either
    microkernel. One shared render context or PB is the configuration known
    to work.
- **Re-opened 2026-11-08:** the 2026-11-04 multi-frame "PB never drains"
  result (0x724 stuck at 0x190019) was put down to the srvinit frame-dump
  harness, because powervr_sgx rendered 10/10 frames.
  - The M1-exit run then hit `DPM_OUT_OF_MEMORY_MT` at about frame 40 on
    powervr_sgx.
  - 25 pages a frame × 40 frames ≈ 0x400, the PB's global threshold. That is
    what a 3D that never releases its TA's pages would do.
  - So the drain failure is probably real, and the 10/10 runs were simply
    shorter than the PB.
  - The DDK drains 0x724 to 0 during every 3D (CE5300 register sample).
  - **Fixed (um-re f0dc24d, 2026-11-08).** The cause: the DDK runs the
    TA free-list load, `save_ta_state` and the 3D free-list load only when
    the PB changes, and the open microkernel ran them every scene. Each TA
    load then re-seeded the free-list head from the descriptor and skipped
    the pages the last 3D had freed.
  - The fix gates both loads on per-PB markers (control block +0xb8/+0xbc,
    as the DDK's TA3D +0xb8/+0xbc). It resets both markers at init and on
    cleanup, as the DDK does on context and PB cleanup; without the reset, a
    PB reallocated at the same address skipped its load. 0x63c = 3 is still
    written at every 3D start.
  - Results: 600 frames with 0 differing, and a second process's 100 frames
    with 0 differing, no OOM.
  - **Corrected 2026-11-08.** Resetting the markers on every cleanup was too
    broad. A render-target or render-context cleanup, sent while the PB lives
    on, reloaded the free list from a descriptor last saved at the first
    frame's 3D start. In a CE5300 register sample of Minecraft, 0x61c and 0x638
    snap back to their first-frame values after a 30 ms idle. The TA head
    (0x700) then runs to 0xffff within one scene. In the DDK's 120 s sample,
    0x61c, 0x604, 0x638 and 0x660 never change. The DDK clears the markers in
    three places:
    - a cleanup whose PB descriptor equals the marker (pc 0x03a5-0x03b2, when
      cleanup data word 0 b5 is set and data word 6 matches; pc 0x0475-0x047b);
    - the power-off path (pc 0x04e7-0x051f, r6 b0). It first saves the TA's
      live counters into the +0xb8 descriptor (pc 0x168d: pb[6] = 0x700,
      pb[7] = 0x710 << 16, pb[8] = 0x724 >> 16, pb[9] = 0x72c >> 16). When
      +0xbc is set and differs from +0xb8, it saves the 3D's into the +0xbc
      descriptor (pc 0x16d4: 0x70c, 0x71c << 16, 0x728 & 0xffff,
      0x730 & 0xffff). Only then does it clear both markers;
    - a reset path (pc 0x0292-0x02aa), unconditionally.
  - The open microkernel (39c2de6) no longer clears the markers at a cleanup.
    Init resets them, and the host never reallocates a PB descriptor at the
    same address within a session. A power-off that loses the DPM registers
    would still need the DDK's save-then-clear.
- The timer / active-power / lockup-detection path is read structurally but
  not run (no timer event in an offline frame).
- The recovery rescan (section 5a) is understood from the init path and the
  replayer's hardware incident, but exactly which dereference in the dead
  context's structures faults or stalls the second re-init is not pinned down.
  The open microkernel's "discard on recovery" approach sidesteps it rather
  than reproducing it, so confirming the DDK's exact failure is useful for the
  write-up but not a prerequisite for the replacement.

## 10. Implementation map and open gaps

A reference for the `ukernel/` implementation: where each handler's behavior
is specified, what it depends on from the other documents, how far it is
validated, and what is still missing.

**Every handler in `ukernel/ukernel.py` and where its behavior is specified:**

| handler | spec |
|---|---|
| `event` (dispatch) | §2 (the control block as secondary attributes; dispatch table §3) |
| `init` / recovery | §5a |
| `kick` (CCB read + type dispatch) | §3 |
| `do_ta` (TA kick) | §3, §6 (BIF/invalidate/DPM), §7 (register tables) |
| `ta_finished` (status walk, 3D start) | §5 |
| `do_3d` | §5, §7 |
| `render_done` (sync + teardown) | §5 ("3D end of render", "DPM 3D memory free") |
| `xfer_done` (transfer completion) | §3 (Transfer) |
| `k_getmiscinfo` / `k_power` / `k_ctxsuspend` / `k_cleanup` | §3 |
| `k_transfer` | §3 (Transfer), with the word→register table |

**What `ukernel/` relies on that is specified in other documents, not here:**
the USSE instruction encodings and the `moe`/repeat load-store addressing, the
`imae` 16-bit source truncation and the `phas`/program-end flags are
`docs/ISA.md`; the control-block base arriving in a loader-patched `limm`
(the event program is a bare `DOUTU`, no DMA), the five entry PDS programs,
and the kernel-data layout `srvinit` allocates are `docs/SRVINIT.md` and
`docs/PDS.md`; the render-details / TA-command / transfer-command structures
whose words §7 and §3 read are written by the Mesa driver and specified in
`docs/STATE.md` and `docs/TEX.md`.

**Validation.** `ukernel/run_ours.py` builds the open microkernel and diffs
the registers and memory it writes against the DDK firmware's, per phase, as an
`oracle` comparison in the offline emulator, over generated frames (`tools/gen`
`frame-fake` and `blitest`) and a real capture: TA/3D 41+61 registers, the
12-write render teardown and the 21-register transfer all match.

**The first hardware run** stalled in init: the event entry wrote `sa`
directly in the bare-`DOUTU` context (§1). The fix carries the control-block
base in a reserved temp (`r17`) from the loader-patched `limm`. The history is
in `docs/log/entries/2026-11-08-ukernel-bringup-history.md`.

**Implementation gaps the re-implementer should know (flags for the reviewer):**
- **A transfer's syncs are not all updated.** `xfer_done` writes the status
  word (`sCtlStatusInfo[0]`, command words 0x168/0x16c), but not the
  destination and source syncs the shared command lists: `ui32NumDstSyncs` at
  0x130, `asDstSyncs[0]` at 0x134 (read-ops pending, read-ops-complete address,
  write-ops pending, write-ops-complete address), `ui32NumSrcSyncs` at 0xac and
  `asSrcSyncs[8]` from 0xb0. The DDK microkernel sets the destination's
  write-ops-complete to its pending value plus one, and `powervr_sgx` completes a
  transfer job on exactly that (log `2026-11-03-0405-drm-transfers`), so
  the open microkernel has to do the same.
- **Single context only.** The render-in-flight list is one control-block slot
  (+0x50); concurrent contexts are not handled (§9).
