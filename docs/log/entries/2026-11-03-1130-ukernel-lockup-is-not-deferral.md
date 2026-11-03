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
  twice (3,4) for the same 2-kick frame, and the last command it consumed had
  cmd+0x04 bit0 = 0. bLastInScene is bit **1**, not bit0; a command with bit0=0 is
  0x62 (bit1=1) OR a misaligned read (bit1 also 0). In count=1, ta_finished ran but
  do_3d did not, so that single command was NOT seen as bLastInScene -> do_ta
  consumed a wrong/misaligned command from the client CCB (read-offset vs the
  client's write-offset update is racy), kicked a TA with inconsistent state, and
  the TA hung (TRIG_TA). count=2 = both kicks consumed but the 0x62 terminate TA
  still hung.

## Next

Offline (ta3d-state): model the client CCB read-offset advance against sgxgen's
write-offset update and the DDK's own consumption — does the DDK ever run do_ta
once for a 2-kick frame, and what orders the read-offset advance against the
next kick so a command is never half-written when consumed. The fix is likely a
read-offset/alignment guard, not a TA-idle wait. Evidence:
`scratchpad/ea3-sentinel-defer.out`.
