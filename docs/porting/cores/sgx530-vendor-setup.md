# SGX530: how the vendor stack starts and runs the EDM task

This page answers the three questions in [sgx530-state.md](sgx530-state.md):

1. What makes an EDM task's temporary and `pa` register writes retire.
2. How its stores commit and are admitted by `EDMPROTECT`.
3. How the event PDS program launches it.

The short answer: the vendor stack differs from ours in **every word of the EDM task's
launch (DOUTU)** and in the **PDS program data layout**. The data-layout difference by itself
explains gate 2 (the DOUTA-then-DOUTU failure, C). The three DOUTU words are the leading
candidates for gates 1, 3 and 4 (temporaries, stores, a retiring load destination). None of it
is run on our board yet, so every claim below is **U**: it is what the vendor configures, read
statically from its user space and the GPL kernel side, not a hardware result.

| Mark | Meaning |
|---|---|
| **C** | Confirmed on SGX530 hardware (our OMAP3630 test board), from the stage-1 bring-up. |
| **U** | Inferred from the vendor stack: what it sets up for this core, not yet run on our board. |
| **?** | Open. |

## Sources

- The GPL kernel module of an SGX530-rev-125 DDK, built for the OMAP3630 (`SGX530`,
  `SGX_CORE_REV=125`): its reset, init, MMU and host↔microkernel interface. This is the same
  interface the whole family's GPL drivers share, read for what the host writes and what it
  leaves to the user space.
- The matching user space for the same DDK and core: the values it computes for the init
  scripts, the `SGX_BRIDGE_INIT_INFO` fields, and the PDS programs that enter and run the
  microkernel. Recorded here as field meanings and register values.
- Cross-referenced against our own confirmed SGX545 init ([SRVINIT.md](../../SRVINIT.md)), PDS
  encoding ([PDS.md](../../PDS.md)) and the stage-1 SGX530 results
  ([sgx530-stage1.md](sgx530-stage1.md)).

The capture kernel, if a board run is needed: the open GPL SGX530 module's last release is a
6.0-rc7-class tree, and the vendor user space's working run was on a 5.4-class kernel. So a
vendor-stack capture needs a 5.4/6.0-class kernel RAM-booted on the OMAP3630 board.

## 1. What differs from ours, at a glance (all U)

| Item | Vendor | Ours (stage 1, attempt 47) | Bears on gate |
|---|---|---|---|
| PDS program data layout | DS0/DS1 interleaved in **8-word blocks** (§2) | the SGX545's 2-word interleave | 2 |
| EDM task DOUTU word 0 | code base + offset + **bit 21**; bits 22–31 clear | code base + offset + bit 27, or a temp count in bits 24–31 | 1, 3, 4 |
| EDM task DOUTU word 1 | **0x04000000** | 2 | 1, 3, 4 |
| EDM task DOUTU word 2 | **1** (0 only for a task that runs no code) | not set; its DS source lies past our one-row program | 1, 3, 4 |
| First instruction of a task | never `phas` | `phas.i` first, required under our DOUTU words (C) | — |
| Event program | 3 data rows; `0xa6c` = 3, `0xa70` = 0x00013d01 | 1 data row; `0xa6c` = 1, `0xa70` = 6 | ? |
| Microkernel data mapping | `EDM_PROTECT` on every control structure | without it since attempt 45 (a stopgap) | 3 |
| `0xa74` | `(EDM-task limit << 16) \| 0x8200`, default 0x003f8200 | 0x00208200 | ? |
| `0xa58` (event enable) | 0x2204f00f | 0x2204b00f | — |
| Clock-gate control value | 0x00222220 (2D left off) | per the 530 clock table | — |

## 2. The PDS program data layout: an 8-word interleave (C; explains gate 2, and confirmed load-bearing for rendering)

The SGX530 lacks `SGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS` (K). The vendor lays out a PDS
program's data segment so that the two data stores interleave **eight words at a time**, not
two:

