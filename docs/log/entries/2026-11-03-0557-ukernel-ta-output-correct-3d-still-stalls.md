# TA output is byte-correct (region headers match the DDK); the 3D still stalls at tile 0

Added the CR 0x274 save in ta_finished (ta3d-state kdiff: the DDK does `ldr 0x274;
st rd+0x124` at TA finish, our TA_RD restores 0x274<-rd+0x124 at the next kick but we
never saved it, so kick 2 restored the client's 0 instead of kick 1's 0x00020000).
Build uk-0x274.bin md5 0b10e040 (sentinel), offline test_emu/test_commands/run_ours
5/5 MATCH (the emulator reads 0x274 as 0, so it only bites on HW).

## HW: the 0x274 save alone does NOT fix the stall, BUT the TA output is now correct

cold boot, srvinit -o uk-0x274.bin rc=0, M2's dump-frame SGXGEN_DUMP_AFTER=1 ->
render TIMED OUT, 33 HWR. Same stall signature: T3C-D0 `...0x0000C01C`, T3C-E0
`0x00007AF1 0x00003DED 0x0 0x0` (do_ta/ta_finished/do_3d, render_done absent),
EVENT_STATUS 0 (isp_end_tile: no, pbe_end_render: no), EVENT_STATUS2 0x40
(lockup_3d: yes), BIF_FAULT 0.

**The decisive new datum (dump_after on timeout):** rgn1 (rd+0x20, the region array
the ISP walks) is POPULATED and matches M2's DDK reference byte for byte:
`04000000 00001000 0 | 00020000 00001080 0 | 00040000 00001400 0 | 00060000 00001480
0 | 00000002 00001100 0 | ...`. Word 0 carries the tile coords + first/last flag
(0x04000000), written by kick 2's terminate TA. So the TA pipeline is fully working
and tiles correctly; the bug is NOT the TA output. It is narrowly in how the 3D
consumes the (correct) tiled scene.

blitest (3D-only transfer) on the same boot did NOT lock up (0 HWR vs the frame's 33)
but rc=1 and never reached a transfer — inconclusive, it ran on the frame-wedged GPU.
Repeat it from a clean cold boot.

## Next: the do_ta cmd->rd copy block (ta3d-state item 1)

ta3d-state's kdiff (every microkernel memory store, ours vs the DDK on base4.bin,
scratchpad kdiff.py) found the DDK runs a routine at pc 0x07b1-0x07fb on EVERY TA
kick (in do_ta, called from 0x0851/0x0aae) that copies TA-command words into the
render details, which OURS skips entirely:
  rd+0xd4 = cmd+0x1c; rd+0xe4/0xe8/0xec/0xf0 = cmd+0x74/0x78/0x7c/0x80; rd+0xe0 =
  cmd+0x3c; rd+0x130/0x134/0x138 = cmd+0x08/0x0c/0x10; rd+0x140 = cmd+0x20 (and
  ta3d+0xa4); rd+0x168 = cmd+0x84 then n*16 B from cmd+0x88; rd+0x120 = cmd+0x48,
  rd+0x118/0x11c = cmd+0x4c/0x50; rd+0x144 = cmd+0x60 then n 8-B pairs from cmd+0x218;
  deferred updates (cmd+0x54 = n pairs at cmd+0x2a8, *addr = value); if (cmd+0x1c &
  0x200) rd+0xfc/0x100 = cmd+0x110/0x114.
rd is microkernel-internal (HW never reads it); these feed do_3d's programming and the
completion path. Implement the block in do_ta. The 0x274 save stays (DDK-matching,
correct). Data: data/2026-11-03-0557-ea1-0x274-ta-output-correct.dump.log.
