# The residual lockup_TA is NOT the deferred kick — sentinel correlation refutes it (EA3)

ta3d-state's lead: the ~35% per-frame lockup_TA is kick 2, deferred while kick 1's
TA runs and re-launched from ta_finished(1) microseconds later, before the TA/MTE
engine is idle. Cheapest test first: correlate the `k_ta_defer` sentinel (0x0DEF)
with the lockup. Built UK_SENTINEL=1, pushed to the EA3, looped boots, read the
driver's watchdog scratch dump (T3C) on each timed-out frame.

## The sentinel map (UK_SENTINEL=1 scratch, dumped as T3C-xx lines, byte offsets)

- T3C-D0: [0xD0] ctlblk ptr | [0xD4] **do_ta entry count** | [0xD8] **last do_ta's
  cmd+0x04 bit0** (bFirstKickOrResume) | [0xDC] 0xC01C = do_ta ran
- T3C-E0: [0xE0] 0x7AF1 = **ta_finished ran** | [0xE4] 0x3DED = do_3d ran |
  [0xE8] 0x4D04 = render done | [0xEC] **0x0DEF = a kick was DEFERRED** (k_ta_defer)

## Result — 5 lockups + 1 render

| boot | 0xD4 count | 0xD8 flag | ta_finished | do_3d | **deferred** | result |
|------|-----------|-----------|-------------|-------|--------------|--------|
| 1 | 1 | 0 | 0x7AF1 | 0 | **0** | lockup_TA |
| 2 | 1 | 0 | 0x7AF1 | 0 | **0** | lockup_TA |
| 3 | 2 | 0 | 0x7AF1 | 0 | **0** | lockup_TA |
| 4 | 2 | 0 | 0x7AF1 | 0 | **0** | lockup_TA |
| 6 | 1 | 0 | 0x7AF1 | 0 | **0** | lockup_TA |
| 5 | — | — | (rendered; no watchdog dump) | | | RENDER |

All five lockups: EVENT_STATUS2 = 0x80 (TRIG_TA, b7), 0x2 tasks, BIF_FAULT 0.

## What it refutes and what it shows

- **Deferral is ruled out.** 0xEC = 0 on every lockup — `k_ta_defer` (SC_TAFLIGHT
  found set when a kick arrives) was NEVER taken. No kick overlaps another at
  dispatch. This also re-explains why the hardware task-mutex was HW-neutral: there
  is no second concurrent event task to serialise.
- **It is a race, not missing state.** Boot 5 rendered with the identical binary;
  a deterministic missing-register bug (like the 0xa74 one) would fail every frame.
- **The variance lives in the CCB consumption.** do_ta ran once (boots 1,2,6) or
  twice (3,4) for the same 2-kick frame.

## CORRECTION (ta3d-state, after this run): the 0xD8 flag column is a sentinel artifact

The table's "flag" column (0xD8 = word 0x36) is **SC_TAFLIGHT**, not cmd+0x04 bit0.
do_ta's DEBUG write to word 0x36 collides with the in-flight flag: do_ta sets
SC_TAFLIGHT=1 at the kick and ta_finished clears it to 0, so on every lockup (where
ta_finished ran) 0xD8 reads 0 regardless of the command. **The "do_ta consumed a
misaligned command (bit0=0, bit1=0)" conclusion has NO support and is withdrawn.**
What still holds: no deferral (0xEC=0), count 1 or 2 (0xD4 valid on lockups),
ta_finished ran, do_3d did not, and boot 5 rendered — so it is a race.

ta3d-state's DDK CCB model (emulated on base4): the DDK runs the TA path once per
command, twice per 2-kick frame, never consuming one kick. It advances the CLIENT
read offset at TA **finish** (RO += cmd+0x00 & 0xffff: 0->0x2c0->0x580), not at the
kick as ours does. At the kick it requires **cmd+0x58 bit0 = 1** (a ready flag the
kernel sets; sgxgen leaves it 0) and skips the command otherwise; it clears that bit
at TA finish. The kernel CCB is a doorbell+counter (per-context pending count at
ctx+0x14), never the queue. Both commands are 0x2c0 and written with a barrier before
the kick ioctl, so a half-written / wrong-size read is impossible — ruling out the
withdrawn misalignment theory.

## Next: a non-colliding CCB-trace sentinel to split the two failure modes

Re-run with UK_CCBTRACE words that don't collide with live state (0x35 count,
0x37 last do_ta client RO, 0x38 last cmd+0x04, 0x39 last cmd+0x58 ready flag,
0x3a/0x3b kernel WO/RO at kick_end). This separates:
- **count=1 + kernel WO != RO** -> a lost wakeup: kick 2's doorbell event merged
  with or ran ahead of ta_finished's `ba kick` re-dispatch. Fix = the DDK's
  per-context pending-kick counter (ctx+0x14) + advance client RO at TA finish.
- **count=2 + reads correct (0/0x71 then 0x2c0/0x62)** -> TA2 itself hangs with
  valid state -> a per-kick register diff against the DDK (the next 0xa74).
- **cmd+0x58 bit0 = 0 on a consumed command** -> we kicked before the kernel
  marked it ready (the DDK gates on this bit); fix = check it at the kick.

Evidence: `scratchpad/ea3-sentinel-defer.out`,
`docs/log/data/2026-11-03-1130-ea3-sentinel-defer.log`. ta3d-state's DDK model +
scripts (ccbtrace.py/ccbtrace2.py) in its round-2 handover.