- DS0 word *n* lives at memory word `(n >> 3) * 16 + (n & 7)`.
- DS1 word *n* lives at memory word `(n >> 3) * 16 + 8 + (n & 7)`.

This is the only layout under which all of the vendor's event, secondary and loopback programs
decode consistently: every launch word it builds lands on the word it fills, and the
attribute-write control values come out 0 / 0x100 / 0x200 for PA0 / PA1 / PA2, as on the
SGX545. [PDS.md](../../PDS.md)'s 2-word layout is the SGX545's.

**Why this is gate 2.** Our stage-1 DOUTA-then-DOUTU probes (gate 2, C) laid the data out in
the SGX545's 2-word interleave, so the launch word's third source (a DS1 word) fell where the
530 reads a different, zero word — hence the task started at code base 0. The one-row and
pds3-mirror variants failed the same way for the same reason. Under the 8-word interleave the
launch word's sources sit where the 530 reads them. Every PDS program the microkernel uses
depends on this layout: the event entry, the launch programs, and Mesa's vertex and pixel
programs.

**Frame-scale confirmation (2026-11-13, C).** A 64x64 kick host-played on 530 silicon (driver
`edm_init`) rendered nothing: the microkernel started and consumed both kicks but the TA emitted
no tiles. The played command's state-emit program (`frame64.trace`) is the PDS.md template
ending `doutu ds0[4..5]`; its DOUTU source `ds0[4]` is memory word 8 (= 0x0000070a, the task
descriptor) under the 2-word layout but memory word 4 (= 0x00000000) under this 8-word one — so
every USSE launch reads a null source on the 530 and starts at code base 0. The open microkernel
and the command/uAPI layer are proven on silicon; the remaining blocker is sgxgen emitting its
PDS data segments in this 8-word interleave (a codegen/packing change keyed on
`!PDS_DATA_INTERLEAVE_2DWORDS`, not an ISA change).

## 3. The EDM task launch word (DOUTU), all three words (U)

The vendor's microkernel-entry DOUTU differs from ours in every word.

**Word 0** — code base, offset, and one high bit:

- Bits 0–3: the USE code base register, `(15 + (off >> 19)) & 15` for an offset `off` into the
  kernel code heap. Matches our code base 15.
- The in-segment offset is encoded in **16-byte units**: bits 8–18 hold `off[14:4]`, bits 4–7
  hold `off[18:15]`. (For an entry at offset 0, as the event program's, these are 0, so the
  difference only shows for the body entries.)
- Bit 21 (0x200000) is set.
- Bits 22–31 are clear. Our word carries bit 27, or a temporary count in bits 24–31; the
  vendor carries neither there.

**Word 1 = 0x04000000** (bit 26). Ours is 2. This is the single most distinctive difference
and the leading candidate for the write-retire context (gates 1/3/4) and the EDM requestor
tag (gate 3; see §6).

**Word 2 = 1**, for every microkernel task that runs code. The one task with word 2 = 0 is a
launch that runs no instructions (it only lets a preceding attribute DMA complete). That split
mirrors the SGX545, where the only microkernel DOUTU without bit 27 is the one whose task does
no work.

**Register usage of the vendor microkernel tasks:** temporaries 0–31 (32 of them), primary
attributes 0–2 (and the task writes PA2), and secondary attributes 0–32. So the task does have
a working temporary file and a writable `pa` bank — the opposite of our stage-1 result (C,
only outputs retire). Whatever enables that comes from these launch words, since nothing on the
host side sizes the register file (§8). My ranking of the candidates: word 2 first (it is 0
only for the task that needs no temporaries), then word 1, then word-0 bit 21.

## 4. No task begins with `phas` (U)

None of the vendor's USE programs contains a `phas` instruction. Our stage-1 result that
`phas.i` must be the first instruction (C, attempt 26) was measured **under our DOUTU words**;
it need not hold under the vendor's. The board test (§9) keeps `phas.i` first on its first run
and can drop it once the launch words are right.

## 5. The launch sequence (U)

The vendor starts the microkernel in three stages, all through the one event-kick path the 530
has:

