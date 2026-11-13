# The terminate TA kicks with ZERO DPM pages; EVENT_STATUS2 is clean before the kick (CE5300 board)

UK_K2LOG logs six CRs at the TERMINATE kick's do_ta entry, just before the 0x200,
to answer ta3d-state's question: is the hang already pending (TA1's fault) or does
kick-2's own kick trigger it? Words: 0x35=0x118, 0x37=0x12c, 0x38=0x008,
0x39=0x724, 0x3a=0x72c, 0x3b=0x700.

## Result (boots where the terminate actually ran: lockup_ta 0x80)

| CR | value | note |
|------|------------|------|
| 0x118 EVENT_STATUS2 | **0x00000000** | TRIG_TA NOT pre-set before our 0x200 |
| 0x12c EVENT_STATUS | 0x80004000 | SW_EVENT (0x4000) + bit31; **no OOM** (0x2/0x4/0x8/0x02000000) |
| 0x008 CLKGATESTATUS | 0x000CCFA0 | partially clocked (healthy-3D 0xfffa7, idle 0x20) |
| 0x724 TA DPM pages | **0x00000000** | zero |
| 0x72c | **0x00000000** | zero |
| 0x700 | **0x00000000** | zero |

## Two findings

1. **The hang is kick-2's own kick, not a pending TA1 fault.** 0x118 = 0 before
   the 0x200; TRIG_TA (0x80) only appears afterward (the watchdog reads 0x80). Per
   ta3d-state's test, TA1 and its finish left no pending trigger — kick-2's
   programming/kick is what trips it. No OOM in EVENT_STATUS either.
2. **The terminate TA kicks with ZERO TA DPM pages.** 0x724/0x72c/0x700 are all 0
   at the terminate entry. save_ta_state reads 0x724>>16 and 0x72c>>16; the terminate
   continues on whatever TA1 left. If the DDK's TA1 leaves 0x724 nonzero (pages
   allocated) and ours leaves 0, the terminate TA has no parameter-buffer pages and
   starves -> lockup_ta. Our free-list load (dpm_freelist mode 3, 0x664/0x684) runs
   at kick-1 only; the lead is whether it actually populates 0x724, or TA1
   consumes/resets it before the terminate.

## Second failure mode

The other boots lock up at "0x4 tasks" with EVENT_STATUS2=0 and ALL K2LOG words 0
(incl. CLKGATESTATUS=0): the terminate's do_ta never ran (K2LOG is gated on the
terminate; count=1, as in the earlier CCB-trace). So some frames die before the
terminate even dispatches. Two modes: (a) terminate never runs (count=1), (b)
terminate runs with 0 DPM pages and hangs (count=2).

## Next

Handed to ta3d-state: what is 0x724 at the DDK's terminate kick in base4, and does
the DDK's TA1 / free-list load populate the TA page count in a way ours doesn't?
Then a fix to ensure the terminate has its DPM pages, or re-run the relevant
free-list/DPM step. Evidence: `scratchpad/ce5300-k2log.out`,
`docs/log/data/2026-11-03-1600-ce5300-k2log.log`. (Box handed to mesa for a 75-min
dEQP run meanwhile.)
