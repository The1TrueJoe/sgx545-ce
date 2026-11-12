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

**Attempt 42 (C): all of o0..o10 are writable** — every one faults at 0x0fd7d000. That is
**eleven** writable outputs against a peak of 10, so the restructure is **a per-core
reallocation, no spill:** a 530 register remap in `ukernel.py`'s emit layer (scratch → `o0..o9`),
plus the §2(A) attribute change for the two bases (r17, r14 → PA0, PA1), plus per-core gating.
Contained — it changes the register convention, not each path's logic. The `<10 usable` spill
case does not arise. **The margin is one** (eleven outputs, ten needed) *if* o10 is distinct too.

**Distinctness is now confirmed (C, attempt 44):** o0..o9 are ten distinct task-writable
registers, so the scratch side of the reallocation is a GO (margin 1 pending `odist10`). **But a
third gate appeared:** the task's `st` to a mapped control-block page faulted (attempt 44, likely
EDM_PROTECT — §5). The microkernel cannot run if its stores fault, so the restructure's three
gates are: **(1) scratch → outputs: green;** (2) bases → attributes (DOUTA-then-DOUTU, pds3-mirror
next); **(3) task stores land (the EDM data-master / EDM_PROTECT question, attempt 45).** The
register reallocation is ready to draft, but it only makes the microkernel *run* once gates (2)
and (3) are green too.

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

- **Usable output count — done (C, attempt 42):** all o0..o10 writable, ≥10 holds. The
  per-output fault-readout (`or oN, r5, r17; ld.a.d r3, oN`: fault page is the readout, 0x0fd7d000
  = writable) was the right shape; the earlier store-based sweep was wrong (it used `r17`, a
  non-retiring `limm`, as the store base — there is no `limm`-loaded temp base on the 530).
- **Output distinctness — done (C, attempt 44).** The ten `odist` images each read their own
  source's leftover (o0 0x0055d000 .. o9 0x0d04f000), so **o0..o9 are ten distinct task-writable
  registers** — the scratch gate is green and covers the budget of 10. (`odist10`, the margin-1
  check, is folded into attempt 45.)
