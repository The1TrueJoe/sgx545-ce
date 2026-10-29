# The open microkernel completes a render

Follow-up to `2026-10-28-2010-ukernel-ta-status.md`. The open microkernel's
3D-end handler now updates the destination sync object the host (and the
blob-free replay) polls on, so a single rendered frame can be observed as
done.

## What it does

On the 3D end-of-render event, `ukernel/ukernel.py`'s `render_done`:

- reads the render details' dependency-block pointer (render-details word 56);
  its sync object holds the write-ops-pending value at +0x10 and the
  write-ops-complete device address at +0x14. It writes `pending + 1` to that
  address — the update the host's `PollForValueKM` on the back-buffer sync
  waits for (docs/UKERNEL.md §5);
- resets the write-ops tracking word (render-details word 66), matching the
  DDK's `rd + 0x108 = 0`;
- raises the host interrupt (`SW_EVENT`).

Single render: the frame index is 0, so there is no per-frame stride into the
dependency block.

## Validation

`ukernel/run_ours.py TRACE --ddk FW` now also diffs the render-completion
sync-heap writes. On the reference DDK capture ours writes `0x2d001094 = 1`
(the dependency sync) and `rd + 0x108 = 0`, both matching the DDK; those are a
subset of the DDK's three completion writes, and every write ours makes equals
the DDK's (no mismatch). The 41 TA + 61 3D register writes and the 7 TA
status-value writes still match. All green.

## What the DDK does here that ours does not yet

At 3D-end the DDK also walks the render's other sync categories (a counted
list at render-details word 81, three entries on this frame), returns the
parameter-buffer free list, and updates the render-in-flight list and the
context's per-frame refcount. Those need the control-block bookkeeping a
multi-render scheduler keeps, which our one-context microkernel does not
maintain; they matter for multi-frame stability (Minecraft), not for a single
replayed frame to display. They, the completion-event *waits*
(cache-invalidate / DPM-load), and the `srvinit --ours` loader are what remain
before the EA3. No hardware has been touched.
