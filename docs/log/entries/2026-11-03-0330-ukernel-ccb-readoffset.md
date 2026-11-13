# The kick-2 TA hang: do_ta never advanced the client CCB read offset, so every kick re-read kick 1 (CE5310 board)

The do_ta entry-counter sentinel settled the kick-2 TA hang on the CE5310 board. This is
the root cause behind the whole "kick 2 hangs / do_3d never runs" chain.

## The sentinel result

A counter at do_ta entry (scratch +0xD4) plus this kick's first-kick flag (+0xD8),
both on the kernel T3C-D0 dump line. Cold boot, `srvinit -o`, one `frame`:

```
(T3C-D0) 0x3200AB10  0x00000002  0x00000001  0x0000C01C
                     ^count=2     ^flag=1     ^post-kick
```

Identical across all 70+ watchdog dumps. So **do_ta ran exactly twice** (kick 2
*does* reach do_ta) **but the last do_ta read a first-kick (0x71) command**
(flag=1), not kick 2's render command (0x62, flag 0). The microkernel re-processes
kick 1's command on the second kick; kick 2's 0x62 terminate is never seen. Hence
two overlapping stream kicks → `lockup_ta`, and `bLastInScene`/READY never seen →
`do_3d` never chained. This also explains why the first-kick gate (4aecd73) had no
HW effect: kick 2 always read a 0x71 command, so it always took the first-kick
path regardless of the gate.

## The bug

do_ta finds the TA command at the **client CCB read offset** (client CCB control
word 1, bytes) and computes `command base = client CCB base + read offset`. The
client advances the **write** offset per kick (`sgxgen.c`: `ccbctl.map[0] =
ccb_woff`, `ccb_woff += 0x2c0`); the **microkernel** must advance the **read**
offset as it consumes each command. do_ta never did — so the read offset stayed 0
and every kick re-read the command at offset 0 (kick 1's 0x71). The kernel passes
`ui32CCBOffset` per kick but only uses it kernel-side to fill the shared area
(`sgxkick.c`); it is not in the scheduled MKIF command, so the microkernel's own
read-offset bookkeeping is the mechanism.

`run_ours` missed this because it sets each kick's command-base offset externally
from the trace (another emulator blind spot, like the earlier TA2 gap) — the
microkernel's read-offset advance is never exercised offline.

## The fix (offline-validated; HW pending)

In do_ta, right after computing the command base, advance the client CCB read
offset by the command size (`cmd+0x00`, 0x2c0 bytes) and write it back to client
CCB control word 1, so the next kick reads its own command. do_ta has already
taken the command's pointers and the TA reads the separate VDM stream, not the
command, so freeing the slot then is safe. (Single frame: no wrap; a multi-frame
CCB needs a mask by its size.) Offline: `test_emu`, `test_commands`, `run_ours`
5/5 MATCH (the advance is a client-memory write, not a core register, so the
register oracle is unaffected).

Not committed yet — verifying on the next CE5310 board slot with the counter sentinel
(expect count 2 / flag 0 => kick 2 reads 0x62; then the terminate TA finishes,
ta_finished sees READY, do_3d runs, the triangle renders — or the next layer, e.g.
kick 1/kick 2 TA overlap if the two do_ta calls aren't serialized). Evidence:
`data/2026-11-03-0325-ce5310-counter-kick2-rereads-kick1.dmesg.log`.
