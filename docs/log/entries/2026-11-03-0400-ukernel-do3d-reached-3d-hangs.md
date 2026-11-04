# bLastInScene fix reaches do_3d on hardware; the 3D now hangs (serialisation: do_3d runs before the last TA finishes)

HW verification of the bLastInScene fix (ta_finished tests cmd+0x04 bit 1) on the
EA1. The whole front of the chain now runs; the frontier moved to the 3D itself.

## Result: do_3d reached, 3D hangs

`T3C-E0 = 0x00007AF1  0x00003DED  0x00000000  0x00000000`: **do_3d (marker 0x3DED)
is now reached** — ta_finished chains to it. So do_ta (read-offset fix) →
ta_finished (ta-finish ack) → do_3d (bLastInScene fix) all run. The decode flipped
again: `lockup_ta: no`, **`lockup_3d: yes`** (EVENT_STATUS2 0x40), BIF_FAULT 0,
BIF_BANK0 0xf, no OOM. T3C-C0 SC_CMD = 0x3200F2C0 (kick 2's command, read-offset
fix holding). The 3D is kicked but **hangs in execution**; render_done (0x4D04)
and the buffer stay empty.

run_ours validates do_3d's registers (the "3D" phase matches the DDK's
"TA finished (2)"), so this is not a wrong 3D register — it is the 3D executing on
the geometry, which run_ours injects and cannot model.

## Hypothesis: do_3d runs before the last TA finishes (no kick serialisation)

The DDK order is kick 1 → **TA finished (1)** → kick 2 → **TA finished (2)** → 3D:
each TA completes before the next, and the 3D runs only after the last TA. openHC
services each kick on its own EUR_CR_EVENT_KICK doorbell, so both do_ta calls run
back-to-back (counter: count 2). `SC_CMD` is overwritten by kick 2's do_ta, so when
**kick 1's** ta_finish fires, ta_finished reads **kick 2's** command, sees
bLastInScene, and runs do_3d — **before kick 2's terminate TA has finished**. The
3D then renders an unfinalised scene (the terminate hasn't closed the tile lists)
and hangs.

## Next (needs an EA1 slot)

Serialise the kicks: process kick 2 only after kick 1's TA finishes, and run do_3d
only after the **last** TA finishes — matching the DDK. Options to design: defer a
queued kick until ta_finished (don't kick the next TA while one is in flight,
using the in-flight slot the microkernel already tracks), or have ta_finished pull
the next kick from the CCB. A per-kick sentinel on which ta_finish fired vs SC_CMD
would confirm the premature-do_3d timing. Likely needs to understand how the DDK
microkernel serialises the two kicks (M2's driver renders through it) — ask the
main session / frame-from-scratch.

## Status

bLastInScene fix is HW-proven (do_3d reached) and offline-validated (run_ours
catches a wrong trigger). Held uncommitted per the parent's "commit once it
renders"; it didn't fully render (3D hangs), so the bLastInScene + run_ours fixes
stay bundled, pending the serialisation fix + a render. Read-offset fix already
committed (ded2c15). Evidence:
`data/2026-11-03-0400-ea1-blastinscene-do3d-reached-3d-no-complete.dmesg.log`.
