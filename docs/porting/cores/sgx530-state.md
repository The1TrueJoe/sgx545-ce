# State of the SGX530 port (stage 1, after attempt 47)

A one-page summary for whoever picks up the **OMAP3630 vendor capture**. Detail lives in
[sgx530-stage1.md](sgx530-stage1.md) (the attempt-by-attempt runbook),
[sgx530-attr-restructure.md](sgx530-attr-restructure.md) (the restructure design) and
[sgx530.md](sgx530.md) (the fact sheet). The register files are fully mapped; the next unknown is a
task-setup question only a working driver shows. There is no AM335x board, so this is now pursued on
the OMAP3630 board: a vendor-stack capture (a separate analysis-side session, into
`sgx530-vendor-setup.md`), plus cheap black-box checks from this side (runbook §"fresh black-box
hypotheses").

## UPDATE 2026-11-14: the vendor stack renders the 64x64 scene; what its render stream looks like

**How it was observed.** On the OMAP3630 board, cold-booted into a capture kernel with the GPL
openpvrsgx 1.14.3699939 services module and the version-matched vendor user space (DDK
1.14.3699939), a small GLES2 test drew a 64x64 pbuffer: clear to (0, 0, 0.5, 1), then one
flat red triangle over the lower-left half. A GPL hook in the services module's TA kick
copied the kick's command and every page mapped in the GPU's MMU contexts, except the
parameter buffer and the USSE code heaps, before the GPU ran it. The words below were read
back from those pages with our own layouts (PDS.md, STATE.md). No vendor code or binary was
read. The vendor EGL needs a KMS device whose driver reports the name `tilcdc`. omapdrm on a
fixed DPI panel, renamed by an LD_PRELOAD shim on `DRM_IOCTL_VERSION`, satisfies it.

- **It renders correctly (C).** The readback has the triangle red and every other pixel the
  clear colour. So nothing about this scene is beyond the SGX530 at rev 0x00010205.
- **VDM words give PDS programs as offsets from the PDS exec base (C).** On the SGX545 they are
  absolute (`type<<28 | addr>>4`, PDS.md). On the SGX530 the field is
  `(addr - PDS pixel heap base) >> 4`, the same origin as the TA/3D state words. This holds
  for the type 4 and type 6 state words and for the draw's vertex-fetch word. Read as
  absolute, they point at unmapped memory. Read relative to the PDS pixel heap base, every one
  lands on a program of the expected form (the state-program template, the vertex fetch).
- **The vertex shader launch carries no write-retire context (C).** The vertex-fetch DOUTU
  has word 0 bit 21 clear, with code base 10 and 0 temporaries in the two draws seen. Words 1
  and 2 are both 0. So the hypothesis that vertex tasks need the §9 bit 21 + word 2 context is
  **refuted**.
- **Bit 21 is set on some other launches (C, what it means: G).**
  - The state programs that DMA a TA state block launch their emit task with word 0 bit 21 set.
  - One pixel secondary program (DOUTD of its uniforms, then DOUTU) does the same.
  - State programs without a DMA don't. Neither do the TA terminate's task or the pixel
    primary programs.
- **Pixel DOUTU word 2 is 0x20 (C).** On the SGX545 every DDK DOUTU has word 2 = 0 (PDS.md).
  On the SGX530 the pixel programs seen have 0x20 there. Vertex and state launches keep 0.
- **The TA terminate (VDM type 6) is two DOUTAs (C).** The first is `(0x2000, 0)`. The second
  is `(0x00030003, 0x100)`, which is the 64x64 target's last tile, y<<16 | x. Then comes a
  state-emit DOUTU. The SGX545's form has three `(0x16000e, 0x100)` DOUTAs. Just before the
  terminate, a type 4 state program writes `DOUTA (0x800, 0), (0, 0x100)`.

**Applied to our stack (C, null).** sgxgen knobs on `wip/sgx530-kick-test`: `SGXGEN_VDM_REL`
(relative VDM PDS addresses), `SGXGEN_TERM530=0x00030003` and `SGXGEN_STATE_BIT21`. Each was
tried alone and then all together, with the 8-word interleave. Every run gives the same hang:
region headers and tail pointers all zero, no BIF fault, no TA_FINISHED, `consumed 2`.

**Where that points (D).** In every variant the microkernel's bounded wait `0xd` gives up once
per TA kick (control block word 0x28 = 0xd, word 0x29 = 2). That wait is the TA-side cache
invalidate in `invalidate()`, which waits for `0x138` bits `0x27`: the SGX545's done bits. It
comes before the TA start. So the next check is the microkernel's TA kick on the SGX530. Do
the invalidate done bits differ, and does the TA start at all? The TA/VDM status registers
after the hang would answer that. The coredump doesn't dump them yet. Keep the three sgxgen
changes, since the vendor stream shows the SGX530 needs them, but they aren't the blocker.

## UPDATE 2026-11-13: full 64x64 kick on silicon — microkernel + command layer PROVEN; only blocker left is sgxgen's PDS data layout

The driver's `edm_init` + host-played kick ran a 64x64 frame on SGX530 silicon (core rev
0x00010205). Result: the microkernel starts clean (EDM init event 0x0, BIF 0x0), consumes
both kicks (`consumed 2`), and HWR recovers on the completion timeout — but the TA emits no
tiles, so the output is all-zero. **Stock (WAIT_3D/WAIT_FREE) and edm (SW_EVENT) images behave
identically**, so completion is not the issue; nothing is rendered to complete.

