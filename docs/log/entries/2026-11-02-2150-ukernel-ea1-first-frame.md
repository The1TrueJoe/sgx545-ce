# First TA/3D frame on the open microkernel (EA1): do_ta runs, the render never reaches the 3D

First-ever TA/3D client against openHC's open microkernel (every prior frame on
any box used the DDK microkernel). Cold boot, `srvinit -o /root/uk.bin` (our
microkernel), then one `tools/gen/frame -o bb.bin` (a plain clear + single
triangle, 736×480). Real UTC ~21:53 (EA1 clock runs +5m11s).

## Result

- `srvinit -o`: rc=0, "microkernel started" — init + the host GETMISCINFO kick
  still work (as confirmed earlier).
- `frame`: **`render TIMED OUT`** (rc=2). The back buffer `bb.bin` came back
  **entirely zero** (one distinct pixel value, `00000000`, across all
  736×480) — the 3D never ran: no clear, no triangle.
- **Not a hard wedge.** The SGX dump decode shows `lockup_ta: no, lockup_3d: no,
  partial_deadlock: no`; the kernel CCB is drained (`WO == RO`). The GPU fell
  into the same recoverable "0x2 tasks" `SGXOSTimer` watchdog storm as the
  pre-fix idle case — because a render was in flight (so the idle ACTIVE_POWER
  signal correctly did not fire), the GPU stayed powered, and the watchdog
  tripped on the stalled render. A plain `reboot` fully recovered the box
  (PVR 1.0.14, webview up, 0 lockups) — no power cycle needed.

## How far it got

The control-block scratch in the dump shows `do_ta` ran and programmed the TA:
`SC_CTX / SC_CMD / SC_RD / SC_PB` (T3C-C0) and `SC_RTENTRY` (T3C-D0) are all set
(`0x30002000 / 0x3200F000 / 0x3200E560 / 0x3200E000 / 0x3200AB10`). (`SC_INFLIGHT`
reads 0 in the dump, but that is post-recovery: HWR re-runs init, whose recovery
path discards the render in flight.) So the TA command was accepted and
programmed; what did **not** happen is a completed render — the all-zero buffer
means the **3D phase never executed**.

## The break: TA → ta_finished → do_3d

`do_ta` programs and kicks the TA (`CR_TA_KICK`), then the microkernel waits for
the **TA-finished** event (`EUR_CR_EVENT_STATUS` bit 0x2000) to run `ta_finished`
→ `do_3d` → kick the 3D → `render_done`. The 3D never ran, so the chain broke at
or before `ta_finished`: either the TA kick did not actually start the TA, or the
TA ran but its finish event never reached the dispatch (so `do_3d` never
started). `run_ours` matches the DDK on this path offline for all 5 traces, so
the register *programming* is right; the gap is live TA-kick / event-delivery on
hardware — the same class of thing as the host-kick (`EUR_CR_EVENT_KICK` vs
SW_EVENT) and dcu_invalid discoveries: an event/kick-delivery detail the offline
emulator doesn't model.

## Next (offline, then an EA1 slot)

Diagnose the TA→3D handoff with sentinels (debug build): did `do_ta` reach the
`CR_TA_KICK` write; does the TA-finished event (STATUS 0x2000) ever get set;
does the event program re-enter and reach `ta_finished` / `do_3d`. Compare the
TA-kick / event-arming the DDK does (0x200 `CR_TA_KICK`, the TA DPM/event enables
in srvinit's init script) against ours. Also check whether `frame`'s vertex
program carries the required epilogue phase (GEN.md must-know #1: a missing TA
epilogue hangs the TA). One frame only per slot; stop on any hard wedge.

## Box / method

Cold boot (held off S60sgx + S95ohc-display), `srvinit -o`, a single `frame`,
read the result before anything else (mesa-gallium's advice; the EA1 has no
plug). gpu.lock held the whole job; slot taken after mesa-gallium's dEQP batch
freed it (queue hwcheck → mesa → me). Box restored to its normal install,
mesa-gallium told it's free. Evidence:
`docs/log/data/2026-11-02-2150-ea1-first-frame.dmesg.log` (bb.bin was all-zero,
not kept).

## Follow-up (frame -v): the TA never runs — PB pages-in-use stays 0

Re-ran with `frame -v`, which reads the parameter-buffer descriptor's
"pages in use" (PB+0x14) after each kick (sgxgen prints it; the DDK capture
shows 0 before the first TA, 0x19 after):

```
sgxgen: after kick 1: PB pages in use 0, RT data +4 0, frame word 0x1
sgxgen: after kick 2: PB pages in use 0, RT data +4 0, frame word 0x1
frame: 720x480, 1 draw(s), render TIMED OUT
```

`frame`/`sgxgen` sends the correct two kicks (kick 1 TA-only `last=0`; kick 2 the
render `last=1`, cmd+0x04=0x62, cmd+0x1c=0x400 render bit, `bLastInScene`→READY),
so the client is right and the 3D-chain condition in `ta_finished` (READY bit)
would fire — this is **not** a `ta_finished`/`do_3d` problem. **PB pages-in-use
stays 0 after both kicks**, so the TA/VDM **never actually ran**: `do_ta`
programs the PB / DPM / TE / RT / RD registers and writes `CR_TA_KICK` (0x200),
but the geometry pipe never processes the parameter buffer. Hence no TA-finished
event, nothing chains, the render sits in flight, the "0x2 tasks" watchdog trips,
and the buffer stays all-zero.

So the break is **inside `do_ta`'s TA-start sequence**, not the event handoff.
Next: diff `do_ta`'s register writes against the DDK's own "TA kick 1" in
`docs/log/data/2026-10-28-ukernel/ddk-ref-frame-registers.txt` — what the DDK
writes to actually *start* the VDM/TA that `do_ta` omits or mis-orders (DPM
free-list load/kick, the VDM/TA control, or the kick value/register). `run_ours`
matches the DDK on struct-sourced registers but does not model the TA actually
running (it injects `ta_finished`), so a missing TA-start register is exactly the
kind of gap it can't see — the same class as the host-kick and dcu_invalid finds.
Box recovered by reboot (not a hard wedge; lockup_ta/3d: no).
