# The terminate's TA DPM pages are LIVE through TA1's finish, then wiped — not by power (CE5300 board)

UK_DPMLOG (commit 7af9d81) logs the TA free-list register 0x700 (readback of
0x61c = 0x04210000) at three kick-1 points, 0x724 (TA DPM page count) at TA1's
finish, 0x740 at kick-1 entry, and the hostctl[11] NumActivePowerEvents delta
(terminate − kick1). Ran on the CE5300 board clean p1; 5 lockups, the terminate-reaching
ones all identical.

## Result (T3C dump; byte offset → logged value)

| point | value | reading |
|-------|-------|---------|
| A  0x700 after free-list load (kick 1)   | **0x04210000** | free-list load TAKES |
| B  0x700 after dpm_load (kick 1)         | **0x04210000** | dpm_load PRESERVES it |
| C  0x700 at TA1's finish                 | **0x04210019** | still live at TA1 finish |
| C  0x724 at TA1's finish                 | **0x00190019** | **0x19 = 25 pages allocated, live** |
| 0x740 at kick-1 entry                    | **0x00000000** | DDK slot-1 free step is a no-op here |
| hostctl[11] delta (terminate − kick1)    | **0** (boots 1/5/6), **1** (boots 2/4) | mixed |

Known from K2LOG (33c353a): at the terminate's pre-kick, 0x700 / 0x724 / 0x72c are
all **0**, EVENT_STATUS2=0. So the state is live through TA1's finish and **0** by
the terminate kick.

## Conclusion — branch ④b (ta3d-state's tree)

1. Free-list load works (A live), dpm_load preserves it (B live) — ①② excluded.
2. TA1 allocates 25 pages and leaves them LIVE at its finish (C: 0x700=0x04210019,
   0x724=0x00190019) — TA1 does NOT close the scene, ③ excluded.
3. 0x740 = 0 — the DDK's kick-1 slot-1 free step (pc 0x0a1d-0x0a4a) is a no-op on
   this frame, ⑤ excluded.
4. **The wipe happens AFTER TA1's finish, in our ta_finished→terminate path, and is
   NOT caused by power-off**: the lockup_ta fires with the power-event delta = 0 on
   3 of 5 lockups (boots 1/5/6). The two boots with delta=1 (2/4) also wiped, so a
   power event is neither necessary nor the common factor — incidental idle AP from
   an HWR cycle, not the cause.

This closes the power hypothesis as the primary cause and fully explains the NULL
apfix result (2026-11-03 1730): clearing ACTIVE_POWER at do_ta couldn't help because
the pages are zeroed with no power event at all.

## Next — split the wipe location (follow-up log)

The gap is between TA1's finish (live) and the terminate's pre-kick (0). It is one
of: (b1) ta_finished(TA1)'s tail / the `ba kick` re-dispatch, or (b2) the terminate
do_ta's own programming (BANK0 / invalidate×2 / TA_TE / a74 RMW / TA_RD / cmd→rd /
0x218 — none writes a DPM reg, so b2 is unlikely a priori). Follow-up: log 0x724 at
the terminate do_ta ENTRY (before any terminate programming); if 0 there → b1
(ta_finished/re-dispatch), if live → walk the terminate's do_ta to the zeroing step.
Handed to ta3d-state (DDK firmware): with TA1 leaving 25 pages live and the DDK's
terminate also continuing on TA1's leftover state, what in our ta_finished→terminate
path frees the TA DPM pages that the DDK does not? Evidence:
`scratchpad/ce5300-dpmlog.out`, `docs/log/data/2026-11-03-1900-ce5300-dpmlog.log`.
