# The open microkernel writes the TA status values

Follow-up to `2026-10-28-1930-ukernel-ta3d-kick.md`. The open microkernel's
TA-finished handler now walks the command's status-value list and writes it to
the sync heap, matching the DDK.

## What it does

On a TA's finish, `ukernel/ukernel.py` reads `ui32NumTAStatusVals` from the
command's `SGXMKIF_CMDTA_SHARED` (command+0x5c) and, for each, the
`{sStatusDevAddr, ui32StatusValue}` pair from command+0x118 onward, and writes
the value to the address (docs/UKERNEL.md §5). These are the sync-heap
counters the host and the next client wait on.

The list is walked by index, `address = base + i*8`, because `imae` (the only
integer multiply-add) truncates its first operand to 16 bits — so a 32-bit
pointer advanced through it corrupts; a small index must drive the addressing,
as the DDK's own list walks do.

The TA register programming and the status walk were split from the 3D start
so each runs on the right event: the status walk after every TA kick (per that
kick's command), the 3D start only on the last kick of the scene.

## Validation

`ukernel/run_ours.py TRACE --ddk FW` now also diffs the sync-heap writes. On
the reference DDK capture ours writes the same 7 status values to the same
sync-heap addresses (`0x2d003040 = 0x5c`, … `0x2d00303c = 1`) as the DDK; the
three `frame-fake` scenes have no status values (count 0) and ours writes
none, as the DDK does. The 41 TA + 61 3D struct-sourced register writes still
match on all four frames. `ukernel/test_emu.py` (clean init, recovery-discard
init, host IRQ) stays green.

## Remaining before hardware

The 3D **destination** sync-object update (`ui32WriteOpsComplete = pending+1`
over the render's sync list) is the last completion piece; the DDK walks
several sync categories from the per-render structure with a counted inner
loop, and reproducing it needs that structure's field offsets traced. After
that: the completion-event *waits* (cache-invalidate / DPM-load — ours pulses
without waiting), then the `srvinit --ours` loader. Only then the EA3, which
the user has shared for this, step by step (bring-up, replayed frame, es2gears,
recovery), coordinated with the box's other sessions and recovered with the
Bench plug. No hardware has been touched.