1. **The event program** (the PDS program the init script points `0xa68` at) is a 3-data-row
   program holding a single task launch into the microkernel's init entry. Its two control
   words are `0xa6c` = 3 (the code-start row / data-row count, consistent with stage 1's
   reading of `0xa6c`) and `0xa70` = 0x00013d01 (meaning **?**; the SGX545's are 1 and 6).
2. **That init task** reads the control block, then issues two further PDS programs through the
   USE's emit path:
   - a program that DMAs the 0x128-byte TA/3D control block into the task's **secondary
     attributes** (DMA control word 0x01e0003f, which is the SGX545's 0x03c0003f shifted down
     one bit) and then runs a launch that executes no code; and
   - a program that launches the microkernel's init body.
3. **The init body** rewrites `0xa68`/`0xa6c` to point the event path at the steady-state
   "other events" program. That program hands its three inputs to the task in PA0–PA2 with
   attribute writes, then branches to one of four task launches depending on which event
   fired.

The microkernel's stores use addresses held in its **secondary attributes** (filled by the DMA
in stage 2), not in temporaries. Our open event entry uses a reserved temporary as the pointer
base ([UKERNEL.md](../../UKERNEL.md)); on the 530 that base should come from a secondary
attribute instead.

## 6. Stores and `EDMPROTECT`: the task is meant to be the EDM requestor (U)

- The vendor maps **every** microkernel control structure — the CCB and its control word, the
  event kicker, the TA/3D control block, the host control block, the misc buffer, the HWPerf
  buffer and the signature buffers — with `EDM_PROTECT` set in the page flags, on the SGX530
  as on the other cores. So a correctly launched EDM task is expected to write them. (The
  rev-125 errata do not include the one that drops `EDM_PROTECT`.)
- The task's code base 15 is written with the data-master field = 3 in the launch. That agrees
  with our stage-1 finding that the data-master field alone does not admit the stores (C,
  attempt 45): it is necessary in the vendor setup but not sufficient.
- So on the 530 the requestor identity that `EDM_PROTECT` admits must come from the **launch
  context**, and the one field that differs and has no other explanation is **DOUTU word 1 =
  0x04000000** (bit 26). That is the lead for gate 3: our stores fault on `EDM_PROTECT` pages
  (C) because our launch word 1 is 2, not because the data arena cannot be reached. If the
  board test makes stores commit with `EDM_PROTECT` back on, gap 19's stopgap (mapping the
  arena without the flag) can be dropped.

## 7. Init-script part 2 values (U)

The host runs these register writes after the reset, from the script the user space builds.
Offsets the 530 GPL header does not name are given by offset; the values are the vendor's.
Where ours differs, the stage-1 value is shown.

| Register | Vendor value | Note |
|---|---|---|
| `0xa58` (event enable) | 0x2204f00f | ours 0x2204b00f; the vendor also enables the bit at 0x4000 (b14) |
| `0xa68` (event program) | init program offset from `PDS_EXEC_BASE`, `& ~0xf` | a 26-bit offset, as stage 1 found (C) |
| `0xa6c`, `0xa70` | 3, 0x00013d01 | the event program's control words (§5) |
| `0xa74` | 0x003f8200 | `(EDM-task limit << 16) \| 0x8200`; the limit defaults to 0x3f. Ours wrote 0x00208200 |
| `0xa78` | 0 | |
| `0xa00` | 0x7c000 | (0xfc000 when clock gating is disabled) |
| `0xabc` (`PDS`, K) | 0x40 | ours wrote 0xe |
| `0x804` | 0xffff | |
| `0xb30` | 0x100 | |
| `0x800`, `0x810`, `0x814`, `0x818`, `0x4e8` | not written | agrees with stage-1 attempt 37, which found these do not latch on the 530 (C) |
| `0x13c`, `0xa7c`, `0xa80` | 0 | |
| `0x130` (`EVENT_HOST_ENABLE`) | 0x4000 | |
| `0x110` (`EVENT_HOST_ENABLE2`) | 0 | |

