# Full 3D-start sequence (A+B+C+D) implemented, offline-green, still lockup_3d on HW

Implemented all of ta3d-state's DDK 3D-start spec in do_3d: A (TPC flush), B (DPM
save_ta_state + load_3d_state: CR 0x700/0x710<<16/0x724>>16/0x72c>>16 -> pb+0x18..
0x24, then 0x600=(pb+0x10)&~0xf / 0x604=pb+0x18 / 0x64c=pb+0x1c / 0x660=pb+0x20|
(pb+0x24<<16)), C (background PDS words: bgblock=*(rtentry+0x10), cmd+0x08/cmd+0x0c
and *(*(rd+0x114)+4*rd[0x108]), BANK0 pulse 0x0f->0x00->0x0f with idf/wdf fence),
D (0xa74|=rd+0x13c, 0x658=0, 0x43c=1). Offline: test_emu/test_commands/run_ours
5/5 MATCH; build ~8KB, shl/shr are valid libusc ops (asm.py/hw.py accept them).

## HW: unchanged — still lockup_3d

Same as before: do_3d (0x3DED) reached, render_done (0x4D04) never, buffer zero,
`lockup_3d: yes`, `lockup_ta: no`, BIF_FAULT 0, EVENT_STATUS2 0x40. The dump is
indistinguishable from the pre-A runs (A TPC flush completes but the register dump
doesn't expose do_3d's 0x604/0x660/bgblock writes, so HW cannot confirm B/C/D
landed correctly).

## Why run_ours can't catch a B/C/D error

run_ours validates only STRUCT-sourced registers and injects the 3D phase; the new
writes are plumbing (CR_TPC, BANK0, 0x658, 0x43c) or memory (the 3 bgblock words,
pb+0x18..0x24), none checked against the DDK. So a wrong value, offset, operand
order or address in B/C/D passes offline and hangs on HW — the same blind-spot
class as the TA2 / read-offset / 3D-trigger gaps before it.

## Next: ta3d-state emulator diff

The micro-architectural debugging is beyond register-dump inference. ta3d-state
runs the DDK microkernel in tools/ukernel/emu with breakpoints; asked it to run the
open microkernel's do_3d through the same frame and diff the actual 3D-start
writes (values + addresses) against the DDK, to find the wrong/missing one. Data:
`data/2026-11-03-0455-ce5310-3dstart-abcd-still-3dhang.dmesg.log`. Committed render
path stands through f852218; A+B+C+D held uncommitted.
