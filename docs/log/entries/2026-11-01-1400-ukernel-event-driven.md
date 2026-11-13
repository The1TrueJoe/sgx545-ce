# The open microkernel runs the full event-driven path

M1 (AGENTS.md §5): the microkernel must run on hardware through the real event
flow, not with its entries driven one by one. With the EAs offline this is
built and checked offline against the DDK.

## One event program, one dispatcher

The bring-up fix made the event program a bare DOUTU to one entry. That entry
is now the microkernel's **event dispatcher**: it loads the control-block base
from the loader-patched limm, populates the four secondary attributes the
handlers read (self / host-control / CCB-control / kernel-CCB) — the software
equivalent of the DDK's pds1 DMA, since an event-kicked program does not run
one — and dispatches:

- `ui32InitStatus == 0` → **init** (clean or recovery);
- `EUR_CR_EVENT_STATUS` SW_EVENT → **kick** (reads the kernel CCB, runs the TA);
- TA_FINISHED → **ta_finished** (the status-value writes; and, when the
  command's CMDTA_SHARED READY flag marks the scene's last kick, the 3D start);
- PIXELBE end / ISP end-of-tile → **render_done** (the destination sync update
  and the host interrupt).

Every handler is a branch target of this one task, so only the dispatcher has
the phase/end wrapper. The handlers' register and sync logic is unchanged.

## Validated offline, end to end

`run_ours.py` now drives the real event entry per phase (it sets
`EUR_CR_EVENT_STATUS` and the kernel CCB, patches the control-block limm, and
lets the microkernel populate `sa` itself). The whole frame — bring-up, host
kick → TA, TA finished → status writes → 3D start, 3D end → completion — comes
out register- and sync-identical to the DDK: 41 TA + 61 3D struct registers,
the 7 TA status writes, and the completion sync, on the reference capture and
three `frame-fake` scenes. `tools/ukernel/pds.py` confirms the bare-DOUTU
event program starts the entry and init sets `ui32InitStatus`.
`ukernel/test_emu.py` drives init (clean + recovery) through the event entry
too. `srvinit -o` is unchanged (bare DOUTU, one `ctlblk` limm patch) and still
cross-builds; the DDK `-f` path is untouched.

This is the hardware path, offline-complete. What remains for M1 is the
missing commands (transfer, cleanup, power, get-misc-info, HWR re-init) and
the full render teardown with multi-render bookkeeping, then the CE5300 board run when
the box is back.
