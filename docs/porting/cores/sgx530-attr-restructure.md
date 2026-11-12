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

A DOUTA writes a primary attribute (confirmed: PA0, attempts 40-41) and the value persists.
**But** setting an attribute *and launching the task in one PDS program* (DOUTA then DOUTU) is
**not yet shown to work on the SGX530** — on the 530 the event program is `pds0`, a bare DOUTU,
and every DOUTA-then-DOUTU the probe built issued the DOUTU at code base 0. §6 analyses why and
gives two ways to get the attributes set before the task reads them.

**(B) Scratch moves from temporaries to output registers.** Every `rN` the SGX530 microkernel
writes becomes an output `oN`. Loads land in outputs (`ld.a.d oN, …`), ALU results go to
outputs, stores read from outputs. This is the part that makes the microkernel able to keep
computed state at all — the heartbeat `ld; imae; st`, dispatch `ld; test; (p0) ba`, address
arithmetic.

## 3. Size, from the measured register pressure

**Not a mechanical rename, because of register pressure.** The microkernel writes 17 distinct
temporaries across the paths, but what bounds the restructure is **peak pressure** — how many
are simultaneously live at the busiest instruction — not the count of names.

**Measured (offline CFG liveness pass, `ukernel/pressure.py`): peak = 12 live temporaries.** The
pass builds the control-flow graph from the assembled SGX530 stream (2136 instructions, 133
labels), runs backward dataflow, and takes the maximum live set. The peak sits in one hot region
(the TA/transfer path, insns ~1120-1138); only 8 instructions reach ≥11 live, and most of the
program runs at ≤8 (histogram: 102 insns at 8, 43 at 9, 14 at 10, 4 at 11, 4 at 12). The peak-12
set is `{2,3,4,5,6,7,9,10,12,13,14,17}`.

**Two of those twelve are the loader-set bases the restructure moves to attributes.** `r17` is
the control-block base and `r14` the PD table — §2(A) puts both in primary attributes (PA0, PA1),
read directly as `pa0`/`pa1` (an attribute works as an `ld`/`st` base; `ld.a.d r3, pa0` is
confirmed). They leave the scratch budget entirely. So **peak OUTPUT pressure = 12 − 2 = 10.**

Against the usable-output count from attempt 42 (architectural ceiling ~11, **U**; `o0`
confirmed):

- **≥10 usable outputs → a per-core reallocation, no spill.** A 530 register remap in
  `ukernel.py`'s emit layer (scratch → `o0..o9`), plus the §2(A) attribute change for the two
  bases, plus per-core gating. Contained: it changes the register convention, not each path's
  logic. This is the likely case if the bank is ~11.
- **<10 usable outputs → reallocation + a little spill**, only in the one hot region that
  exceeds the count (store/reload the overflow to a scratch word). Not a full spilling allocator
  — most of the program fits, so only the ~8-instruction peak needs it. Bigger than the clean
  case but still bounded.

So the one number still pending is attempt 42's usable-output count; the pressure side is done.

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

- **Usable output count** (§3) — peak output pressure is measured at 10, so the question is
  whether ≥10 outputs are writable. Tested by the per-output fault-readout sweep (`or oN, r5,
  r17; ld.a.d r3, oN`: the fault page is the readout, 0x0fd7d000 = writable). The earlier
  store-based sweep was wrong — it used `r17` (through a `limm` that does not retire on the 530)
  as the store base, so nothing would reach memory; **on the 530 there is no `limm`-loaded temp
  base to store through.** The fault-readout needs no base.
- **`st.a.d` reading an output as its source.** Needs a *mapped* store base, which `limm` cannot
  provide. Use PA0: a **standalone DOUTA** (driver-set, persists — this does not need
  DOUTA-then-DOUTU) puts a mapped control-block scratch address in PA0, then the task does
  `or o0, r5, r17; st.a.d pa0, #0, o0`, and the host reads that scratch word (0x0fd7d000 =
  st-from-output works). Probe `cores/sgx530_stout_o0.py`, paired with the driver's DOUTA.
- **Emit-at-end semantics.** Outputs are normally emitted to the output buffer when a task
  ends. Every output probe self-loops, so emit is untested. If the microkernel's EDM-style task
  emits outputs on completion, using them as scratch could write scratch to memory. Confirm the
  task emits nothing or somewhere harmless (the EDM task has no real output buffer), or the
  restructure needs the emit suppressed. Its own careful, recovery-ready probe (it needs the
  task to actually end), later.

## 6. DOUTA-then-DOUTU on the SGX530: the open problem, and two ways round it

**The facts (C, attempts 40-41).** A DOUTA writes a primary attribute and it persists (PA0 read
back as 0x0e850000, then 0x0e860000, across resets/reloads). But a DOUTA *followed by a DOUTU in
the same PDS program* makes the DOUTU issue with word 0 ≈ 0 — the task launches at code base 0
(fault 0x0f400000). One-row and two-row DOUTA layouts fail identically, so it is **not** the
data-row interleave. On the 530 the event register points at `pds0` (a bare DOUTU, `pds_ours()`
in `srvinit.c`); `pds3` — the DDK/open event dispatcher that does DOUTA-then-DOUTU — **has never
run on the 530**, so this pattern is simply unproven there, not known-good.

**Why it may break (H, from PDS.md).** PDS.md records the captured working DOUTA as `2f800466`
— top nibble 2 with bit 27 set (`.x2.s27`), the form that "also consumes word 2." The open
`srvinit` `movs()` emits DOUTA as the plain `07…` form (no bit 27), and the probe does too. A
DOUTA is a 2-word DOUT; if the hardware's DOUT sequencer expects the `2f` (3-word-consuming)
form for a DOUTA that precedes another DOUT, the plain `07` DOUTA could leave the sequencer
mis-aligned so the next DOUTU's word 0 reads short. This is a hypothesis, testable by rebuilding
the DOUTA in the `2f` form and seeing whether the following DOUTU recovers. A second
construction difference worth matching: the working `pds3` DOUTU reads word 1 from a DS word
holding the constant **2** (the microkernel phase/swizzle count, D0(0)=2) and word 0 from a DS1
word holding `doutu()`; the probe's slot choices differ, and mirroring `pds3`'s exact slots
removes that variable.

**Two ways to get the attributes set before the task reads them:**

- **(A) Solve DOUTA-then-DOUTU** so the launch program (`pds0` today) also sets the attributes.
  Needs the hypothesis above resolved on hardware. This is the general path and matches how the
  DDK primary-attribute setup works (per-task, in the launch program).
- **(B) Exploit persistence.** The primary attributes survive between PDS invocations on the 530
  (observed across resets and reloads). So a **one-time setup program** could DOUTA the pointers
  once, and the normal bare-DOUTU `pds0` launches would read the persisted PA0-PA2 — no
  DOUTA-then-DOUTU needed. Cheaper, but it depends on the attributes surviving across **real task
  launches** (not just resets), which is **U** and must be confirmed: set PA0 in one program,
  launch an unrelated bare-DOUTU task, and check PA0 still reads back.

Path (B) is the lower-risk first move if persistence holds across launches; path (A) is the
fallback and the more general answer. Either way, attribute *setup* is an open SGX530 PDS
problem, not a solved one — the earlier claim that the driver's `pds3` already handles it was
wrong (it does not run on the 530).

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
