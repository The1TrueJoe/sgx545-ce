# SGX530: the register-file restructure (attributes in, outputs for scratch)

A proposal, not yet code. The hinge it rested on is now **confirmed** (attempt 41: output
registers are task-writable), so the restructure is viable. This gives the design, a rough
size, the microkernel paths it touches, and the constraints that still need a measurement or a
cheap probe before code. Send-back requested before code.

## 1. What is established (C, stage-1 attempts 24-41)

- The open microkernel's USSE code **executes** on the SGX530 (code base `>> 8`, `phas.i`
  first). `ld.a.d` addresses and faults; an unconditional `ba` self-loops; the source operand
  decodes correctly.
- **No write to a temporary retires.** `limm`, an ALU `or`, `str`, an `ld` destination into
  `rN` — all leave the register a later read sees unchanged (stable power-on leftovers).
- **PDS-set primary attributes are task-readable and persist (attempt 40).** `DOUTA` → PA0,
  read back through `ld.a.d r3, pa0`, held across resets/unload/reload.
- **Output registers are task-writable (attempt 41, the hinge).** `or o0, r5, r17` then
  `ld.a.d r3, o0` faulted at 0x0fd7d000 = r5 ⊕ r17 leftovers OR'd — `o0` took the write, the
  dependent `ld` read it back. First task-side register write to retire on the SGX530.

So the SGX530's USSE, in the microkernel's task context, has a **writable output bank and a
read-only attribute bank, but a temporary bank that drops writes.** Why temporaries differ is
still open (the EDM-task temp allocation is suspected; the AM335x capture would show it), but
the microkernel no longer needs that answer to run — it has a file to compute in.

## 2. The restructure, in two parts

Both are **per-core, SGX530 only.** The SGX545 keeps `limm`-patched temporaries and its
current PDS programs; its golden stays byte-identical (§5).

**(A) Pointers and constants move from `limm`-patched temporaries to PDS-set attributes.**
Today the event entry holds pointers in temporaries set by loader-patched `limm` (UKERNEL.md):
the control-block base and the PD table. `limm` does not write on the SGX530, so these never
load — the root of the first fault. Move them to primary attributes the PDS sets with `DOUTA`:

- PA0 = control-block base (replaces the r17 `limm` patch). It is referenced ~190 times and is
  live for the whole task, so sourcing it from an attribute also spares a scratch register
  that would otherwise be pinned to it for the entire task (§4).
- PA1 = PD table base (replaces the r14 `limm` patch), single-context.
- PA2+ = the small constants the entry stores (0, 1, version 0x00010000, DPM clear values),
  so `st.a.d PAn → memory` writes a constant with no ALU.

The driver's PDS builder (`pvrsgx_pds_entry`) sets the DOUTA rows; attempt 40 proved its
DOUTA-first `pds3` writes attributes correctly, so attribute setup is **not** blocked on the
probe's multi-DOUTA ordering bug (§6).

**(B) Scratch moves from temporaries to output registers.** Every `rN` the SGX530 microkernel
writes becomes an output `oN`. Loads land in outputs (`ld.a.d oN, …`), ALU results go to
outputs, stores read from outputs. This is the part that makes the microkernel able to keep
computed state at all — the heartbeat `ld; imae; st`, dispatch `ld; test; (p0) ba`, address
arithmetic.

## 3. Rough size

**Not a mechanical rename, because of register pressure.** The microkernel writes ~20 distinct
temporaries (r0-r17, r22, r23; the hot ones are r5, r4, r17, r2, r3, r6), across ~8 paths. The
output bank's architectural ceiling is **~11** (the SGX530's VS-output count, **U**,
unconfirmed), and only `o0` is tested usable. So the mapping is bounded by **peak register
pressure** (how many temporaries are simultaneously live at the busiest instruction), not by
the count of distinct names:

- If peak live ≤ usable outputs → a per-core register **reallocation**: a 530 renaming applied
  in `ukernel.py`'s emit layer, plus the §2(A) attribute change in the driver. Contained — it
  changes the register convention, not each path's logic by hand. Estimate: the emit-layer
  remap + the PDS/attribute change + per-core gating. Moderate.
- If peak live > usable outputs → add **memory spill** (store/reload the overflow to a scratch
  BO). Larger and slower: each high-pressure region gets load/store scaffolding, and the
  heartbeat/dispatch hot paths want to stay in registers. This turns a convention change into
  a register-allocation pass.

