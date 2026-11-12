# State of the SGX530 port (stage 1, after attempt 47)

A one-page summary for whoever picks up the **AM335x vendor capture**. Detail lives in
[sgx530-stage1.md](sgx530-stage1.md) (the attempt-by-attempt runbook),
[sgx530-attr-restructure.md](sgx530-attr-restructure.md) (the restructure design) and
[sgx530.md](sgx530.md) (the fact sheet). Board probing is **paused** here by plan: the register
files are fully mapped and the next unknown is a task-setup question only a working driver shows.

## Where it stands, in one paragraph

The open microkernel's USSE code **executes** on the SGX530 (two per-core fixes got it there,
below). But the launched task cannot move data to or from memory: its only write that takes
effect is an ALU write to an output register. Temporaries don't retire, the attribute (`pa`) bank
doesn't retire, loads have no retiring destination, and stores don't reach DRAM. That is one
signature, not many bugs — **the task lacks the write / EDM-task context a real EDM task has**,
and only the output/emit path (which bypasses that context) works. The register-file restructure
(scratch in outputs) is designed and validated but **cannot by itself make the microkernel run**,
because the microkernel must read the CCB/control block (loads) and persist state (stores), and
neither works. Unblocking the 530 needs the task-context setup the vendor driver uses.

## What works (C, on hardware)

- **The task executes.** Two per-core differences, both confirmed and fixed:
  - `USE_CODE_BASE` counts **256-byte units** (`(field << 8) & 0x0fffffff`), not 128 B — code
    base 15 = `kernel_code >> 8` (attempt 24).
  - **`phas.i` must be the first instruction** of a task (no `USE_UNLIMITED_PHASES`) (attempt 26).
- **ALU writes to outputs retire.** `o0..o10` are **eleven distinct, task-writable** registers
  (attempts 41, 44, 47). This is the one writable file.
- **Reads all work:** `ld.a.d rN` reads the named register; an output reads back as an ALU source
  or `ld` base; a primary attribute set by a **standalone** `DOUTA` reads back and **persists**
  across resets, unload and reload (attempts 40-43).

## What doesn't — the four gates (all C, attempt 47 unless noted)

| # | Gate | Result |
|---|---|---|
| 1 | Scratch → **outputs** (ALU) | **Green.** o0..o10 writable and distinct; peak pressure is 10 (`ukernel/pressure.py`), so it fits. |
| 2 | Bases/pointers → **attributes** | **Path A dead.** `DOUTA`-then-`DOUTU` in one program never launches (task starts at code base 0); in the `pds3` layout even the `DOUTA` fails to write PA0. Only a **standalone** `DOUTA` writes an attribute (path B: the driver re-issues it per power transition). |
| 3 | Task **stores** reach memory | **No.** `st` of a distinctive value commits to **neither arena** (scanned from DRAM after an `rmb`); `pa0` is inside the data arena, so it is not misplaced — the store does not commit. |
| 4 | A **retiring load destination** | **No.** The task can't write the `pa` bank; `ld` can't target an output (not encodable); temps don't retire. A loaded value has nowhere to live. |

**Register-file / memory map (the complete picture):**

| mechanism | effect |
|---|---|
| ALU write to an output `oN` | **retires (C)** |
| ALU write / `limm` / `ld` into a temporary `rN` | does not retire (C) |
| ALU write into the `pa` bank | does not retire (C, attempt 47) |
| `str` to an `EUR_CR_*` register | dropped (C; UAPI P1) |
| `ld` into an output | not encodable by `sgx-asm` (and perhaps not an ISA destination) |
| `st.a.d` to memory | does not commit to DRAM (C, attempt 47) |
| standalone `DOUTA` → `pa` | writes it, persists (C) |
| `DOUTA` then `DOUTU` in one PDS program | misfires — DOUTU at code base 0, and the DOUTA may not write either (C) |
| task's store to an `EDMPROTECT` page | faults; the DM field (0/1/2/3) doesn't change it (C) — the task isn't the EDM requestor |

## What a vendor capture (TI's SGX530 EDM task, AM335x) must show

The capture is the critical path. Three things to read from a working EDM task:

1. **The task context / temp allocation.** What in the task setup makes its **temporary (and
   `pa`) register writes retire**? Ours don't. Candidates already ruled out on our side: the DOUTU
   word-0 temp count, `phas.i` temps4, the host init-regs, instruction pairing. So it is something
   else in how the EDM task is issued — the field or context the vendor sets that we do not.
2. **The store / write path.** How do the EDM task's **stores reach memory** and get **admitted by
   `EDMPROTECT`** (i.e. how is it tagged as the EDM requestor)? Ours don't commit and aren't the
   EDM. This is the same context question from the write side.
3. **The PDS launch.** How does the vendor's **event PDS program launch the EDM task** — the DOUTU
   word, the data-master / code-base setup, and whether it sets attributes with `DOUTA` in the
   launch program (and, if so, how that works there when it misfires for us). Our event program is
   a bare DOUTU; the DDK's `pds3` has never run on the 530.

If the capture shows one task-setup difference that makes writes commit, the restructure (and the
rest of the bring-up) resumes from solid ground. If it can't be obtained, the 530 is blocked at
this layer.

## On the shelf, ready to resume

- The **register-file restructure** (scratch → outputs, bases → attributes): design in
  [sgx530-attr-restructure.md](sgx530-attr-restructure.md), with the measured peak pressure (10)
  and the gate analysis.
- The **probe infrastructure**: `ukernel/cores/sgx530_*.py` (the diagnostic cores), the
  `USSE_*` probe branches in `ukernel/ukernel.py`, and `ukernel/pressure.py` (the liveness pass).
  The 545 golden stays byte-identical throughout.
- **Clean-room**: everything here is from `docs/` and black-box board tests; no decompiles.

## Open questions for Joseph

- The AM335x capture board (open question §7.1/§7.3): the critical path above.
- Isolation before client work is a separate hard gate (fact-sheet **gap 19**), independent of this.
