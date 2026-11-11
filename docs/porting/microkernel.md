# Bringing up the microkernel on an SGX core

This is the blueprint for getting the open microkernel running on a PowerVR SGX
GPU — SGX545 today, any SGX core next. It is a bring-up order: each stage builds on
the last, and each has a way to tell it is done before you move on. The hard part
of SGX545 took weeks of hardware runs; this guide is so the next core does not.

All methods here are **black box**: you compare the microkernel's output against a
reference (the offline emulator, or a vendor image run only as an oracle), never
against anyone's source. How the hardware works is in [`UKERNEL.md`](../UKERNEL.md);
the SGX545 story, run by run, is in [the build log](../log/BUILDLOG.md) and its
`entries/`. This guide links them instead of repeating them.

A new core is meant to be a config file plus tests, not a fork. The per-core
config, the build's core selector (`build.py <core>` / `SGX_CORE`) and the
symbolic assembler now exist — [`cores/sgx545.py`](../../ukernel/cores/sgx545.py)
is the first core. The ISA-coverage gate and assembling through the shared
`sgx.xml` host tool ([`UKERNEL-TOOLING.md`](../UKERNEL-TOOLING.md) §6 steps 5-6)
are the remaining cross-repo pieces on the Mesa side, so coordinate in your
core's issue. Most stages below are "confirm the shared handler works on this
core"; only a few need new code, and those are called out.

---

## Tools

- **Offline emulator** (`tools/ukernel/sgxsim.py`, `run_ours.py`). Builds the
  microkernel and runs it against a register/memory model, diffing every register
  and memory write per phase against the reference firmware. This is where a stage
  is designed and first checked, before any hardware.
- **Oracle / interleave** (`tools/ukernel/interleave.py`, `interleave_ours.py`).
  Replay a multi-render trace (`tools/gen`) through the reference firmware and
  through ours in a chosen order, and diff the two. How render ordering, overlap and
  the queue were validated.
- **`run_ours` per-phase oracle.** The assertion "our registers and memory equal the
  reference firmware's, phase by phase" is the correctness bar for every stage.
- **The hwt rig** (`tools/isa/hwt*.py`). Drives real hardware for the ISA and for
  small microkernel probes: submit a known input, read back, compare. Use it to
  confirm a register address or a sequence on the actual core.
- **Build** (`ukernel/build.py <core> out.bin manifest.json`). Deterministic; the
  core's serial image has a checked-in golden and CI fails on any diff.

## Safety, for fragile boards

These boards are old and often the only one someone has. The rules that kept ours
alive:

- **Read only named registers.** Every register you read must be one you can name
  and expect a value from. A blind sweep of a BAR can wedge the GPU or the box — a
  read-only sweep of the display controller's address space hung a CE5300 board
  ([`2026-11-03` memory-constants run](../log/entries/2026-11-03-0311-memory-constants-hw.md)).
- **No MMIO sweeps, ever.** Discover addresses from captures and the emulator, not by
  scanning. The register map is in `UKERNEL.md` §6; a new core overrides only what
  moved.