Part 1 writes `0xa58` = 0 and `EVENT_TIMER` (`0xacc`) = 0 before the reset.

**Watchdog / clock values** passed in `SGX_BRIDGE_INIT_INFO`:

- EDM-task progress registers 0xb10 and 0xb1c (the host reads and XORs them to tell a making-
  progress EDM from a stuck one). The SGX545's pair is 0xb14/0xb20.
- Clock-gate control value 0x00222220, which leaves the 2D unit's field off (0x00111110 when
  clock gating is disabled).

## 8. What the host side does *not* set up (GPL kernel-side facts, U)

The GPL kernel side's reset and init path writes only: the clock-gate control registers; the
BIF control, 36-bit-addressing disable and memory-arbiter config; the directory-list base; the
EDM's directory list; and the soft-reset register. On this core:

- **The EDM directory list collapses to list 0.** Without `SGX_FEATURE_MULTIPLE_MEM_CONTEXTS`
  there are no bank registers and no `DIR_LIST_BASE1..15`; the reset points the EDM's context
  at `BIF_DIR_LIST_BASE0` (the kernel page directory). TA and 3D share that one list.
- **The reset waits** for `BIF_MEM_REQ_STAT` outstanding reads to reach 0 after the
  directory-cache invalidate (the single-context path). Our reset does not.
- **Nothing on the host side sizes the USE register file, the temporaries, the `pa` bank or a
  USE partition.** There is no such register write in the reset, in either init-script part, or
  in the host↔microkernel init structure. So the register-file / write-retire setup is entirely
  in the PDS launch (§3), which is where this analysis points.

The GPL feature table accounts for every confirmed SGX530-vs-545 difference **except** two that
only hardware showed: `USE_CODE_BASE` counting 256-byte units (C), and the write-retire blocker
(C). Those two are exactly what the launch words in §3 are expected to resolve.

## 9. Next board test (the first thing to try)

One change set, bisectable, on the OMAP3630 board. It addresses gates 1, 3 and 4 together,
because the vendor data says they are one context difference, not three:

1. Lay the event program out in the **8-word interleave** (§2): three data rows, launch word 0
   at data word 0, word 1 at data word 1, word 2 at data word 8.
2. Set the launch words to the vendor's: **word 1 = 0x04000000, word 2 = 1**, and word 0 with
   **bit 21** set and bits 22–31 clear (§3).
3. Set the event control words `0xa6c` = 3 and `0xa70` = 0x00013d01.
4. Map the kernel data arena **with `EDM_PROTECT`** again (§6), to test the store path under the
   vendor's requestor assumption.
5. Keep `phas.i` first on this first run.

Read out, as in stage 1:

- the `aluwr` probe — does an ALU write to a **temporary** now retire (gate 1)? (A fault at the
  OR'd page, not the leftover.)
- the `stout_o0` and `stout_r5` probes — does a **store** commit to DRAM without faulting on an
  `EDM_PROTECT` page (gates 3/4)?

If writes retire, bisect which field did it: word 2 first, then word 1, then word-0 bit 21,
then `0xa70`. Then re-run a single DOUTA-then-DOUTU program under the 8-word interleave to close
gate 2 directly. If the store still does not commit with `EDM_PROTECT` on, word 1 bit 26 is the
next thing to vary on its own.

Board rules for the run: coordinate the slot (the board lock), RAM boot only, never write NAND,
named registers and the already-approved offsets only.

## 10. What a vendor-stack capture would still add

The static analysis gives the vendor's configuration but not a running trace. A capture on the
board (a 5.4/6.0-class kernel with the vendor user space, RAM-booted) would confirm, rather than
infer: that the task's temporaries and `pa` writes retire under these launch words; that its
stores reach `EDM_PROTECT` memory; the register writes in init-script order with their read-back
values; and `0xa70`'s meaning. It is worth doing only if the board test in §9 leaves one of
these open — the test should settle the write-retire and store questions directly.
