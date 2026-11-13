# Cache ops (op0xff / op0x100) do NOT fix the terminate-TA hang; frame 1 always fails (CE5300 board)

ta3d-state's firmware diff (candidate 1) said the DDK runs USE/EDM cache ops that
ours doesn't — op0xff after every MADD invalidate and op0x100 in the shared
task-end tail — and that missing them could be the intermittent terminate-kick
lockup_TA. Tested both on the CE5300 board, each against a same-method baseline control.

## Method

Per binary: 3 cold boots, each `srvinit -o <uk>` then 10 consecutive frame-dumps,
count renders (fair before/after; no "find a rendering boot first" bias).

## Result

| build | boots | total |
|-------|-------|-------|
| baseline (no cache op, d664d9f1) | 4/10, 3/10, 6/10 | **13/30** |
| op0xff #0 (TA-side invalidate, 61f390b4) | 4/10, 5/10, 1/10 | 10/30 |
| op0x100 #3 (ta_finished non-last tail, 4e5a64d2) | 4/10, 3/10, 3/10 | 10/30 |

Both cache ops are **no better than baseline** — marginally worse, within the
boot-to-boot variance. Offline both stayed green (test_emu, run_ours 5/5 MATCH;
the ops are sim no-ops). So the missing cache ops are NOT the terminate-kick cause.
Reverted both (kept only breadcrumb comments).

## The real signal: frame 1 ALWAYS fails

Across all 9 boots (3 binaries x 3 boots), the FIRST frame after cold `srvinit -o`
never rendered — every boot's first frame-dump was a lockup_TA. Only LATER frames
(2-10), i.e. after the watchdog HWR has reset the GPU, ever rendered (and then only
~40% of them). So the terminate TA hangs deterministically on the first render of a
cold-srvinit GPU, and a HWR reset is what lets a later render succeed.

This reframes the hunt: the bug is not a missing per-kick op (cache ops ruled out)
but **state that cold `srvinit` init leaves wrong for the terminate TA and that the
driver's HWR reset fixes**. Candidates: (a) something sgxinit/HWR programs that our
init handshake doesn't (the DDK's 0x40=0x1ff / 0xe80 pulse were tried and regressed,
but there may be other init state); (b) a per-scene TA register that is correct only
after a reset; (c) ta3d-state's secondary 0x630 RMW (clear bits 0+2 at each TA kick;
ours clears 0-2 only in do_3d) — not yet tested, cheap to fold in.

Open question for the driver: does the watchdog HWR reload OUR microkernel, or does
it reset to the driver's own fw? If HWR swaps in a different microkernel, the "later
frames render" would be a different code path — must confirm before trusting the
post-HWR renders as ours. (The M1 first-triangle and multi-frame results do show
ours renders, so ours is capable; the question is only what the post-HWR renders in
THIS run were.)

Evidence: `scratchpad/ce5300-op0xff-mf.out`, `ce5300-op100-mf.out`, `ce5300-baseline-mf.out`.
ta3d-state's firmware detail: ta3d-state-round2-handover.md, kseq5.txt.
