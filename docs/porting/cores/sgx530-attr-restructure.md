# SGX530: the pointers-in-attributes restructure — proposal

A proposal, not a plan. It rests on one unresolved fact (does any task-written register file
retire on the SGX530?), and it is **conditional on that**. Send-back requested before code.

## 1. What is established (C, stage-1 attempts 24-40)

- The open microkernel's USSE code **executes** on the SGX530 (code base `>> 8`, `phas.i`
  first). `ld.a.d` generates addresses and faults/loads; an unconditional `ba` self-loops.
- **No task-written register retires.** `limm`, an ALU `or` into r0-r31, `str` to a hardware
  register, all leave the register a later read sees unchanged. Reads return each register's
  stable power-on leftover. (The `ld` destination is untested in isolation but is the same
  write port.)
- **PDS-written primary attributes work (attempt 40).** A PDS `DOUTA` wrote PA0; the task read
  it through `ld.a.d r3, pa0`; the value persisted across resets, unload and reload.

## 2. The decisive unknown this proposal rests on

**Does *any* task-written register file retire?** `ld.a.d r3, …` writes its destination `r3`;
arithmetic (`imae`, `or`) writes its destination; `test` writes a predicate. If **none** of
these retires, the microkernel cannot do a read-modify-write or a data-dependent branch at
all — not the heartbeat `ld r3; imae r3,r3,#1; st r3`, not dispatch (`test; (p0) ba`), not
address arithmetic. Pointers- and constants-in-attributes would supply *inputs*, but the
microkernel still has to *compute*, and computation needs a writable destination.

The hinge probes (built, `sgx530_aluwr_o0`, `sgx530_aluwr_pa0w`) answer it: `or o0/pa0, r5,
r17` then `ld` it back. 0x0fd7d000 = that file is task-writable scratch.

- **If some file (outputs `o`, or attributes written by the task) retires:** the restructure
  below is feasible — scratch in that file, pointers/constants in PDS-set attributes.
- **If nothing task-written retires:** the open microkernel cannot run on the SGX530 as a
  computing program. There is no memory-only escape: read-modify-write of a memory word
  still loads into a register, adds (register write), and stores — the middle step needs a
  retiring destination. Then the only paths are (a) find the temp-allocation the SGX530 needs
  (the AM335x vendor capture would show it), or (b) abandon this core.

So §3-§5 are the "some file retires" branch.

## 3. Which pointers and constants move to PDS-set attributes

Today the event entry carries pointers in **temporaries**, set by loader-patched `limm`
(UKERNEL.md): r17 = control-block base, r14 = PD table. `limm` does not write on the SGX530,
so these never load — the root of the first fault (`ld.a.d r3, r17` at r17's leftover).

Move to PDS-set primary attributes (DOUTA, which works):
- PA0 = the control-block base (replaces the r17 `limm` patch).
- PA1 = the PD table base (replaces the r14 `limm` patch), on a single-context core.
- PA2+ = the small constants the entry stores (0, 1, the version 0x00010000, the DPM
  clear values), so a `st.a.d PAn → memory` writes a constant without an ALU.

The host already knows these addresses; the driver's PDS builder (`pvrsgx_pds_entry`) writes
them into the DOUTA rows instead of the loader patching `limm` sites. No `limm` of an address
remains in the SGX530 image's hot path.

## 4. How the PDS programs set them (DOUTA rows), and the multi-row caveat

`DOUTA (value, ctrl)` writes `value` to the primary-attribute register named by ctrl b8-15
(ctrl 0 = PA0). One DOUTA per attribute. The data words come from the PDS program's data
segment (DS0/DS1 rows).

**The caveat, and the answer to the coordinator's second question.** The two-row DOUTA probe
issued its DOUTU with word 0 = 0 under both 0xa70 = 6 and 8, so `0xa70` is **not** the data
length, and **the second data row is not being read as we lay it out**. The PDS data segment
interleaves DS0 and DS1 two words at a time (`DS0[n] @ (n>>1)*4+(n&1)`, `DS1[n] @
(n>>1)*4+2+(n&1)`, PDS.md). The SGX530 has 1 PDS pipe region difference and may interleave
differently, so row 1's `D0(2)` reaches the MOVS as 0. The one-row variant (DOUTA from
D0(0)/D0(1), DOUTU word in D1(0)/D1(1), no row 1) avoids it and is the clean confirmation.

**This matters for the restructure:** setting several attributes needs **several DOUTA rows**
— exactly the multi-row PDS layout that is currently mis-read. So the restructure depends on
first decoding the SGX530's PDS data interleave (how row 1+ lay out), by growing the one-row
program one row at a time and reading which attribute each row sets. That is a prerequisite,
not an afterthought.

## 5. What the microkernel still writes at runtime

Even with pointers and constants in attributes, the SGX530 microkernel must still, at
minimum:
- increment the heartbeat (`ld; imae; st`) — a register add;
- dispatch on `EVENT_STATUS` / init-done (`ld; test; (p0) ba`) — a register test and a
  predicated branch;
- compute addresses for the CCB, RT data, sync words (register arithmetic).

All need a retiring destination (§2). So the restructure is **scratch in the task-writable
file (if one exists) + pointers/constants in attributes**. If no file is writable, there is
no runtime-state story and the restructure cannot stand.

## 6. SGX545 impact: none

All of this is per-core. The SGX545 keeps `limm`-patched temporaries and its current PDS
programs; `limm` writes there, so nothing changes. The SGX545 golden stays byte-identical.
The SGX530 build would select the attribute path by a per-core flag.

## 7. Recommendation

1. The one-row DOUTA (kernel session) — clean same-run confirmation of §2's PDS write/read.
2. The hinge probe `sgx530_aluwr_o0` (and `_pa0w`) — does a task-written output/attribute
   retire? This decides whether §3-§5 are possible at all.
3. If (2) is positive: decode the PDS multi-row interleave (§4), then I draft the full
   microkernel restructure. If (2) is negative: the open microkernel cannot run on the
   SGX530 as designed; the AM335x capture of TI's working EDM task is the path (it shows the
   temp allocation we could not find), or the core is deferred.

Two probes settle whether a restructure exists. Everything past them waits on their result.
