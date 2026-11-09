# Runtime PM, first try: the EA1 froze

EA1 RAM boot of 2fec9ff, 2026-11-03 ~06:10 UTC, DDK microkernel, nothing flashed. The
box stopped answering ping and ssh and needs a power-cycle. It has no smart plug.

## What ran

2fec9ff added runtime PM:
- a job on the GPU holds a runtime PM reference;
- autosuspend after 500 ms;
- suspend is the microkernel power-off (`pvrsgx_fw_stop`: `POWER`/`POWEROFF`, wait for
  the clocks to gate, deinit script, cores held in reset);
- resume is the full start a reset does, with the PB set up again.

The SGX's PCI function has no PM capability, so the PCI core leaves it in D0.

The driver loaded, the microkernel started, and 500 ms later the device was runtime
suspended. The host control block read power status 0x2c (POWEROFF_COMPLETE) and
`runtime_status` read `suspended`. The next command started the uAPI suite and then
`powervr_sgx_hang` (render, mixed submit, then waits for suspend and renders from
suspended, plus forced hangs). No output came back: the box froze before the session
returned anything, so it is not known which step it reached.

## Suspects

Two things are new with runtime PM:
- **The SGX now stays powered off for long stretches while its interrupt handler is
  registered.** The handler reads `EVENT_STATUS` on every interrupt of its line, and
  the line is shared. Before 2fec9ff the stopped state lasted only from the unload's
  `fw_stop` to `free_irq`. A register read of an unclocked CE5300 block can hang the
  bus (the VDC sweep hung an EA3).
- **The restart path ran after a clean power-off.** Until now it had only ever run
  after a hang, with the clocks still on.

Changes made without the box:
- a `powered` flag, cleared (and the IRQ synchronized) before the deinit and set at
  the start of every start. The hard IRQ handler, debugfs `status` and the
  devcoredump read registers only while it is set, and a stop that finds the device
  already off no longer touches it;
- `runtime_pm` is off by default until a box shows which of the two suspects it was.

## Next on hardware

1. `runtime_pm=0`: the uAPI suite, frames, blits and hangs, to show the rest of the
   build is unaffected.
2. `runtime_pm=1`: suspend and resume alone, with the shared interrupt line's other
   user named (`/proc/interrupts`), and no forced hangs.
3. Then the full `powervr_sgx_hang` with its idle case.

## Outcome

On the EA3 the first resume froze the same way, and the cause was the second suspect:
the restart's quiesce reads a register of the gated core. Fixed in log
`2026-11-03-0630-drm-runtime-pm-ea3`.
