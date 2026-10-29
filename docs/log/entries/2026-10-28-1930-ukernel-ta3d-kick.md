# The open microkernel programs the cores like the DDK

Follow-up to `2026-10-28-1830-ukernel-spec-and-emulator.md`. The open
microkernel in `ukernel/` grew from the init handshake to a one-context TA and
3D kick, and it now programs the GPU's core registers identically to the DDK
for a whole frame, checked offline.

## What it does (ukernel/ukernel.py)

- **init**, two paths. A clean boot sets `ui32InitStatus`. A recovery re-init
  (the host control block's `ui32InterruptClearFlags` HWR bit set, which
  `HWRecoveryResetSGX` sets) first discards the surviving kernel CCB (read
  offset := write offset) and empties the render-in-flight list, then sets the
  status. That is the fix for the double-lockup wedge the replayer session hit
  (`2026-10-28-2210-hwr-workqueues.md`, docs/UKERNEL.md §5a): the DDK rescans
  the stale state and walks into a dead context; ours drops it.
- **host-kick dispatch**: read the kernel CCB command, dispatch on its type
  (our host-kick table is the identity, so `ui32ServiceAddress` *is* the type).
- **TA kick**: from the context and the client CCB control, derive the TA
  command, render details, PB descriptor and RT entry; point the BIF at the
  context's page directory; program the parameter-buffer, TE/macrotile/screen
  and RT-data registers (the 41 struct-sourced registers of docs/UKERNEL.md
  §7, including the `0x628 |= 0x10000` global-threshold bit and the TA-flags-2
  computation at `0x218`); start the TA.
- **3D start**: program the 61 ISP/PBE registers from the render details, plus
  the fixed 3D constants; start the 3D.

The register-programming tables in `ukernel.py` are §7's provenance turned
around: each says which structure word to load a core register from. The DDK's
elaborate multi-render scheduler is not reproduced — ours is the straight path
one client's frame takes.

## Validation (ukernel/run_ours.py)

`run_ours.py TRACE --ddk FW` runs our microkernel and the DDK's through the
same frame in the emulator and diffs, per phase, every core register each
programs from a structure. They **match exactly** — 41 registers on the TA
kick, 61 on the 3D start — for four frames: the reference DDK capture
(`cap/ref.bin`) and three `tools/gen` `frame-fake` scenes (the default
triangle, a 320×200 clear-and-colour, and a depth test). So for these frames
the open microkernel sets the GPU up bit-for-bit as the DDK does.

`ukernel/test_emu.py` adds the no-trace checks: clean init, recovery-discard
init, and the host-interrupt tail.

## Not yet (next increments, each to be validated the same way)

The completion-event handshakes (the cache-invalidate and DPM-load *waits* —
ours pulses the request but does not yet wait), the TA status-value and 3D
sync-object list walks (docs/UKERNEL.md §5), transfers, cleanup, power, and
the multi-render scheduler. Nothing has run on hardware; a full frame's
completion path must be reproduced offline first, then the box and recovery
method chosen.

Data: `data/2026-10-28-ukernel-impl/ours-registers.txt` (our register writes
for the default frame).
