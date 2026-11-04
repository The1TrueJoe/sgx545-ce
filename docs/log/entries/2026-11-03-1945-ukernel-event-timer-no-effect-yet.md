# EVENT_TIMER enable did not stop the watchdog HWR yet — timer likely never ticks (EA3)

ta3d-state root-caused the terminate lockup as the host watchdog (SGXOSTimer) HWRing
the quiescent EDM in the TA1->terminate gap (SGXReset wipes the TA DPM). Fix: init
enables EUR_CR_EVENT_TIMER (0xacc = hostctl[6] ui32uKernelTimerClock | 0x01000000)
so a periodic task keeps the EDM moving. Built uk-timer.bin (md5 e8f8ff13), offline
green (test_emu, run_ours 5/5).

## HW result: NULL

3 boots x 10 frames: 5/4/5 = **14 / 30, vs the 13/30 baseline — unchanged.** The
"HWRecoveryResetSGX / Hardware Recovery triggered" dmesg lines are **still present**
(5-6 per boot). Rendered frames correct (triangle 56f8466a). So the enable did not
stop the watchdog HWRs. (Frame 1 rendered on all 3 boots, where baseline "always
died" — possibly the timer covering the first gap, but within noise at this rate.)

## Analysis — the root cause still holds; the timer just isn't ticking usefully

The HWRs persist, so ta3d-state's watchdog mechanism is confirmed, not refuted. The
fix did not take effect. Offset is correct: in SGXMKIF_HOST_CTL with SUPPORT_HW_RECOVERY
on and EDM_BREAKPOINTS/BRN_28889 off, ui32uKernelTimerClock is word 6 (the same layout
that puts InterruptFlags at 8, InterruptClearFlags at 9, NumActivePowerEvents at 11 —
all matching our constants). So the read is right.

Two live hypotheses:
- **H1 (leading): hostctl[6] = 0 in our srvinit -o path.** The driver sets it to
  CoreClockSpeed / uKernelFreq in SGXUpdateTimingInfo (sgxpower.c:125-128); if that
  path doesn't run (or the SGX timing info isn't provided) hostctl[6] stays 0, so our
  init writes 0xacc = 0x01000000 (ENABLE, VALUE field = 0 per sgx545defs VALUE_MASK
  0x00FFFFFF). A zero period does not give a useful sub-30 ms tick, so the EDM still
  goes quiescent and the watchdog still fires. Not a storm (14/30 renders, no hang),
  which fits "period 0 = effectively no tick".
- **H2: hostctl[6] nonzero, timer fires, but our fall-through timer event doesn't move
  0xb14.** The DDK's timer event has a real handler (AP countdown 0x04b0-0x04d6); ours
  acks the bit and falls through to `ba kick` -> empty CCB -> end. If EDMTaskReg0
  (0xb14) only advances for a dispatched TA/3D task, not a bare event-program pass, the
  watchdog still sees no progress.

## Decision

Fix HELD UNCOMMITTED (parent's criterion — 10/10 with no HWR — not met). Root cause
unchanged; the fix mechanism is right but ineffective as built. NEXT: a gated log of
hostctl[6], the computed 0xacc, and the 0xacc read-back at init settles H1 vs H2 in
one slot. If H1, write a real timer period (CoreClockSpeed/uKernelFreq, or a safe
sub-30 ms VALUE) instead of relying on hostctl[6]; if H2, give the timer event a real
handler that advances the task registers. Handed to ta3d-state (does our srvinit -o
path populate hostctl[6], and does a bare event-program pass move 0xb14, or is a real
timer handler needed). Evidence: `scratchpad/ea3-timer-mf.out`,
`docs/log/data/2026-11-03-1945-ea3-timer.log`. Box left clean p1, /boot/once removed.
