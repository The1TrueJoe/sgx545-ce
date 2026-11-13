# SGX530 board: 8-word PDS confirmed correct on silicon, render still fails — a second blocker

**2026-11-13 ~09:00 UTC. Our OMAP3630 test board, kernel 7.3.3. Under lock. Clean, recovered.**

Re-run after the 530 PDS 8-word interleave fix (6c1cae05), with the client-side PDS dump.

## The 8-word fix is correct and reached the board frame

`run.log` (stock image, edm_init=1, SGXGEN_PDS_8WORD=1 SGXGEN_DUMP_PDS=1):

```
sgxgen: PDS data interleave: 8-word (SGX530)
pds @ 0x0e001180 rows=4 8word: 00002000 00000000 0016000e 00000100 0001040a 00000000 ...
```

- The env reached the board frame (`8-word (SGX530)` printed; pds_2dwords = 0).
- The uploaded data segments are genuinely 8-word: the state-emit program's `ds0[4] =
  0001040a` sits at **word 4** — the 530's read position — not word 8. (The `0001` high
  bits are the board's heap base vs frame-fake's `0000`; the low `040a` matches.) This is
  exactly `pds_interleave_check.py`'s 8word column, now on real silicon.

So the DOUTU launch sources are at the right words on the 530. The ds0[4]=0 gate-2 failure
(the confirmed cause of the earlier no-render) is fixed on hardware.

## But the render still fails identically

```
frame 0: render FAILED, centre 00000000 (want ff00ff00), corner 00000000 (want ff000080)
job timed out (ccb 3, consumed 2); resetting
microkernel started again        # HWR
```

Same symptom as before the fix: both kicks consumed, no completion, black frame, HWR.

## Conclusion: the PDS interleave was necessary but not sufficient — a second 530 blocker

The offline go/no-go (9/9) validated the PDS *launch resolution*, and the board confirms the
8-word data is correct and uploaded. Yet nothing renders. So there is a **second 530 difference
downstream of the PDS launch** that the offline trace cannot see: the TA still emits no tiles,
or a later stage (USSE task execution, DOUTD vertex-fetch data, ISP/3D program, a state word)
fails. Candidates for the ukernel/ISA side to rank.

## Next: the render debugfs

The region headers / RT data / sync words are kernel-owned, so the client can't tell whether
the TA now emits tiles with the correct 8-word PDS. The coordinator greenlit a read-only
`CONFIG_DEBUG_FS` dump of the last render's RT dataset (`pvrsgx_rt_dataset`: rtdata, rtword,
entword, rgn[2], tailptr, sync, ccbctl) — one board run then says whether the TA ran. That,
plus the ukernel session's call on the most likely second difference, is the next step.

## Board

Clean throughout; HWR recovered the GPU each timeout, driver unloaded, lock released, left on
its normal boot. No power cycle needed.
