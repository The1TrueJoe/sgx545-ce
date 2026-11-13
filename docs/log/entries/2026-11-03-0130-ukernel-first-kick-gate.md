# do_ta re-ran the per-scene setup on every kick and hung the render TA (gated; offline-validated)

After the ta-finish acknowledge (`2026-11-03-0025-ukernel-ta-finish-ack`) let the
frame advance past kick 1, kick 2's render-TA ran but **hung**
(`lockup_ta: yes`, `do_3d` unreached). This is the cause, found entirely offline
from the kick-2-vs-DDK diff.

## The diff

`BIF_INT_STAT = 0x80000` decodes to `FLUSH_COMPLETE` (bit 19), not a fault
(`BIF_FAULT = 0`, `FAULT_TYPE = 0`) — the BIF is clean, so the TA hung
internally. Diffing the DDK's `TA kick 1` vs `TA kick 2`
(`ddk-ref-frame-registers.txt`): **kick 2 writes far fewer registers.** It skips
the whole per-scene setup — TA soft reset (`0x80`), PB descriptor
(`0x618/0x61c/0x620/0x624/0x628/0x638/0x648/0x668`), DPM free-list load
(`0x664/0x684`), DPM state/control load (`0x694/0x698`), OTPM (`0x24c`), RT region
registers (`0x62c/0x634/0x234`), client PD (`0xc84`) and INVALDC (`0xc00`) — and
does only a minimal kick: TE/MTE block, invalidate, BANK0, RD (`0x274`), flags
(`0x218`), kick (`0x200`).

STATE.md §"A single-triangle frame": **kick 1 is `bFirstKickOrResume`** (runs the
full VDM stream + setup), **kick 2 is `bLastInScene`** (terminate words only).
`sgxgen.c` sets `cmd+0x04 = last ? 0x62 : 0x71` with `bFirstKickOrResume` /
`bLastInScene` from `last` — so **cmd+0x04 bit 0 is the first-kick flag**.

the open microkernel's `do_ta` is one unconditional path: it re-ran *all* the setup on every
kick. Re-loading the DPM free list (which kick 1 already allocated PB pages from)
and re-resetting the TA mid-scene corrupts the in-flight scene and hangs the
render TA.

## Why the oracle missed it

`run_ours` mapped only `TA1 → "TA kick 1"` and `3D → "TA finished (2)"`; it never
compared our **TA2** (render kick) against the DDK's `"TA kick 2"`. Adding
`"TA2": "TA kick 2"` to `PHASE_MAP` immediately flagged **12 EXTRA registers** our
do_ta wrote on kick 2 that the DDK does not (`0x618…`, `0x62c/0x634/0x234`,
`0xc84`) — the bug, confirmed offline.

## The fix

Read the first-kick flag (`cmd+0x04` bit 0) into `r13` at the top of `do_ta`, and
gate each first-kick-only block on it (skip on a render kick): the TA soft reset,
client PD + INVALDC, PB descriptor + DPM free-list load, RT region registers, and
the DPM state/control load. The render kick now does only TE/MTE, invalidate,
BANK0, RD, flags, kick — matching the DDK. The proven first-kick sequence is
unchanged (the gates are branch-not-taken on the first kick).

## Validation

Offline: `test_emu` all OK, `test_commands` all OK, and `run_ours` **MATCH on all
5 traces including the new TA2 check** — our render kick's core-register writes
now equal the DDK's `"TA kick 2"` exactly (the 12 EXTRA writes are gone), while
`TA1` and `3D` stay matching.

**HW: NOT yet verified** — the CE5310 board GPU slot was not free (queued behind the M2
kernel driver's RAM-boot slot and mesa-gallium's piglit slices). Expected on HW:
kick 2's TA finishes (no `lockup_ta`), its `ta_finished` sees the READY bit and
chains `do_3d` → the first rendered triangle, or the next frontier if something
else remains. A debug build with the chain sentinel (ctlblk 0x37-0x3A → T3C-D0/E0)
is the probe when a slot opens.

## Commit

do_ta first-kick gate + the run_ours TA2 check, committed locally (parent pushes).
Offline-validated; HW verification pending an CE5310 board slot.
