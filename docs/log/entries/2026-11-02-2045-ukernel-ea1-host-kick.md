# The host kick is the EUR_CR_EVENT_KICK doorbell, not SW_EVENT — dispatch corrected

Offline follow-up to the EA1 run (`2026-11-02-2015-ukernel-ea1-dcu-invalid.md`),
done while the EA1 was unavailable (a sibling session's D9 register-write test
locked its GPU into a reboot loop awaiting a power cycle). No hardware this
entry; the fix is offline-validated and HW confirmation is pending.

## The question left by the EA1 run

After the `dcu_invalid` fix, `GETMISCINFO` was still never serviced: the event
program ran and reached the post-init dispatch, but `EUR_CR_EVENT_STATUS &
0x4000` (SW_EVENT) was never set when it read it, so it never branched to
`kick`; the command sat in the kernel CCB (`WO` > `RO`) and the core timed out
into a HWR-retry loop. Why does the host kick not show as SW_EVENT?

## Answer, from the kernel source

`kernel/.../devices/sgx/sgxutils.c SGXScheduleCCBCommand` is how the host sends
a command to the microkernel:

1. `psSGXCommand->ui32ServiceAddress = aui32HostKickAddr[eCmdType]` — the
   command type, from the host-kick table.
2. `*pui32WriteOffset = (*pui32WriteOffset + 1) & 255` — advance the kernel CCB
   write offset.
3. `*pui32KernelCCBEventKicker = (… + 1) & 0xFF`; `OSWriteMemoryBarrier()`.
4. `OSWriteHWReg(…, EUR_CR_EVENT_KICK, EUR_CR_EVENT_KICK_NOW_MASK)` — ring the
   doorbell.

So the host→microkernel kick is the **`EUR_CR_EVENT_KICK` doorbell**. It runs
the event program but sets **no** `EUR_CR_EVENT_STATUS` bit. The microkernel
finds the waiting command by the kernel CCB **write offset ≠ read offset** — the
doorbell is only the trigger, the CCB offsets are the queue.

`SW_EVENT` (0x4000) in `EUR_CR_EVENT_STATUS` is the *opposite* direction: the
microkernel sets it (`raise_host_irq`) to raise the host interrupt, and the
host's ISR clears it (docs/UKERNEL.md §2, confirmed by `test_emu`
`raise_host_irq`). the open microkernel's dispatch gated the host kick on this bit, so the
kick could never fire — matching the EA1 dump exactly (`EVENT_STATUS = 0`,
`sw_event: no`, while the command sat unconsumed).

## Fix

- **Dispatch:** drop the `SW_EVENT`-gated kick branch. After the hardware-event
  bits (TA_FINISHED, PIXELBE_END_RENDER, ISP_END_TILE), fall through to `kick`
  unconditionally; `kick` already reads the kernel CCB write/read offsets and
  returns via `kick_end` if they are equal (nothing waiting).
- **`kick` handler:** remove the SW_EVENT acknowledge added earlier (it was
  based on the wrong "kick == SW_EVENT" premise; worse, it could clear the
  microkernel→host SW_EVENT the host ISR owns). No ack is needed — the
  `EUR_CR_EVENT_KICK` doorbell is edge-triggered, not a latched status bit.
- The `dcu_invalid` STATUS2 acknowledge from the previous entry stays.

## Offline validation (the harnesses now model the kick the hardware way)

`run_ours` and `test_commands` previously drove our kick by setting
`EUR_CR_EVENT_STATUS = SW_EVENT` *and* the CCB `WO`≠`RO` — so they never caught
this. Both now set `EVENT_STATUS = 0` and rely on `WO`≠`RO`, i.e. exactly what
the `EUR_CR_EVENT_KICK` doorbell leaves. With that:

- `test_commands`: get-misc-info **21/21 words match the DDK**; power, cleanup,
  context-suspend all pass — every host command dispatched via `WO`≠`RO`.
- `run_ours`: all 5 traces (fake, v1, v2, xfer, xfer-rect) **MATCH** — TA and
  transfer kicks dispatched via `WO`≠`RO`.
- `test_emu`: 5/5 (init, recovery, raise_host_irq, render teardown, transfer).

So the command handlers are byte-for-byte unchanged against the DDK under the
corrected, hardware-faithful trigger. What offline cannot show is the doorbell
itself (the emulator models neither `EUR_CR_EVENT_KICK` nor event re-entry), so
**a serviced GETMISCINFO on the EA1 is the pending confirmation**: expect the
event program to reach `kick`, read the CCB, run `k_getmiscinfo`, set the misc
buffer READY and advance `RO`, with no lockup.

## Status

Committed to `um-re`. This is the fix for the second EA1 blocker; with init
(option B) and now both post-init issues addressed, the next EA1 slot should get
past `GETMISCINFO` to the first real work. If the kick is serviced, the `TODO`
acknowledges in `ta_finished`/`render_done` (§2) are the next thing to validate
when frames run.