Root cause, confirmed from the played command (`frame64.trace`, decode in the kick-annotation
note): **sgxgen emits its PDS data segments in the 545's 2-word interleave.** The state-emit
program is the PDS.md template `… doutu ds0[4..5]`; its DOUTU source `ds0[4]` sits at memory
word 8 (= 0x0000070a, the task descriptor) under the 2-word layout, but at memory word 4 (=
**0x00000000**) under the 530's 8-word interleave (vendor-setup §2). So on 530 silicon every
DOUTU/DOUTD reads a null source → the USSE task launches at code base 0 → no emit. This is
gate 2 (vendor-setup §2) reproduced at frame scale.

**So the open microkernel AND the command/uAPI layer are both proven on 530 silicon.** The one
remaining blocker is a **codegen change in sgxgen's PDS data-segment packing**: lay DS0/DS1 out
in 8-word blocks when the core lacks `PDS_DATA_INTERLEAVE_2DWORDS` (formula in vendor-setup §2,
row-size note in PDS.md). It is NOT an ISA/encoding change — the PDS instructions and USSE words
are unchanged. Everything below this line predates the kick and is the register-file stage-1
history.

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

## UPDATE 2026-11-12: the §9 vendor launch SOLVES the write-retire blocker

Everything from "What works" down was measured under the **pre-§9 launch** and
describes the task **without** an EDM write context. The vendor launch
([sgx530-vendor-setup.md](sgx530-vendor-setup.md) §9) supplies that context, and the
blocker is now **solved** for the register file:

- **Gate 2 (launch) closed (C).** Under the 8-word PDS interleave + the vendor DOUTU
  words (word0 0x0020000f, word1 0x04000000, word2 1), `0xa6c`=3 / `0xa70`=0x00013d01,
  EDMPROTECT on, the task launches and runs — USE goes busy, no code-base-0 fault.
- **Gate 1 (temporary writes retire) SOLVED (C, `sgx530_xorr5`, 21:53 UTC).**
  `xor r5, r5, r17` faults at **0x00a90000** = r5 ^ r17 (leftover 0x0fa93000 ^ ctlblk
  0x0f003000), the predicted "the write landed" value, so the xor changed r5 —
  temporaries retire. The earlier or-based `aluwr`/`aluwr_pa0w` read 0x0fa93000 and
  looked like no-retire, but that was a **subset trap**: ctlblk (r17 = 0x0f003000) ⊆
  r5's leftover made `or r5,r5,r17` a value-no-op. `xor` is non-identity there (see
  the runbook's "subset trap" section).

**Minimal temp-retire launch (bisect, 22:01 UTC):** word2=1 **and** word0 bit21;
word1 is free (0x04000000 or 2 both retire). Notably word1 = 0x04000000, vendor-setup
§6's leading write-context candidate, is **not** the temp-retire lever.

**What §9 does and does NOT fix (the microkernel's write paths):**

| path | instr | under §9 | note |
|---|---|---|---|
| temp register file | ALU (xor/or) | **retires (C, xorr5)** | all computation / scratch |
| pa bank | ALU | does not retire (C, xorpa0) | **not a blocker** — the vendor writes PA0–PA2 via **attribute writes (DOUTA)**, not ALU (vendor-setup §5); the standalone-DOUTA path works (C) |
| memory store | `st.a.d` | **silently drops (C, stor5)** | survivable — the host owns memory |
| EUR_CR register | `str` | **works (C, limmwr_r5)** | programs BIF/DPM/kicks |
| load immediate | `limm` | **works (C, limmwr_r5)** | constants |

**ARCHITECTURE DECIDED (2026-11-12): the open microkernel runs as an EDM task on the
530.** `sgx530_limmwr_r5` under §9 read EVENT_TIMER = 0x00000a5a — `limm` AND `str`
both commit. So the task can compute (temps retire), load constants (`limm`), read
memory into temps (`ld`), and program every register (`str`) — enough to point the
BIF, program the DPM, and kick the TA/3D. The one thing it cannot do, commit arbitrary
`st.a.d`, is the host's job anyway: the **kernel owns every firmware-visible structure**
and drm_sched/dma_fence signal completion from the **host IRQ**, not from microkernel
stores; `pa` goes via DOUTA. Host-kick stays the fallback but is not needed. The plan
to fold the §9 launch into real init with host-owned memory is
[sgx530-edm-path.md](sgx530-edm-path.md). (One confirmation pending: `limmwr_r5_5c3`
re-runs with a fresh constant to rule out EVENT_TIMER residual; `ldret` confirms loads
retire.)

So the gates table below is the **pre-§9** picture, now superseded: §9 + host-owned
memory is the path.

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

## What a vendor capture (TI's rev-125 SGX530 EDM task, on the OMAP3630 board) must show

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

## Open questions

- **The three task-context questions now have candidate answers from the vendor stack**
  ([sgx530-vendor-setup.md](sgx530-vendor-setup.md), all U): the vendor's launch (DOUTU) differs
  from ours in all three words (word 1 = 0x04000000, word 2 = 1, word 0 bit 21), its PDS
  programs use an 8-word data interleave (which alone explains gate 2), it maps every control
  structure with `EDM_PROTECT`, and the host side sets up no register file at all. The one-change
  board test is in that page's §9: the 8-word layout plus the vendor launch words, read out with
  the `aluwr` (temporary retire) and `stout` (store commit under `EDM_PROTECT`) probes. That test
  should settle whether writes retire and stores commit without a full vendor capture.
- A vendor-stack capture (a 5.4/6.0-class kernel with the vendor user space, RAM-booted on the
  OMAP3630 board) is now a fallback, needed only if the board test leaves the write-retire or
  store question open — not the critical path it was.
- Isolation before client work is a separate hard gate (fact-sheet **gap 19**), independent of
  this. If the vendor launch words make stores commit with `EDM_PROTECT` on, the gap-19 stopgap
  (mapping the arena without the flag) can be dropped.
