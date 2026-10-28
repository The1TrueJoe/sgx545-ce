# Kernel bug: hardware recovery sleeps inside a timer softirq

During a gl4es ctest on the EA3 (foobillardplus, ES1, through the DDK), the
SGX locked up and the driver's recovery could not bring it back. Every retry
logged `HWRecoveryResetSGX: SGXInitialise failed (24)`. At 2927.7 s the
kernel then reported:

```
BUG: scheduling while atomic: glretrace/14912/0x00000102
Call Trace:
 <IRQ>
 __schedule_bug
 schedule_timeout
 msleep
 SGXScheduleCCBCommand
 SGXScheduleCCBCommandKM
 SGXScheduleProcessQueuesKM
 HWRecoveryResetSGX.constprop.0
 OSTimerCallbackWrapper
 call_timer_fn
 run_timer_softirq
 handle_softirqs
 sysvec_apic_timer_interrupt
 </IRQ>
```

`glretrace` itself was only the task that the timer interrupt happened to
land on (it was in a compat ioctl, printing). It then sat in D state, holding
`/tmp/gpu.lock`. `reboot` took about 10 minutes to get past it.

## Why it happens

- `SGXOSTimer` (sgxinit.c) is the driver's lockup watchdog. It runs from
  `OSAddTimer`, and with `PVR_LINUX_TIMERS_USING_WORKQUEUES` unset in this
  build that is a plain kernel timer, so the callback runs in softirq context.
- When it sees a lockup it calls `HWRecoveryResetSGX`. That function
  re-initialises the SGX, then calls `SGXScheduleProcessQueuesKM` to restart
  the queues.
- That path ends in `SGXAcquireKernelCCBSlot` (sgxutils.c). It waits for a
  free kernel CCB slot with `OSSleepms(1)`, which is `msleep`.
- After the failed `SGXInitialise` the microkernel no longer consumes the
  kernel CCB, and the dump shows it full: `SGX Kernel CCB WO:0x2F RO:0x30`
  ((WO + 1) & 255 == RO). So the loop sleeps, inside the softirq.

The other caller, `SGX_MISRHandler` (when the microkernel itself flags HWR),
is no better. The Kbuild defines none of the DDK's workqueue options, so the
MISR is a tasklet, which is also softirq context. The timer is the path seen
here.

## Fix to try

Build with `PVR_LINUX_TIMERS_USING_WORKQUEUES` and
`PVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE`. Both options are already in osfunc.c
(`OSAddTimer` and the MISR on workqueues), and they put both recovery callers
in process context, where `msleep` is allowed. The alternative, busy-waiting in
`SGXAcquireKernelCCBSlot` when `in_atomic()`, would spin for up to
`MAX_HW_TIME_US` on a GPU that is already dead. This doesn't fix the reason
recovery fails (`SGXInitialise` 24); it only stops the recovery path from
corrupting the scheduler and hanging shutdown.

Not yet tried; no kernel change in this entry. Evidence: one full recovery
cycle and the BUG in `data/2026-10-28-1230-hwr-sleep-in-timer-dmesg.txt`. The
same failing recovery loop appeared again on the user-VA test kernel at
144 s (ctest neverball through the open driver), without the BUG line in
the part of the ring buffer that survived.

Follow-up: moved to workqueues in 5d1ae00 and tested on the EA3, see `2026-10-28-2210-hwr-workqueues.md`. No more atomic sleeps and the box reboots normally, but the GPU still does not recover from this wedge.
