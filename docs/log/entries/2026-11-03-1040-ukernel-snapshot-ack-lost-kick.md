# Event ack: mask to the snapshot so a concurrent kick isn't lost; the task-mutex is HW-neutral, held

Two things came out of chasing the ~35% lockup_TA that caps consecutive frames.

## Fix: ack only the event bits the snapshot saw

The dispatcher took a snapshot of EUR_CR_EVENT_STATUS (r2), then acknowledged a
fixed mask (0x2000|0x40000|0x800000) to EUR_CR_EVENT_HOST_CLEAR unconditionally
before dispatching. If a second event bit set AFTER the snapshot — e.g. a TA
finished for a kick this task has not dispatched yet — the unconditional ack
cleared it, so that event never re-triggered and its kick was lost
(ta3d-state's race_interleave.py: ~5 interleavings dropped a kick). Fix: `and
r3, r2, r3` before the HOST_CLEAR store, so we clear only the bits the snapshot
dispatched. Offline: run_ours fake/v1/v2/xfer/xfer-rect MATCH, test_emu 5/5,
test_commands OK. (0x134 is in run_ours PLUMBING; the fix is validated by
race_interleave, not run_ours.)

## The task mutex is offline-perfect but HW-neutral — held out

The ~35% lockup_TA looked like a two-event-task race (kick 2 racing kick 1's TA
on the shared flag / CCB). ta3d-state's race_interleave.py sweeps every two-task
interleaving and the current code fails 283+26 of them with a double 0x200 kick.
A hardware `lock #1`/`release #1` mutex (event entry / every a.end()) closes all
of them: 358/358 + 61/61. But on the EA3 it made NO difference — 7/10 frames vs
6/10, and the failed frames show the identical lockup_TA signature (EVENT_STATUS2
0x80, 0x2 tasks, isp_end_tile no, BIF_FAULT 0). So the two-task interleave is not
what fails on hardware: either the EDM runs one event task at a time, or HW
`lock` only catches a same-task re-lock (the DDK assert idiom `lock #0; lock #0`).
The mutex is held out of the build — a lock still held if HWR resets a task
mid-run is an unknown risk, for no measured benefit (comment in event / a.end()).

## Next: the real lockup_TA is a per-frame TA-accept miss

EVENT_STATUS2 0x80 = TRIG_TA (b7), the TA twin of the TRIG_3D (b6) that flagged
the 0xa74 bug: on a failed frame the TA trigger is pending and not accepted — the
0x200 landed but the TA did not take it. The DDK baseline never raises either
bit. Working hypothesis (ta3d-state): the lockup is the DEFERRED kick 2, launched
from ta_finished(1) microseconds after the TA-finish event, before the TA/MTE
side is idle; the DDK's emit->worker launch gives a natural delay. Next step
(needs a box slot): a k_ta_defer sentinel to correlate deferral with the lockup,
then wait for TA idle (CLKGATESTATUS TE/MTE/VDM -> 0, or EVENT_STATUS2
MTE_STATE_FLUSHED) before the re-dispatched 0x200.
