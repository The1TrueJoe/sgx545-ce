# Kick serialization works but does not change the 3D hang — the 3D execution is a separate frontier

HW test of the kick-serialisation fix on the EA1 (process kick 2 only after kick 1's
TA finishes, via a new SC_TAFLIGHT "TA in flight" flag + ta_finished pulling the
next kick).

## A bad scratch-word first (fixed)

SC_TAFLIGHT was placed at control-block word 0x17 (+0x5c) — but that word holds the
**page-directory address** srvinit writes (dump T3C-50 word 3 = 0x01D33000 = the
pd). So k_ta read non-zero and **deferred every kick, including kick 1**: do_ta
never ran, all T3C markers zero. Moved SC_TAFLIGHT to word 0x19 (+0x64, zero in the
dumps). do_ta runs again.

## Serialization runs, but the outcome is unchanged

With SC_TAFLIGHT=0x19: T3C-D0 count 2 / flag 0 (kick 2 reads 0x62), T3C-E0 do_3d
0x3DED reached — **identical** to the pre-serialisation bLastInScene run, and the
same hang: `lockup_3d: yes`, `lockup_ta: no`, BIF_FAULT 0, EVENT_STATUS2 0x40, no
OOM, render TIMED OUT, buffer zero. So the serialization fix (correct by DDK
analogy, kick1 -> finished -> kick2 -> finished -> 3D) **does not change the HW
outcome** — the earlier "premature do_3d" hypothesis is not the cause. The 3D hang
is independent of kick timing.

## Where this leaves it

Everything the microkernel does for a render now works on hardware: init, the host
kick, dcu_invalid, idle power-down, the TA (do_ta, read-offset, first-kick gate),
ta_finished (+ ack + bLastInScene), and do_3d is reached. The **3D itself hangs in
execution** (lockup_3d) with a clean BIF and DDK-matching registers (run_ours
validates the 3D phase). This is not a microkernel register value — it is the 3D
running on its input. Likely candidates, needing direction:
- the generated geometry / VDM stream / tile data from `tools/gen` (frame-from-
  scratch's area) — a malformed stream the TA accepts but the ISP hangs on;
- a 3D-execution detail run_ours cannot model because it injects the 3D phase
  (the TA's parameter-buffer / tile-list handoff to the 3D, a ZLS/ISP enable, or a
  DPM 3D-state the open do_3d omits).

M2's powervr_sgx driver renders through the DDK microkernel, so the DDK's 3D path
is a working reference to diff against.

## Status

Serialization fix HELD uncommitted (offline-validated but unproven on HW — it
changed nothing observable; run_ours doesn't exercise the defer). The committed
render-path fixes stand: ack (b6a65d3), first-kick gate (4aecd73), read-offset
(ded2c15), bLastInScene + emulator test (f852218). Evidence:
`data/2026-11-03-0415-ea1-serialization-markers-zero.dmesg.log` (the 0x17 bug) and
`-0420-ea1-serialization-do3d-still-3dhang.dmesg.log` (the fixed run).