- **Expect to wedge the GPU and plan the recovery.** A microkernel hang usually
  needs a GPU reset (the kernel's reset path) or a power cycle of the board; keep a
  way to power-cycle remotely. Run one job at a time while bringing a stage up, and
  take a hardware lock before any run so two sessions never drive one board at once.

---

## The stages

Each stage: what to **establish**, the **tools**, what **done** looks like, and the
**traps** that cost us time (one line, with the log entry).

### 1. The processor boots our code

**Establish.** The GPU's EDM (the microkernel processor) fetches and runs our image,
reaches the event loop, and does not fault. The control-block base must arrive in a
reserved register, because the event entry is a bare `DOUTU` with no DMA.

**Tools.** Emulator first (`run_ours` reaches the event loop); then the kernel loads
the image and you watch for the first sign of life (a heartbeat write, no BIF
fault).

**Done.** The core is up and idle in the event loop, no fault, no reset.

**Traps.**
- The event entry wrote `sa` directly in the bare-`DOUTU` context and stalled init;
  the base must be carried in a reserved temp (`r17`) from the loader-patched `limm`
  ([bring-up fix](../log/entries/2026-11-01-1200-ukernel-bringup-fix.md),
  [init confirmed](../log/entries/2026-11-02-1000-ukernel-ea3-init-confirmed.md)).

### 2. Init without the vendor srvinit

**Establish.** Our own init: program the core registers the microkernel needs
(`UKERNEL.md` §4), set up the event kick (`0xa68`/`0xa6c`) and the periodic timer,
and leave the lists/BANK0 as the kernel's reset set them. No vendor srvinit runs.

**Tools.** `run_ours` diffs our init writes against the reference; the kernel's reset
path sets lists 0/15 and BANK0 before the microkernel runs (see `UKERNEL.md` §5a).

**Done.** The microkernel reaches its event loop having configured itself; a host
kick is serviced (stage 3).

**Traps.**
- Our own `srvinit` replacement had to supply what the blob's did
  ([srvinit-ours](../log/entries/2026-10-28-2140-srvinit-ours.md)).

### 3. Host kick and the CCB

**Establish.** A host `EVENT_KICK` wakes the microkernel; it reads the kernel CCB at
the write offset, services the command, and advances the read offset. The kick is
gated on write-offset ≠ read-offset, not on a bare doorbell.

**Tools.** `run_ours` with a host-kick event; on hardware, submit one command and
confirm it is consumed (`ccb wo`/`ro` advance).

**Done.** A kick is serviced and the read offset advances; a second kick is serviced
too.

**Traps.**
- The first kick was lost until the handler gated on `WO ≠ RO` and the ready check
  was right ([host kick](../log/entries/2026-11-02-2045-ukernel-ea1-host-kick.md),
  [serviced](../log/entries/2026-11-02-2100-ukernel-ea1-kick-serviced.md),
  [read offset](../log/entries/2026-11-03-0330-ukernel-ccb-readoffset.md),
  [ready check](../log/entries/2026-11-03-0345-ukernel-readoffset-works-ready-check.md)).
- A snapshot/ack race dropped a kick ([snapshot ack lost kick](../log/entries/2026-11-03-1040-ukernel-snapshot-ack-lost-kick.md)).

### 4. The first TA

**Establish.** A TA (tiling/vertex) kick runs: point the BIF at the context PD,
load the DPM free list, program the TA registers from the command, kick, and handle
the TA-finished event and its status walk.

**Tools.** Emulator per-phase diff of the TA registers (`run_ours`); `tools/gen`
makes a known frame; on hardware, confirm the TA-finished event and the binned
output.

**Done.** A TA binds and finishes; the control-stream/region output is correct.

**Traps.**
- The TA-finished event must be acked or it re-triggers forever and starves the next
  kick ([ta-finish ack](../log/entries/2026-11-03-0025-ukernel-ta-finish-ack.md)).

### 5. The first 3D and the first frame

**Establish.** After the scene's last TA, flush the TPC, start the 3D, and complete
on the rendezvous of the pixel-back-end and the DPM free. The 3D reads the region
headers and background-object PDS the TA produced. This is the hardest stage — most
bring-up time was here.

**Tools.** `run_ours` phase diff of the ~61 3D registers; `interleave.py` for
ordering; `tools/gen` frame read back and compared to the reference render.

**Done.** One TA+3D renders a generated frame that reads back correct — the "frame
from code" milestone ([frame from code](../log/entries/2026-10-28-0830-frame-from-code.md),
[first triangle on hardware](../log/entries/2026-11-03-0925-ukernel-first-triangle-renders.md),
[first frame](../log/entries/2026-11-02-2150-ukernel-ea1-first-frame.md)).

**Traps.**
- The 3D reached but stalled at tile 0 with a clean BIF through a long saga — the
  causes were the TPC flush, the background-object PDS, and the DPM state hand-off,
  not any one fix ([do_3d reached, 3D hangs](../log/entries/2026-11-03-0400-ukernel-do3d-reached-3d-hangs.md),
  [TA output correct, 3D still stalls](../log/entries/2026-11-03-0557-ukernel-ta-output-correct-3d-still-stalls.md),
  [DDK renders, ours stalls](../log/entries/2026-11-03-0700-ea3-ab-ddk-renders-ours-stalls.md)).
- `0x630` (the double-buffered DPM slot select) left at a power-up value points the
  3D at a slot the TA did not fill → the same clean-BIF stall (`UKERNEL.md` §6).

### 6. Multiple frames

**Establish.** Frame N+1 reuses state frame N set; the per-frame registers that must
be reset are, and the ones that must persist are not. `0xa74`'s fields are a
masked-replace so init bits survive.

**Tools.** `interleave.py`/`interleave_ours.py` across several frames; the hardware
runs many frames without a reset.

**Done.** Many frames render back to back, 0 recoveries.

**Traps.**
- `0xa74` must be read-modify-write (keep srvinit's config bits, replace only the
  per-frame field) or the second frame is wrong
  ([multiframe a74 masked-replace](../log/entries/2026-11-03-1010-ukernel-multiframe-a74-masked-replace.md)).
