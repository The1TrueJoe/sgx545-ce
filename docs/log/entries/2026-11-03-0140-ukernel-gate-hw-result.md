# The first-kick gate (4aecd73) matches the DDK but does NOT fix the kick-2 TA hang (CE5310 board)

HW verification of `4aecd73` (first-kick gate) on the CE5310 board, same cold-boot +
`srvinit -o` + `frame` recipe, sentinel+gate debug build (md5 ef10d655).

## Result: no change

`frame` still **render TIMED OUT**, back buffer all-zero, 8 lockups. The dump is
**byte-identical** to the pre-gate (ta-finish-ack-only, b6a65d3) run
(`data/2026-11-03-0025-*`): same markers `0xC01C` (do_ta reached CR_TA_KICK) +
`0x7AF1` (ta_finished), `do_3d` still absent; `EVENT_STATUS = 0` (ta_finish
acked), `EVENT_STATUS2 = 0x80` (`lockup_ta: yes`), `BIF_INT_STAT = 0x80000`
(FLUSH_COMPLETE, benign), same T3C-C0/D0/E0. Only HC-10 word 0 differs (a
counter) and the timestamps. Data: `data/2026-11-03-0100-ce5310-gate-still-kick2-lockup.dmesg.log`.

## What this means

The gate is a **correct, DDK-matching** change — `run_ours` proves our render
kick (TA2) now equals the DDK's "TA kick 2" exactly (the 12 extra writes are
gone) — but it is **not** the unblocker: the over-setup was not (the sole) cause
of the kick-2 TA hang. 4aecd73 stays (it is still right and closed a real oracle
blind spot), but its "...hangs the TA" causality is disproven on HW.

The byte-identical outcome is the clue: if the gate had changed what happens on
kick 2, *something* in the dump would move. It didn't. So either **kick 2 never
runs its do_ta**, or it runs in a way the per-kick register setup doesn't affect.

Relevant architecture (`ukernel.py`): the `kick` handler processes **one CCB
command per event-program entry** and ends at `kick_end`/`kick_idle_done`; it does
**not** drain the CCB, and `ta_finished` ends without checking the CCB for the
next kick. Nothing serializes kick 2 against kick 1's in-flight TA. So kick 2
relies on its own `EUR_CR_EVENT_KICK` doorbell producing a fresh entry — which, if
it arrives while the EDM is busy with kick 1's TA, could be missed (kick 2 never
runs) or could kick a second TA over the first (overlap → lockup). In the DDK
capture the order is kick 1 → **TA finished (1)** → kick 2, i.e. the DDK processes
kick 2 only after kick 1's TA finishes.

## Next (needs an CE5310 board slot)

A **do_ta entry-counter + path sentinel**: at do_ta entry increment a scratch word
(count), and write a path marker that differs for the first kick (r13=1) vs the
render kick (r13=0). After a run: count == 1 → kick 2 never ran; count == 2 with
the render marker → kick 2 ran the minimal path and the hang is after its kick
(the terminate stream, or a kick-1/kick-2 overlap). That tells us whether to look
at command sequencing (serialize kick 2 after kick 1's finish, or from
ta_finished) or at the generated terminate stream. Box left on normal boot, slot
handed to mesa-gallium.
