# Clearing ACTIVE_POWER at every TA kick does NOT fix the terminate lockup (CE5300 board)

ta3d-state root-caused the residual lockup_ta as a broken ACTIVE_POWER handshake:
ours raises hostctl[8] bit1 (EDM_INTERRUPT_ACTIVE_POWER=2) when idle but never
clears it, so the host MISR can power the core off mid-scene (between TA1 and the
terminate), the terminate powers back up via soft reset and loses its TA DPM pages,
and kicks with 0x724/0x72c/0x700=0 -> lockup_ta. Proposed fix 2: do_ta withdraws
[8] bit1 at the start of every kick.

## Fix built and HW-tested

uk-apfix.bin (md5 08b363e5c8e585e13bc78bfc65939c55). do_ta entry reads hostctl[8],
ANDs out bit1, writes it back (idf/wdf). Offline clean: test_emu "all OK",
run_ours fake/v1/v2/xfer/xfer-rect all MATCH (the clear is a hostctl memory write,
not a core register, so the oracle is unaffected).

## Result: NULL

| boot | renders | first-frame md5 |
|------|---------|-----------------|
| 1 | 4/10 | 56f8466a (triangle, correct) |
| 2 | 5/10 | 56f8466a (triangle, correct) |
| 3 | 5/10 | 14ca6477 (stale — frame 1 did not render) |

**14/30 vs the 13/30 baseline — within noise.** The fix did not move the lockup
rate. Rendered frames are still bit-correct (triangle 56f8466a).

## Conclusion

Clearing [8] bit1 at do_ta entry is NOT the fix. Either:
1. the ACTIVE_POWER power-off is not the lockup mechanism, or
2. the clear is ineffective — the host may latch [9] bit1 and power off before our
   do_ta entry runs, or the clear races an in-flight power-off.

The two K2LOG failure modes persist unchanged: (a) terminate never dispatches
(count=1, "0x4 tasks"), (b) terminate runs with 0 DPM pages (0x724/0x72c/0x700=0,
EVENT_STATUS2=0 before our 0x200) and hangs. The fix is held UNCOMMITTED (not in
git); nothing to revert.

## Next

Handed to ta3d-state. The hypothesis hinges on one unverified fact: does the DDK's
TA1 leave 0x724 nonzero (pages allocated) where ours leaves 0? If the free-list
load at kick-1 (dpm_freelist mode 3, 0x664/0x684) never populates 0x724, that is
upstream of any power issue -> fix 3 (robust POWEROFF save of 0x700/0x710/0x724/
0x72c into pb w6-9; TA_PB loading 0x638 = w8 | w9<<16, not w8 only). Box handed to
mesa for a full dEQP run; no HW until it frees. Evidence:
`scratchpad/ce5300-apfix-mf.out`, `docs/log/data/2026-11-03-1730-ce5300-apfix.log`.
