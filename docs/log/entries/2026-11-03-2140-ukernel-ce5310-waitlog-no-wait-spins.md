# CE5310 board WAIT_LOG: no self.wait() spins — the EDM freezes in a hardware stall, mixed TA/3D

ta3d-state's hypothesis was that a render hangs silently, then a self.wait() poll loop
spins forever and freezes the EDM. UK_WAITLOG (uk-waitlog.bin 7bf41aec) named the live
wait-site (0x35), the furthest render stage (0x3a, reset first-kick-only), and per-tick
EVENT_STATUS/STATUS2/CLKGATESTATUS. Run on the CE5310 board cold GPU (S50 rig), 4/10 rendered,
6 HWRs.

## Result — the wait-spin hypothesis is REFUTED

T3C (D4=waitsite, DC=EVENT_STATUS, E0=EVENT_STATUS2, E4=CLKGATESTATUS, E8=stage,
EC=ticks; D0=SC_RTENTRY):

| HWR | waitsite | stage | SC_RTENTRY | EVENT_STATUS | EVENT_STATUS2 | CLKGATE | ticks |
|-----|----------|-------|-----------|--------------|---------------|---------|-------|
| 1-4 | **0** | **4 render_done** | 0x3200AB10 (in flight) | 0x1 | 0 | 0x000CCFA0 | 0x129..0x1EE1 |
| 5-6 | **0** | **2 ta_finished** | 0 (none) | 0x0 | 0 | 0x000CCFA0 | 0x279E,0x305C |

- **waitsite = 0 at ALL 6 HWRs**: no self.wait() is spinning when the EDM freezes. The
  hang is NOT in one of our poll loops.
- **Mixed stage**: 4 HWRs at render_done (3D-completion side, render in flight), 2 at
  ta_finished (TA-finish side, no render in flight — the CE5300 board terminate-TA signature).
- ticks increase monotonically across HWRs, so the timer fires and the EDM runs between
  HWRs; but 0xb14 (the watchdog "(0x108c2 tasks)" value) is FROZEN at each HWR — the EDM
  runs fine, then freezes >=30 ms, repeatedly. CLKGATESTATUS 0xCCFA0 = EDM on USSE, TA
  geometry units off; EVENT_STATUS2 = 0 (no TRIG_TA/OOM) throughout.

## Conclusion

The EDM freezes WITHOUT being in our wait — a hardware operation the EDM issues and
blocks on, for >=30 ms, on BOTH the 3D-completion path (render_done reached) and the
TA-finish path. This is a real GPU/render hang, not a microkernel poll-loop bug, and
it is the CE5310 board's own failure (the committed EVENT_TIMER fix 9e48757 is confirmed running
here; the complete fix does not help because the render hangs regardless). Handed to
ta3d-state: what hardware op in render_done (dst-sync writes / cache ops / SW_EVENT) or
the ta_finished->terminate re-dispatch would stall the EDM 30 ms+ with EVENT_STATUS2=0
and no self.wait? The S50 cold-GPU rig + uk-waitlog are reusable for the follow-up.
Data: docs/log/data/2026-11-03-2132-ce5310-waitlog.log. CE5310 board restored to normal.
