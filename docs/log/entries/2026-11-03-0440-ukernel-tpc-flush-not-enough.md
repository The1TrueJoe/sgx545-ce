# The 3D hang is the missing 3D-start sequence (ta3d-state); TPC flush added but not sufficient alone

ta3d-state decoded why the 3D hangs and corrected the BANK0 theory: at the 3D the
BIF is unchanged (BANK0 = 0x0f same as the TA, DIR_LIST_BASE0 only read). do_3d
skips three DDK 3D-start steps (ddk-ref "TA finished (2)"):

- **A. TPC (tail-pointer cache) flush** — first thing on the last-in-scene TA
  finish: `CR_TPC (0x224) = 0x40000000`, wait `EUR_CR_EVENT_STATUS` bit 10 (0x400,
  TPC_FLUSH), clear. Without it the ISP walks UNTERMINATED region streams through
  valid PB pages and hangs with BIF_FAULT=0 — matches the symptom. (CR_TPC was
  defined in ukernel.py but never used.)
- **B. DPM allocated-page handover** — read CR 0x700/0x710/0x724/0x72c, write
  0x604/0x64c/0x660 and pb+0x18/0x1c/0x20/0x24; do_3d wrote only 0x600.
- **C. Background-object PDS words** — write the TA cmd+0x08/0x0c/0x10 words
  (0x04001800, 0x00030001, 0x0c00060e) into the client bgblock (BANK0 pulsed
  0->0x0f around it); the client leaves bgblock[0..2]=0, so the ISP's background
  object otherwise starts a PDS program from zeroed words.

## HW: A added, completes, still hangs

Added item A (TPC flush) in do_3d. HW run: the flush RUNS and COMPLETES (EVENT_
STATUS bit 10 consumed, `tpc_flush: no` / `tpc_clear: no` at the lockup), do_3d
reaches CR_3D_KICK, but the 3D still hangs — `lockup_3d: yes`, BIF_FAULT=0, render
TIMED OUT, buffer zero (identical markers: count 2, do_3d 0x3DED). So A is
necessary but NOT sufficient; B and C are still missing.

## Next

Implement B (DPM handover) and C (background PDS words) — getting the exact
bgblock-address derivation and B ordering from ta3d-state — then one CE5310 board run of
A+B+C together. Held uncommitted with A (commit the full 3D-start fix once it
renders). Data: `data/2026-11-03-0440-ce5310-tpcflush-do3d-still-3dhang.dmesg.log`.
Committed render-path fixes stand through f852218 (read-offset, bLastInScene).
