# Read-offset fix works (kick 2 reads 0x62, TA no longer hangs); next bug: ta_finished's 3D trigger reads the command size

HW verification of the client-CCB read-offset fix on the EA1 (sentinel build),
same cold-boot + `srvinit -o` + `frame` recipe.

## The read-offset fix is confirmed

`T3C-D0 = 0x3200AB10  0x00000002  0x00000000  0x0000C01C`: count still 2, but the
last do_ta's first-kick flag is now **0** (was 1) — **kick 2 reads its own 0x62
render command**, not kick 1's 0x71. And the decode flipped: `lockup_ta: no` (was
yes), `EVENT_STATUS2 = 0`, `EVENT_STATUS = 0`. The overlapping-stream TA lockup is
**gone**. So advancing the client CCB read offset in do_ta is correct and HW-proven.

## Next bug: do_3d still not reached

The render still times out (buffer all-zero) and `do_3d` (marker 0x3DED) is still
absent though `ta_finished` (0x7AF1) ran. Cause: `ta_finished` decided whether to
chain `do_3d` by reading **command word 0** (cmd+0x00) bit 0 as a "READY" flag —
but cmd+0x00 is the **command size** (0x2c0, STATE.md §1 header), whose bit 0 is
always 0, so do_3d never ran. The comment calling word 0 `ui32CtrlFlags` was
wrong. run_ours injects the 3D phase, so it never exercised this branch (another
emulator blind spot, like the TA2 and read-offset gaps).

The real last-in-scene signal is the TA flags word **cmd+0x04 bit 1**: sgxgen
writes `last ? 0x62 : 0x71`, and bit 1 is set only on 0x62 (the bLastInScene /
render kick). Fix: `ta_finished` reads cmd+0x04 (word 1) and tests bit 1 (0x2)
instead of cmd+0x00 bit 0. Offline: test_emu, test_commands, run_ours 5/5 MATCH
(run_ours injects the 3D, so still green).

## Status

Both fixes (read-offset advance + ta_finished 3D-trigger) are in ukernel.py,
uncommitted, bundled for the next EA1 slot; staged sentinel binary `uk-ready.bin`
(md5 64e3862b). Expected next run: do_3d marker 0x3DED appears, the 3D runs,
render_done (0x4D04), the first triangle. Watch for a possible serialization race:
our kick handler services both kicks on their own doorbells, so SC_CMD may already
hold kick 2's command when kick 1's ta_finish fires — if do_3d then runs after
kick 1 (too early), the fix is to serialize kick 2 after kick 1's TA finishes
(process it from ta_finished rather than its own doorbell). Evidence:
`data/2026-11-03-0340-ea1-readoffset-kick2-reads-62-still-hangs.dmesg.log`.
Per the parent, commit all clean once it renders.
