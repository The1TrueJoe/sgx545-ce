# TA-finish event never acknowledged: the microkernel re-triggered ta_finished forever (EA1, fixed)

A chain sentinel on the EA1 located why the open microkernel's first TA/3D frame
stalled (prior entry: `2026-11-02-2150-ukernel-ea1-first-frame`), and the fix
let the frame advance one whole stage — from "stuck at kick 1" to "kick 2's TA
runs and hangs".

## The sentinel

Four env-gated markers (`UK_SENTINEL=1`, reverted before commit) each wrote a
constant to a free control-block scratch word that lands on the kernel's
`SGXDumpDebugReg` `T3C-D0/E0` lines: post-`CR_TA_KICK` (0xC01C @ +0xDC),
`ta_finished` entry (0x7AF1 @ +0xE0), `do_3d` entry (0x3DED @ +0xE4),
`render_done` entry (0x4D04 @ +0xE8). Cold boot (both `S60sgx` and
`S95ohc-display` held off), `srvinit -o /root/uk.bin`, one `frame`.

## What the markers showed (before the fix)

`data/2026-11-03-0020-ea1-sentinel-ta-finish.dmesg.log`:

```
(T3C-D0) 0x3200AB10 0x00000000 0x00000000 0x0000C01C   <- do_ta reached CR_TA_KICK
(T3C-E0) 0x00007AF1 0x00000000 0x00000000 0x00000000   <- ta_finished ran; do_3d/render_done absent
EUR_CR_EVENT_STATUS: 00002000  (ta_finish: yes)        <- STILL SET at every lockup dump
lockup_ta: no
```

So the earlier "idle VDM" reading was wrong: the TA **ran and finished** (the HW
raised ta_finish, 0x2000), and `ta_finished` dispatched and ran. The break was
that **the ta_finish event was never acknowledged**, so it re-triggered the
event program forever — monopolising it so kick 2's host-kick was never
processed, `do_3d` was never reached, the render sat in flight, and the "0x2
tasks" watchdog stormed. PB-pages-in-use (0) was a red herring: the DDK writes
`pb+0x14 = 0` at TA finish too (`ddk-ref-frame-registers.txt` line 193).

The dispatcher's `dispatch(bit, label)` only tests the `EUR_CR_EVENT_STATUS`
snapshot and branches; it never clears the serviced bit (the DCU-invalidate
acknowledge beside it was the only ack). The DDK clears ta_finish in its handler
(`str 0x134 = 0x2000`, ddk-ref line 188) and the 3D-end event likewise
(`str 0x134 = 0x40000`, line 473).

## The fix

In the event dispatcher, after the DCU-invalidate acknowledge and before the
`dispatch()` calls, acknowledge the TA-finish and both 3D-end bits in
`EUR_CR_EVENT_STATUS` via `EUR_CR_EVENT_HOST_CLEAR` (0x134):

```
limm r3, #(0x2000 | 0x40000 | 0x800000)
str  #(0x134 >> 2), r3
```

Clearing an unset bit is a no-op, and the dispatches test the r2 snapshot taken
above, so the branch is unaffected. Offline oracles stay green (test_emu 5/5,
test_commands 5/5, run_ours fake/v1/v2/xfer/xfer-rect all MATCH).

## What the fix did on HW (verification run)

`data/2026-11-03-0025-ea1-tafinish-ack-kick2-lockup.dmesg.log`, same cold-boot
recipe with the fixed microkernel:

```
EUR_CR_EVENT_STATUS:  00000000  (ta_finish: no)        <- ACKED, the storm is gone
EUR_CR_EVENT_STATUS2: 00000080  (lockup_ta: yes)       <- NEW: kick 2's TA hangs
EUR_CR_BIF_INT_STAT:  00080000
(T3C-D0) ... 0x0000C01C   (T3C-E0) 0x00007AF1 ...       <- do_3d still absent
```

So the ta_finish acknowledge is **HW-confirmed correct**: EVENT_STATUS went from
stuck-`0x2000` to `0`, the re-trigger storm vanished, and execution advanced past
kick 1. It uncovered the next frontier — **kick 2's render-TA runs but locks up**
(`lockup_ta: yes`, STATUS2 `0x80`, `BIF_INT_STAT 0x80000`), so `do_3d` is still
not reached. That TA hang is a separate, deeper issue (the render kick's
geometry/VDM processing) for the next investigation, not an event-dispatch bug.
The 3D-end acknowledge (0x40000/0x800000) is included by analogy to the DDK and
is harmless now, but is **unverified** on HW because `render_done` never runs yet.

## Box / method

EA1, cold boot each run (plain `reboot`, no plug), both sgxinit boot scripts held
off for the srvinit window, restored + rebooted to normal after. `gpu.lock` taken
with `flock` (mesa-gallium handed the slot off and stood down; the flock file is
persistent, never `rm`ed). EA1 clock runs ~5m fast; times above are real UTC.
Box left healthy (0 lockups, DDK boot). Commit: the acknowledge only; sentinel
markers reverted.