- The periodic event timer must be enabled and its event acked, or the host watchdog
  false-HWRs a quiescent core — and the first fixes had no effect until the ack was
  right ([timer no effect](../log/entries/2026-11-03-1945-ukernel-event-timer-no-effect-yet.md),
  [timer fixes frame 1](../log/entries/2026-11-03-2030-ukernel-event-timer-fixes-frame1.md),
  [cold GPU still HWRs](../log/entries/2026-11-03-2120-ukernel-ea1-coldgpu-timer-runs-still-hwrs.md)).

### 7. Power-down and idle

**Establish.** Between frames the core can clock-gate and power down and come back on
the next kick. Auto clock gating (`CLKGATECTL`) is left to the hardware; runtime PM
holds a reference so the core is not powered off under a live job.

**Tools.** The power log (`UK_PWRLOG`); the kernel's runtime-PM path; watch
`CLKGATESTATUS` only as a named read.

**Done.** The core idles and wakes across frames with no lost kick and no false HWR.

**Traps.**
- The idle/active-power handling is subtle; clearing active power did not by itself
  fix a lockup, and runtime PM wedged the GPU on one path
  ([idle active-power analysis](../log/entries/2026-11-02-2130-ukernel-idle-active-power-analysis.md),
  [active-power clear is not the fix](../log/entries/2026-11-03-1730-ukernel-active-power-clear-does-not-fix-lockup.md),
  [runtime PM](../log/entries/2026-11-03-1700-drm-runtime-pm-ea1.md)).
- `CLKGATESTATUS` is **not** EDM-readable as a plain value on 545 — read it only from
  the devcd (the kernel side), not from the microkernel (`UKERNEL.md` §11).

### 8. Secure mode

**Establish.** With the client's heaps not mapped into the microkernel's own PD (no
blanket sharing), the microkernel must not fetch client memory through the EDM. The
background-object block is written by the host instead (`HOSTCTL_HOST_BGBLOCK`), and
the microkernel skips the EDM→client pulse.

**Tools.** The secure-mode kernel path; `run_ours` with the host-bgblock flag set.

**Done.** Frames render in secure mode with clients isolated from the firmware.

**Traps.**
- The EDM→list-1 pulse to write the background block faults under secure/no-share;
  the host writes the block and the microkernel skips the pulse
  ([secure mode](../log/entries/2026-11-08-1858-drm-secure-mode.md),
  [latency in secure mode](../log/entries/2026-11-08-1815-drm-ea3-latency-secure.md)).

### 9. OOM partial render

**Establish.** When the parameter buffer fills mid-scene, the microkernel renders a
partial frame to free pages and resumes, instead of hanging. The teardown order and
the DPM free-list bookkeeping are exact.

**Tools.** `run_ours` OOM path; a scene large enough to exhaust the PB; the devcd at
the hang.

**Done.** A PB-full scene degrades to a partial render and completes; a long world
run has 0 recoveries.

**Traps.**
- A frame ran ~40 frames then hung on a PB free-list leak — the terminate path missed
  a free-list store ([frame-40 PB leak](../log/entries/2026-11-08-1915-drm-frame40.md)).
- The DPM pages read zero after the first TA and looked like a power problem but were
  not ([DPM wiped after TA1](../log/entries/2026-11-03-1900-ukernel-dpm-wiped-after-ta1-not-power.md),
  [terminate-TA DPM pages zero](../log/entries/2026-11-03-1600-ukernel-terminate-ta-dpm-pages-zero.md)).

### 10. Overlap (parked)

**Establish.** Running render N+1's TA while render N's 3D is in flight (the DDK's
`credit=2`). This is **parked** on SGX545: the vendor microkernel proves it is
achievable, but even working it buys ~2% here (the GPU is already ~96% busy), and a
queued-3D kick-drop survived six fixes. A new core should get stages 1-9 working and
measured before considering overlap.

**Tools.** `interleave_ours.py --order overlap2`; the full story and the one
remaining lead (alternating BIF directory lists) are in `UKERNEL.md` §11 and
[the overlap entry](../log/entries/2026-11-10-1030-ta3d-overlap-parked.md).

**Done.** Not required for a working core. If pursued: the queued 3D kicks and
steady-state frames render, judged on the first overlapped pair, not teardown.

**Traps.**
- `BANK0`'s 3D nibble gets zeroed by a `0x1f` write during the concurrent TA,
  re-routing the live 3D to the idle PD → fault; the write must be read-modify-write
  (`UKERNEL.md` §11).
- A BIF directory-cache invalidate under a live 3D (the TA's first kick on the same
  PD) is a hazard the emulator cannot show (`UKERNEL.md` §11).

---

A fuller narrative of the SGX545 bring-up, in order, is
[`2026-11-08-ukernel-bringup-history.md`](../log/entries/2026-11-08-ukernel-bringup-history.md).
