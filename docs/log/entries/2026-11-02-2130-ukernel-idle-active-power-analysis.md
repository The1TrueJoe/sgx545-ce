# Idle "0x2 tasks" HWR: root cause is the missing active-power idle signal (offline analysis)

Offline analysis of the residual from `2026-11-02-2100-ukernel-ea1-kick-serviced.md`
(no hardware this entry; the EA1 is mesa-gallium's). Root cause found; a concrete
fix is proposed, with one design choice for the next step.

## What SGXOSTimer actually checks

`sgxinit.c SGXOSTimer()` (the host watchdog, runs off the host timer):

- If the device is **powered down** (`SGXIsDevicePowered` false) it resets its
  counter and does nothing — **no lockup check while powered down.**
- Otherwise it reads `reg(ui32EDMTaskReg0) ^ reg(ui32EDMTaskReg1)` — on the EA1
  `srvinit` sets these to **0xb14 / 0xb20** (`srvinit.c:542`). If that value is
  **unchanged for 3 consecutive ticks** (and no reset happened in between), it
  declares `SGX lockup (0x%x tasks)` and triggers HWR. "0x2 tasks" is just that
  frozen value.

openHC's microkernel is purely event-driven: each event program run ends
(`nop [end]`) and the EDM halts until the next event. Idle with no client, it
runs nothing, so 0xb14/0xb20 never change → every 3 ticks → HWR. (Host commands
still drain — the APM keeps poking it — so `WO == RO` keeps advancing even as the
HWRs recur.)

## Why this is normally masked, and why it bit the test

The watchdog is skipped when the GPU is powered down. In real use the GPU is
either **busy** (a client — webview/Minecraft — kicks it, so the EDM runs and
0xb14/0xb20 change) or **powered down** by active-power management. My test ran
`srvinit -o` and then nothing — no client, and (the bug) **no power-down** — so
the GPU sat powered and idle, the one state the watchdog punishes.

## The missing piece: the active-power idle signal

`sgxutils.c SGXTestActivePowerEvent()` powers the GPU **off** only when:

```
(hostctl.ui32InterruptFlags      & PVRSRV_USSE_EDM_INTERRUPT_ACTIVE_POWER) != 0 &&
(hostctl.ui32InterruptClearFlags & PVRSRV_USSE_EDM_INTERRUPT_ACTIVE_POWER) == 0
```

`PVRSRV_USSE_EDM_INTERRUPT_ACTIVE_POWER = (1<<1) = 0x2` (`sgx_mkif_km.h:230`).
`ui32InterruptFlags` is **host-control word 8** (ClearFlags is word 9 =
openHC's `HC_INTCLEAR`, so no `BRN_28889` shift). So the **microkernel** tells
the host "I'm idle, power me down" by setting bit 1 of host-control word 8 and
raising the host IRQ; the host's MISR then calls `SGXTestActivePowerEvent`,
powers off, and the watchdog goes quiet. **openHC never sets this flag**, so the
host never powers down the idle GPU. That is the whole bug.

(The DDK drives this from its TIMER-event handler using
`ui32ActivePowManSampleRate` / `ui32ActivePowManLatencyms` — raise ACTIVE_POWER
after the GPU has been idle for the configured latency. openHC implements no
TIMER handler and no active-power at all; docs/UKERNEL.md §4 already flagged the
timer/active-power path as "read structurally but not implemented".)

## Proposed fix

When the microkernel drains the kernel CCB and has no work left, raise the
idle signal. Concretely, at `kick_end` (reached after `kick_adv` advances the
read offset, or when the dispatch found the CCB already empty): if the CCB is
now empty (write offset == read offset) **and** nothing is in flight
(`SC_INFLIGHT == 0` and `SC_XFER == 0`), set `hostctl[8] |=
EDM_INTERRUPT_ACTIVE_POWER (0x2)` and raise the host IRQ (write `SW_EVENT` to
`EUR_CR_EVENT_STATUS`, as `raise_host_irq` does). It is idempotent: once the
host acts it sets its ClearFlags bit, so re-raising does nothing until a resume
clears it — no storm. New constants: `HC_INTFLAGS = 8`,
`EDM_INTERRUPT_ACTIVE_POWER = 2`.

This covers the observed residual (idle after a GETMISCINFO with no render). It
does **not** need the TIMER event, because the idle transition happens inside an
event-program run (the one that drains the last command). Renders-then-idle
would also want the same signal at the end of `render_done`; add that when
frames run.

### The design choice (for the next step)

- **A — minimal, queue-drain signal (above):** raise ACTIVE_POWER the moment the
  microkernel goes idle. Simple, no timer, powers down eagerly. Might power-cycle
  more than the DDK between bursts of work.
- **B — DDK-style timer-based active-power:** enable the TIMER event, count idle
  time against `ui32ActivePowManSampleRate`, raise ACTIVE_POWER after the
  latency. Matches the DDK's behaviour and avoids eager power-cycling, but is
  more code (a TIMER handler + idle counter) and enabling the timer event in
  `srvinit`.

A is enough to clear the HWR and is the natural first step; B is the "right"
long-term behaviour. Recommend landing A, HW-confirming the idle GPU powers down
(no more `0x2 tasks` HWR, `SGXIsDevicePowered` goes false), then deciding on B.

## Validatability

The offline emulator models neither the active-power MISR nor power-down, so it
can only check that the idle path writes `hostctl[8] |= 0x2` and raises
`SW_EVENT` — the actual power-down and the silenced watchdog are HW-only. So
this fix wants an EA1 slot to confirm, coordinated with mesa-gallium under the
gpu.lock rules.