Two measurements decide which, both cheap:
1. **Usable output count** — one board slot: a probe that writes `o0..o10` and reads each back
   (same shape as the o0 probe). Tells us the real ceiling, not the doc's unconfirmed 11.
2. **Peak register pressure** — offline, no board: a liveness pass over the assembled
   microkernel paths. If it comes in under the ceiling from (1), the restructure is the
   contained reallocation.

## 4. Which microkernel paths change

All of them write temporaries, so **all** change — it is a global convention change, not a
local patch. The paths (UKERNEL.md; labels in `ukernel.py`):

- **event entry + dispatch** (`event`, `tmr_tick_*`, `not_timer_evt`) — heartbeat, EVENT_STATUS dispatch.
- **init** (`init`, `init_set`).
- **host kick** (`kick`, `cc_*`, `k_ta`, `k_getmiscinfo`, `k_power*`, `k_ctxsuspend`, `k_cleanup`).
- **OOM / transfer** (`k_oom*`, `k_transfer`, `kick_adv`, `kick_end`).
- **TA** (`do_ta` and its logging/BIF/PB/RT sub-labels).
- **TA finished** (`ta_finished`, `ta_oom_*`, `ta_status_*`).
- **3D** (`do_3d`, `skip_*`).
- render-done and the not-yet-reproduced paths (teardown, cleanup, power) when they land.

Because it is per-core, the change is expressed once as the SGX530 register convention, not
rewritten per path. The persistent control-block base moving to PA0 (§2A) removes the single
most-pinned live value from the output budget, which is what makes the pressure in (3) more
likely to fit.

## 5. What still needs confirming before code

- **Usable output count and peak pressure** (§3) — the two measurements that pick reallocation
  vs spill. Do these first; the whole size depends on them.
- **Emit-at-end semantics.** Outputs are normally emitted to the output buffer when a task
  ends. The o0 probe wrote and read an output but then self-looped — it never ended, so emit
  behavior is untested. If the microkernel's EDM-style task emits outputs on completion, using
  them as scratch could write scratch to memory. Confirm the task either emits nothing or emits
  somewhere harmless (the EDM task has no real output buffer), or the restructure needs the
  emit suppressed.
- **`st.a.d` reading an output as its source** (§2A constants path) — the o0 test read an
  output as an `ld` base; a store reading an output as its data source is the same file but
  untested. Fold into the usable-output probe.

## 6. The two-row / one-row DOUTU-zero, and why it does not block §2(A)

The coordinator asked why the multi-DOUTA PDS program issued its DOUTU with word 0 ≈ 0 under
`0xa70` = 6 and 8. Attempt 41's one-row DOUTA hit the **same** b2 fault at code base 0 as the
two-row form, so it is **not** the second data row's interleave (the earlier guess). With a
DOUTA before it, the DOUTU issues with word 0 ≈ 0 in both layouts; the same DOUTU words with no
preceding DOUTA run. The fault is the **DOUTA-before-DOUTU ordering in the probe's hand-built
PDS** — the DOUTA consumes or rewrites what the following MOVS DOUTU reads, or that PDS needs
the DOUTU issued first.

This does **not** block the restructure's attribute setup, because the driver's real `pds3`
puts its DOUTAs first and sets PA0 correctly (attempt 40). The probe's PDS construction differs
from the driver's; only the probe needs its ordering fixed, and only to build multi-attribute
*probes*. The restructure's attributes come from the driver's PDS builder, which works.

## 7. SGX545 impact: none

Per-core throughout. The SGX545 keeps temporaries and its PDS programs; `limm` writes there.
The SGX530 build selects the attribute + output convention by a per-core flag. The SGX545
golden stays byte-identical.

## 8. Recommendation

1. **Measure before building:** the usable-output probe (one slot, §3.1 + the §5 emit/store
   checks folded in) and the offline peak-pressure pass (§3.2). These turn "moderate
   reallocation vs larger spill" into a decided number.
2. **Then draft the code-level change:** the SGX530 register convention (outputs for scratch),
   the driver PDS attribute rows for the pointers/constants, per-core gating.
3. **In parallel, the AM335x capture stays the cheaper win:** if it shows the temp allocation
   the SGX530 EDM task needs, temporaries would work directly and the restructure becomes
   unnecessary. The restructure is the path that does not depend on getting the capture; the
   capture is the path that avoids the restructure. Worth pursuing both until one lands.
