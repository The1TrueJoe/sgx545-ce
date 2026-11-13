# EVENT_TIMER keeps the EDM alive: frame 1 goes from lockup to render (CE5300 board)

The terminate lockup_ta is the host watchdog (SGXOSTimer) HWRing the quiescent EDM
in the TA1->terminate gap: our EDM is purely event-driven, so between the two kicks
nothing runs, EDMTaskReg0 (0xb14) freezes, and after 30 ms (HWRecoveryFreq) the
watchdog runs HWRecoveryResetSGX -> SGXReset, which wipes the TA DPM; the terminate
then kicks into a wiped free-list and hangs (ta3d-state, from the DPMLOG dmesg).

## Fix

init enables EUR_CR_EVENT_TIMER (0xacc = hostctl[6] ui32uKernelTimerClock | ENABLE),
exactly as the DDK does at init/RESUME (pc 0x01f9), and the event dispatcher acks the
timer event (EVENT_STATUS bit 29, 0x20000000) so it does not storm. A periodic timer
task keeps the EDM running across the inter-kick gap, so the watchdog sees progress
and does not false-recover. srvinit's init script sets 0xacc=0; our init runs after
it (and after every HWR/power-up, which zeroes ui32InitStatus) and re-enables it.

## HW result (CE5300 board): frame 1 fixed

With the fix, frame 1 renders on every boot tried (3/3 in the single-frame diagnostic,
3/3 in the 10-frame run), whereas the baseline collected frame-1 lockups readily. So
the first-frame gap — the longest, which always died — is fixed, and the rendered
output is the correct triangle (md5 56f8466a). This is a real fix that matches the
DDK, committed now as an incremental milestone even though the aggregate is not yet
10/10 (14/30): later frames in a session still fail (~50%), a separate issue.

## Known follow-up (not in this commit)

ta3d-state's review: frames 2+ still HWR because the timer-only event currently falls
through to `ba kick` and scans the kernel CCB every tick, racing the doorbell task's
scan. The fix is to have the timer-only path ack bit 29 and END (never scan the CCB),
matching the DDK (its timer path only samples AP/lockup). Also k_power POWEROFF/IDLE
should set 0xacc=0 so SGXPrePowerState's clock-gating poll can complete. Those are the
next iteration, to be HW-proven on the CE5310 board (the CE5300 board is now audio-priority). The gated
UK_TMRLOG diagnostic (hostctl[6], 0xacc read-back, timer-tick counter, 0xb14^0xb20 at
init and the terminate) ships with this commit as tooling (default binary unchanged).
