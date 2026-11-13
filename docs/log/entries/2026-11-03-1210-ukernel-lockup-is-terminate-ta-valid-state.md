# The residual lockup_TA is the kick-2 terminate TA hanging with fully-valid CCB state (CE5300 board)

Follow-up to the deferral refutation (b52b093/bf43365). ta3d-state corrected the
earlier sentinel (0xD8 was SC_TAFLIGHT, not cmd+0x04) and handed over the DDK CCB
model. Built UK_CCBTRACE (78d2dcf): non-colliding T3C words that record how the
client CCB is actually consumed. Ran it on the CE5300 board (5 lockups captured).

## Trace words (UK_CCBTRACE, T3C byte offsets)

- D0: [0xD4] do_ta count | [0xD8] SC_TAFLIGHT (ignore) | [0xDC] last do_ta client RO
- E0: [0xE0] last cmd+0x04 | [0xE4] last cmd+0x58 (kernel ready bit) | [0xE8] kernel
  WO | [0xEC] kernel RO (both read at kick_end)

## Result — every lockup identical

| boot | count | last RO | cmd+0x04 | cmd+0x58 | kWO | kRO |
|------|-------|---------|----------|----------|-----|-----|
| 1 | 2 | 0x2C0 | 0x62 | 0x01 | 0x09 | 0x09 |
| 3 | 2 | 0x2C0 | 0x62 | 0x01 | 0x0A | 0x0A |
| 4 | 2 | 0x2C0 | 0x62 | 0x01 | 0x09 | 0x09 |
| 5 | 2 | 0x2C0 | 0x62 | 0x01 | 0x09 | 0x09 |

(boot 2 dropped on a transient SSH auth failure, not a GPU event.) All:
EVENT_STATUS2 = 0x80 (TRIG_TA), lockup_ta yes / lockup_3d no, BIF_FAULT 0.

## What it rules out — all three candidates at once

- **Not a lost wakeup.** count = 2 (both kicks ran do_ta) and kernel WO == RO (the
  kernel CCB is fully drained, no kick left pending).
- **Not a misaligned / wrong read.** The second do_ta read at client RO = 0x2C0
  (kick 2's slot) and got cmd+0x04 = 0x62 — the terminate / bLastInScene command,
  exactly right. (This buries the withdrawn "misaligned read" theory for good.)
- **Not the kernel ready-flag race.** cmd+0x58 bit0 = 1 on the consumed terminate:
  the kernel HAD marked it ready before we kicked.

## Conclusion: the terminate (0x62) TA hangs with valid state

Both kicks are consumed correctly, the terminate is read at the right offset with
its ready bit set, and the TA still locks up (TRIG_TA) before do_3d. So the CCB
consumption is correct and the bug is in how do_ta programs / kicks the TA for the
0x62 terminate — a per-kick register or state the DDK sets that we don't. It is
intermittent (other runs render 1-in-6 to 6-in-10), so it is timing-sensitive
HW-only state, the TA twin of the 0xa74 bug: the offline emulator reads 0 for the
CR and so can't see it, which is why run_ours has matched the DDK value-for-value
on the TA kicks all along.

## Next (ta3d-state's domain — read the DDK terminate-kick path)

Find what the DDK's kick-2 (0x62, bLastInScene) TA programming does that ours does
not — especially an RMW that preserves a HW/CR value (like 0xa74 b27-31 = cmd+0x14,
or the 0x274 save/restore), or a TA-idle/flush/barrier between the two kicks the
DDK gets from its emit->worker launch. Candidate HW registers for a targeted
DDK-vs-ours trajectory poll next slot: 0x8 CLKGATESTATUS, 0x724/0x72c TA DPM,
0x274, 0xa74, 0x630, 0x618-0x628 DPM free-list, 0x12c EVENT_STATUS (OOM). Evidence:
`scratchpad/ce5300-ccbtrace.out`, `docs/log/data/2026-11-03-1210-ce5300-ccbtrace.log`.
