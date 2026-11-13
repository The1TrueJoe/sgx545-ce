# The open microkernel's bring-up on the dev boards (moved from UKERNEL.md)

UKERNEL.md describes the platform neutrally (CE5300, SGX545). The dated bring-up
narrative it carried is kept here, as it stood, with the dev-board names (CE5310 board, CE5300 board:
the two CE5300 boards used for bring-up).

## From §4 (Init)

**Hardware status (CE5310 board, 2026-11-02; supersedes the CE5300 board reading below).** The
open microkernel's clean-boot init path runs on hardware: after `srvinit -o`,
the advisory version word is at ctlblk+0x58 (`T3C-50` word 2 = `0x00010000`),
on both the CE5300 board and the CE5310 board. **Init is done; the host `GETMISCINFO` kick is not
serviced**, and the CE5310 board run with a proper entry counter shows the event program
*is* re-entered after init (CTR climbs, the post-init dispatch runs) — revising
the earlier CE5300 board guess that it was never re-entered. Two things were found and
one fixed:

1. **Fixed — `dcu_invalid` storm.** A data-cache-unit invalidate the kernel
   leaves pending in `EUR_CR_EVENT_STATUS2` (0x400) after `SysFinalise`
   re-triggered the event program forever, because the open microkernel read only
   `EUR_CR_EVENT_STATUS`. The dispatch now acknowledges it (§2). HW-confirmed:
   STATUS2 `0x400`→`0`.
2. **Fixed, HW-CONFIRMED — the host kick is serviced.** Two fixes: (a) the host
   kick is the `EUR_CR_EVENT_KICK` doorbell (no status bit, `sgxutils.c`), found
   by the kernel CCB write ≠ read offset, not `SW_EVENT` — dispatch now falls
   through to `kick`, the bogus SW_EVENT ack is removed (§2); (b) the kernel CCB
   base lives at control-block word **0x16** (+0x58), but `SA_KCCB` read word 16
   (+0x40) and `SC_VERSION` sat on 0x16 so `init` overwrote the base — so the kick
   read the command from garbage and ran a bogus `do_ta`. `SA_KCCB = 0x16`,
   `SC_VERSION = 0x18`. On the CE5310 board: `srvinit` returns rc=0 ("SGX545 microkernel
   started"), the `GETMISCINFO` DDK-version validate passes, Kernel CCB `WO == RO`
   (commands consumed). Offline: `test_emu` 5/5, `test_commands` 21/21, `run_ours`
   5/5 — all driven the hardware way.
3. **Fixed, HW-CONFIRMED — idle active-power.** After `srvinit` exited idle, a
   periodic `SGXOSTimer` lockup/HWR (~0.5 s) recurred because the host never
   powered the quiescent GPU down, so its watchdog kept sampling a halted EDM
   (`0xb14^0xb20` frozen). The microkernel must raise the active-power idle
   signal: `ui32InterruptFlags |= EDM_INTERRUPT_ACTIVE_POWER` (host-control word
   8, bit 1) + the host IRQ, which `SGXTestActivePowerEvent` turns into a
   power-down. the open microkernel now does this at `kick_end` when the CCB is empty and
   nothing is in flight. On the CE5310 board: **0 lockups / 0 HWR over ~60 s idle** (was
   6+ in 20 s); `srvinit` still rc=0. Later refinements: the same signal at the
   end of `render_done` (render-idle), and the DDK-style timer-based active-power
   with a latency.

Full evidence and next steps:
`docs/log/entries/2026-11-02-2100-ukernel-ce5310-kick-serviced.md` (current),
`docs/log/entries/2026-11-02-2045-ukernel-ce5310-host-kick.md` and
`docs/log/entries/2026-11-02-2015-ukernel-ce5310-dcu-invalid.md`, then
`docs/log/entries/2026-11-02-1000-ukernel-ce5300-init-confirmed.md` (the earlier
CE5300 board run, whose "never re-entered" reading the CE5310 board counter corrected).

## From §8 (Implementation map)

**First hardware run (CE5300 board, `srvinit -o`).** The microkernel loads, is kicked,
and executes, but init stalls: the event entry dies when it writes `sa` (`mov
sa1`) directly in the initial bare-`DOUTU` context, so InitStatus is never set
and `SGXInitialise` times out. `sa` is not a general "tasks can't write it"
limit — the DDK microkernel writes `sa` (e.g. `mov sa9`) and runs — but only
after it emits to trigger the pds1 DMA that establishes `sa`; the open microkernel wrote `sa`
before any such setup (`docs/log/entries/2026-11-02-0630-ukernel-ce5300-bringup.md`).
The emulator modeled `mov sa` as a plain register write, so the suite passed
offline. **Fix (option B, offline-validated):** drop `sa` entirely — carry the
control-block base in a reserved temp (`r17`) from the loader-patched `limm`
and derive the host-control / CCB pointers from it; `run_ours`/`test_emu` still
match the DDK. Its one hardware unknown is whether the bare-`DOUTU` context
allocates that temp, settled by a one-word sentinel on the next slot.

