# Kick serialization fixes the CE5300 board TA race (lockup_TA -> do_3d reached); 3D hang remains

ta3d-state reproduced the box-dependent failure in the emulator (race.py): a kick
race. Our microkernel dispatched each host kick immediately, so when kick 2 arrives
while TA 1 is still running (a fast host, CE5300 board p1), it re-kicked the TA mid-run
(lockup_TA) and ta_finished acted on the latest-kicked command, not the one that
finished. The DDK defers a kick arriving while a TA is busy and re-launches it from
ta_finished.

Fix (ukernel.py): SC_TAFLIGHT flag (control-block word 0x36 — NOT 0x19/0x17, which
srvinit writes: a flag there defers the FIRST kick, do_ta never runs, all T3C markers
0, HW-confirmed), set in do_ta at the 0x200 kick, cleared + re-dispatched (`ba kick`)
in ta_finished when not bLastInScene; k_ta defers (leaves the kernel-CCB entry
unconsumed) while SC_TAFLIGHT is set, with the flight check BEFORE anything writes
SC_CMD. Offline: race.py now defers kick 2 and starts TA 2 (not the 3D) at TA
finished 1; run_ours 5/5, test_emu OK.

## HW (CE5300 board p1, uk-serial.bin md5 69b4323d): the race fix WORKS

srvinit -o rc=0, frame TIMED OUT, but the T3C dumps show the frame progressed where
it used to hang:
- T3C-D0 `...0x0000C01C` (do_ta), T3C-E0 `0x00007AF1 0x00003DED 0 0` -> **do_3d
  REACHED** (0x3DED). Before (uk-rdcopy, no serialization) the CE5300 board gave lockup_TA
  with do_3d absent.
- rgn1 word 0 POPULATED (`04000000 00001000 0 | 00020000 00001080 0 | ...`) -> the TA
  TERMINATED correctly, matching the DDK reference. The race fix lets kick 2's
  terminate TA finish.
- Now lockup_3d (isp_end_tile: no, pbe_end_render: no, BIF_FAULT 0, render_done
  absent) -- the SAME do_3d/3D-start hang as the CE5310 board. So the race was real and is
  fixed; a SEPARATE 3D-start bug remains.

## The DDK init writes (0x40/0xe80) are HARMFUL -- dropped

Tried ta3d-state's DDK init-event writes (0x0040=0x1ff, 0x0e80 1->0 pulse; perf
counters). HW result (uk-serial + init writes, md5 7df85440): REGRESSED to lockup_TA,
do_3d never reached (0 do_3d dumps across 69 do_ta dumps). ta3d-state expected them
harmless; they are not -- reverted. 0xacc EVENT_TIMER stays off too.

## Where it stands

Serialization is a proven, necessary fix (HW: CE5300 board lockup_TA -> do_3d + correct scene).
Both boxes now reach do_3d with a correct, matching scene (rgn1 == DDK) and matching
3D-start registers (ta3d-state kdiff), yet the ISP never walks tile 0 (lockup_3d,
clean BIF). The A/B proved the DDK microkernel renders the same frame on the same
stack, so the remaining gap is a do_3d/3D-start difference in ours that register- and
memory-diffs haven't caught. Next: ta3d-state's deeper do_3d analysis on this clean
repro. Code held uncommitted on the do_3d-investigation stack until it renders. Data:
data/2026-11-03-0734-ce5300-serial-do3d-reached.dmesg.log (+ initw-lockup-ta variant).