- **Can the task store to the control block at all? — the third gate (attempts 44-45).** The
  store `st.a.d pa0, #0, o0` **faulted** (BIF 0x4004, control-block page). Attempt 45 found the
  cause: **EDM_PROTECT (C).** The code-base-15 data-master field at 3/0/1/2 made no difference, but
  mapping the arena **CACHECONSISTENT only (no EDM_PROTECT)** stopped the fault. So the driver fix
  is clean: the SGX530 maps the microkernel's writable structures without EDM_PROTECT (a bring-up
  stopgap, landed as d8160ba2). (This fits UAPI.md P2 — a 545 client `st` into EDM_PROTECT memory
  wedges the GPU — the 530 task simply is not treated as the EDM, and the DM field does not change
  that: on the 530 a USSE task issues as a USSE data master, not the EDM, and the single shared
  directory list 0 leaves EDM_PROTECT nothing to scope by. Analysis in sgx530.md, "Why the SGX530
  USSE task is not the EDM requestor".) **Stage-2 open item (gap 19):** the stopgap removes kernel
  isolation, so before any client work, isolate kernel memory by the per-context page directory
  (the kernel driver omits kernel heaps from client PDs), not EDM_PROTECT.
- **Does the store reach memory? — still open (attempt 45).** With EDM_PROTECT off the store no
  longer faults, but the target word still read 0. Either `st` cannot read an **output** as its
  data source, or `idf`/`wdf` does not flush the USE data cache to memory (the DDK uses op0x100).
  The control `sgx530_stout_r5` (store a temp through the same path) tells which. **This is
  load-bearing for the restructure:** scratch lives in outputs, and the microkernel must *store*
  that scratch to the control block. If `st` cannot read outputs, the restructure needs another
  path from an output to memory (e.g. the emit mechanism), which would enlarge it.
- **Emit-at-end semantics.** Outputs are normally emitted to the output buffer when a task
  ends. Every output probe self-loops, so emit is untested. If the microkernel's EDM-style task
  emits outputs on completion, using them as scratch could write scratch to memory. Confirm the
  task emits nothing or somewhere harmless (the EDM task has no real output buffer), or the
  restructure needs the emit suppressed. Its own careful, recovery-ready probe (it needs the
  task to actually end), later.

## 6. DOUTA-then-DOUTU on the SGX530: the open problem, and two ways round it

**Why it matters: attribute setup is required, not optional.** The microkernel's bases r17
(control block) and r14 (PD table) are loader-patched `limm`s, and `limm` does not retire on the
530 — that is the original blocker. So those addresses cannot reach the task through a temporary
at all; the **only** way to hand the task a mapped pointer is a primary attribute (DOUTA). The
microkernel therefore cannot run until attribute setup works. This is the gating problem for the
whole restructure, not a convenience.

**The facts (C, attempts 40-43).** A DOUTA writes a primary attribute and it persists (PA0 read
back as 0x0e850000, then 0x0e860000, across resets/reloads). But a DOUTA *followed by a DOUTU in
the same PDS program* makes the DOUTU issue with word 0 ≈ 0 — the task launches at code base 0
(fault 0x0f400000). One-row and two-row DOUTA layouts fail identically (not the data-row
interleave), and the **`2f`/`.x2.s27` DOUTA form is ruled out (C, attempt 43):** both the plain
`07…` and the `2f…` DOUTA (word | 0x28000000, purely additive on bits 27/29, every source field
intact) give the same code-base-0 fault. So the DOUTA's *encoding* is not what breaks the next
DOUTU. On the 530 the event register points at `pds0` (a bare DOUTU, `pds_ours()` in
`srvinit.c`); the DDK/open `pds3` dispatcher that does DOUTA-then-DOUTU has never run there.

**The better-evidenced next step: mirror `pds3`'s exact slots.** The remaining differences from
the known DDK construction are not the DOUTA form: `pds3`'s DOUTA takes its *value from an input*
(`IN(0)`), not a DS word; its DOUTU reads word 1 from a DS word holding the constant **2** (the
microkernel phase count), and word 0 from a DS1 word holding `doutu()`; and `pds3` has *many
instructions between* its DOUTAs and its DOUTU, where the probe puts them adjacent. Mirroring
`pds3` tests these together against the one construction known to work on the 545. If it is still
flat, the order-swap diagnostic (DOUTU first, then DOUTA) shows whether a DOUTA breaks *any*
following DOUT or only this sequence.

**The pds3-mirror spec (from docs/SRVINIT.md "Buffer contents", capture-confirmed).** pds3's
DOUTU is the word **`07003885`**, which "sends ds1[3], ds0[0] = 2, ds0[1]" (SRVINIT.md): DOUTU
word 0 = ds1[3] (a DS1 word holding `doutu(entry)`), word 1 = ds0[0] = **2** (the microkernel
phase count), word 2 = ds0[1], dest 5. Its DOUTAs are `douta in0/in2/in4` to PA0-PA2 with ctrl
0/0x100/0x200 — the **value comes from an input** (in0), not a DS word. A minimal mirror probe:

    data: ds0[0] = 2 ; ds0[1] = 0 ; ds1[0] = 0 (DOUTA ctrl for PA0) ; ds1[3] = doutu(reader entry)
    in0  = V  (the PA0 value, supplied as the task input by the host)
    code: douta in0, ds1[0], in0, in0        # PA0 = in0 = V, ctrl 0  (value from an INPUT)
          <several non-DOUT instructions>     # pds3 has ~30 here; the probe had none
          movs 07003885                       # DOUTU: w0=ds1[3]=entry, w1=ds0[0]=2, w2=ds0[1]
          halt

The four deltas from the failing probe, any of which could be the cause: DOUTA value from an
input not a DS word; DOUTU word 1 = 2 (the probe's was 0); DOUTU word 0 from a DS1 word; and
non-DOUT spacing between the DOUTA and the DOUTU. The reader (`sgx530_pa0`) launched by the DOUTU
reads PA0; a fault at V = both the DOUTA wrote PA0 and the DOUTU launched the reader. Build it
from the SRVINIT.md description above, not from `srvinit.c` (whose pds3 is our reconstruction).

**If DOUTA-then-DOUTU cannot be made to work**, the fallback is persistence: a standalone DOUTA
(confirmed to persist) sets the pointers, and the driver re-issues it after every reset / resume
/ power transition rather than relying on them surviving one. Heavier for the driver, but it does
not need DOUTA-then-DOUTU. Either way, attribute *setup* is an open SGX530 PDS
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
