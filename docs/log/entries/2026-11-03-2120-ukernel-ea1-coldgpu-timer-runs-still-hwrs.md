# EA1 cold-GPU reached; EVENT_TIMER runs correctly but the EA1 still HWRs (5/10)

First microkernel run on the EA1 (10.0.0.110). The EA1's fat kernel SysFinalises the
GPU at boot via a web of services (S60sgx SrvInit + the WPE webview's EGL, launched by
webd past the init guards and respawned), so service-guarding under ohc.ramboot was
whack-a-mole. The clean route: run the test from an EARLY boot script (S50zuktest,
before S60sgx), under ohc.ramboot, synchronously in rcS — at S50 nothing has
SysFinalised yet, so srvinit -o gets a COLD GPU regardless of the display stack.

## Cold GPU — WORKS

`srvinit -o uk-timer.bin` → **"SGX545 microkernel started"** (not 129). First frame
renders, color buffer md5 **56f8466a** (the triangle). So the open microkernel runs
on the EA1, and the S50 early-boot RAM-boot recipe is a reusable cold-GPU rig for it.

## Complete fix (EVENT_TIMER + timer-end + k_power): 5/10, HWRs persist

10 frames: 5 "render done", 5 "render TIMED OUT"; **6 HWRecoveryResetSGX**. The
EVENT_TIMER fix is confirmed RUNNING on HW (uk-tmrlog T3C dump):
- hostctl[6] = **0x00061A80** (400000 = 400 MHz / 1 kHz) — the driver DOES populate the
  timer period; H1 (hostctl[6]=0) is REFUTED.
- 0xacc read-back = **0x010618CF** — ENABLE bit set + a live countdown value: the timer
  is enabled AND running.
- tick counter (0x38) = thousands (0x1D91 first, ~0x8BB between each HWR) — the timer
  IS firing; the EDM runs our timer task every period.
- 0xb14 (EDMTaskReg0, = the SGXOSTimer "(0xN tasks)" value 0x108c0/0x108be/…) IS moving.

So the timer works exactly as designed (enabled, firing, EDM task-register advancing) —
and the EA1 STILL HWRs ~50% + times out renders. **The EA1's render failure is NOT the
quiescent-EDM watchdog trip the timer fixes; it is a separate, real render hang** (the
EA1's historical lockup_3d, or another per-frame hang). EVENT_STATUS2=0 at each HWR
(not 0x80 TRIG_TA), consistent with a 3D/ISP hang, not the terminate-TA lockup.

## Bearing on the committed fix

9e48757 (EVENT_TIMER enable) stands — it is HW-proven to fix the frame-1 watchdog gap
on the EA3, and here it is confirmed to RUN correctly on HW. The complete fix (timer-end
+ k_power, uncommitted) also runs correctly but is NOT proven to improve the rate — on
the EA1 it does not, because the EA1's hang is a different bug. Held uncommitted.

## Next (ta3d-state)

Why does the render hang (~50%) on the EA1 with the EDM demonstrably alive (timer
ticking, 0xb14 moving)? EVENT_STATUS2=0 at the HWR points at a 3D/ISP-side hang, not
the terminate TA. The cold-GPU S50 rig is reusable for any follow-up EA1 diagnostic.
Data: `docs/log/data/2026-11-03-2120-ea1-coldgpu-tmrlog.log` (+ the 5/10 timer run
2026-11-03-2115). EA1 restored to normal (display up) afterwards.
