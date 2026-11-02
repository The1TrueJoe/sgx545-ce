# The bring-up fix: patched limms, not a DMA in the event program

The first EA3 slot (`2026-10-29-...`) got the core up but our microkernel never
set `ui32InitStatus`. The cause, pinned down offline this round.

## A PDS interpreter

`tools/ukernel/pds.py` runs a PDS program against the emulator's device memory
and starts the USSE task it launches: DOUTD DMAs into the secondary
attributes, DOUTA writes a primary-attribute register, DOUTU launches. So the
whole event-program → microkernel path runs offline. It confirmed our *old*
event program's logic was correct: it DMAs the control block into `sa0..63`
(self/hostctl/ccbctl right) and launches init, which sets `ui32InitStatus` =
1. So the hardware failure was not a logic bug.

## What was wrong

Our old event program (`pds_ours`) combined a DOUTD (DMA the control block
into `sa`) with the directly-started entry. No DDK program does that: the
DDK's event program is a *bare DOUTU*, and it does its DMA in a separate
program the microkernel triggers by emitting an event. An event-kicked PDS
program is evidently not relied on to run a DMA, so our DOUTD did not populate
`sa`, and init wrote `InitStatus` to a stale address — exactly the observed
"control block correct, InitStatus still 0".

## The fix

Use the DDK's own mechanism for dynamic addresses — patched limms — and keep
the event program a bare DOUTU:

- the init entry now reads the host-control, CCB-control and control-block
  addresses from three `limm` sites, not from `sa`;
- `ukernel/build.py` records those sites in a `.pat` sidecar;
- `srvinit -o` builds `pds0` as a bare DOUTU (bit 27, like every directly
  started microkernel entry) to our entry, and fills the three limms with the
  real addresses (`limm_ours`), exactly as the DDK build patches its own
  placeholders.

Validated offline with the PDS interpreter against the binary and event
program srvinit actually produces: bare DOUTU → init → `InitStatus` = 1 on a
clean boot, and on a recovery boot (HWR set) init discards the stale CCB and
empties the render-in-flight list, then sets it. `ukernel/test_emu.py` patches
the limms the same way and passes; the 41 TA + 61 3D register match, the TA
status writes and the completion sync write are unchanged.

Only `sa` is gone from the *init* entry. The TA/3D handlers still read the
control block through the secondary attributes; on hardware they will need the
same limm-base treatment (address the control block from a patched base
register), since the event program no longer DMAs. That is the next step,
after this bring-up is confirmed on the EA3.
