# Hardware recovery in process context

Follow-up to `2026-10-28-1230-hwr-sleep-in-timer.md`: the driver's two recovery
callers ran in softirq context and slept there. This round moves them to
workqueues. The same test kernel also carries srvinit's DEVINITPART2 dispatch
fix. One CI kernel, booted once from RAM on the EA3, tested both.

## The change (um-re 5d1ae00)

- **Kbuild:** `PVR_LINUX_TIMERS_USING_WORKQUEUES` and
  `PVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE`. Both paths were already in
  `osfunc.c`. The lockup watchdog (`SGXOSTimer`) and the MISR now run from
  workqueues, so `HWRecoveryResetSGX` and anything it calls may sleep.
- **osfunc.c:** the two queues come from `alloc_ordered_workqueue` instead of
  the legacy `create_workqueue`/`create_singlethread_workqueue`. One timer and
  one MISR need no parallelism, and ordered queues keep the DDK's
  single-thread semantics.
- **sgxinit.c:** after a failed `SGXInitialise`, `HWRecoveryResetSGX` returns
  without queueing work. The microkernel never came back, so the kernel CCB is
  not being read and is usually full (`WO 0x2F / RO 0x30` in the first
  incident). Queueing could only wait out `MAX_HW_TIME_US` for a slot. The
  watchdog tries again on its next tick.

On sgx545-ce's old layout this is `user-va-64bit` d2b3187, together with the
DEVINITPART2 fix (c00e56f). openHC test branch `sgx-user-va-64bit` 59ea8a4,
CI run 40694900336. The decompressed kernel contains both queue names
(`pvr_timer`, `pvr_workqueue`), and dmesg shows `Installing MISR` instead of a
tasklet.

## srvinit with the 8-byte DEVINITPART2 OUT

The open `srvinit` ran first after the boot, as its test script expects:

- `srvinit rc=0`, microkernel started;
- **no `invalid param size for IOCTL#99` line**: the 8-byte OUT went through
  first time, so the 16-byte fallback never ran;
- replay identical to the reference, `gles2tri` green, replay identical again.

Data: `data/2026-10-28-2210-hwr-workqueues-srvinit.txt`.

## A deliberately wedged GPU

`wedge.sh` ran four replays. The first was a control. The second was the PDS
replay that sets bit 26 of the triangle's vertex `doutu` (0x3040d848:
`030403e5` → `070403e5`), which locks up the TA. Then the control twice more,
with 15 s pauses.

| Time | What happened |
|---|---|
| 88.4 s | control replay: back buffer matches |
| 88.9 s | wedge replay kicks; the TA locks up |
| 89.1 s | the microkernel flags HWR (`timer_irq: yes`, `lockup_ta: yes`). The MISR, now a work item, runs `HWRecoveryResetSGX`: **recovery succeeds** (no `SGXInitialise` error) |
| 94.5 s | wedge replay returns rc 0 after `SGXCleanupRequest … failed`, back buffer garbage |
| 109.8 s | control replay: its kick never completes (`write ops 0/1 … timed out`) |
| 110.1 s | `SGXOSTimer() detected SGX lockup`, watchdog recovery. From here every attempt ends `Wait for uKernel initialisation failed` → `SGXInitialise failed (24)`, about one per 1.2 s (29 by 143 s, 42 by 158 s) |
| 134–143 s | control replay again: times out the same way, returns rc 0 after about 9 s |

What changed against the softirq build:

- **No `scheduling while atomic`, no `BUG:`**, in 42 recovery attempts.
- **No task in D state.** Every client returned within its 30 s watchdog,
  and none needed killing.
- **Reboot works.** `reboot` took the box down in 13 s and it was back in 42 s.
  The softirq build took about 10 minutes to get past the stuck `glretrace`.
- The recovery loop runs in a `kworker` at about 25 % of one CPU. Each attempt
  busy-polls up to `MAX_HW_TIME_US` (500 ms) in `PollForValueKM`, which uses
  `udelay`, then the watchdog fires again. The rest of the box stayed
  responsive (load average 0.7).

So the GPU now **fails cleanly**, but it does not **recover** from this wedge.
The first recovery worked. What broke the second time is open. The likely
suspect is the wedge replay's render context: its cleanup request failed at
94.5 s, so it is probably still registered with the microkernel, and the next
TA kick runs into the same bad state. After that, the microkernel never
finishes initialising again.

Data: `data/2026-10-28-2210-hwr-workqueues-wedge.txt` (the run) and
`data/2026-10-28-2210-hwr-workqueues-dmesg.txt` (dmesg 89–113 s).

## Open

- Why the reset after the second lockup leaves the microkernel unable to
  initialise (`ui32InitStatus` never reaches `INIT_COMPLETE`). Next step:
  dump the host control block and the EDM registers around a failing
  `SGXInitialise`, and compare against the successful one at 89 s.
- Whether the context of a client that died mid-wedge should be torn down
  during recovery. The DDK leaves that to `SGXCleanupRequest`, which needs a
  running microkernel.
- A failed recovery could back off: retrying every 1.2 s spends one core
  busy-waiting. A counter that stops after N failures and logs once would cost
  nothing.
