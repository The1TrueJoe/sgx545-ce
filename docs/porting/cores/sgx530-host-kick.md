# SGX530 host-kick: driving TA and 3D from the CPU, with no EDM task

**Status: design spec for `powervr_sgx` (sgx530-kernel) to implement. Written
from our own docs only (UKERNEL.md, STATE.md, SRVINIT.md) and the stage-1 board
results. Track C of the SGX530 effort: a fallback that sidesteps the EDM
write-retire blocker while §9 (track A) and a vendor capture (track B) run in
parallel.**

Each step is tagged for the SGX530:
- **C** — confirmed on the OMAP3630 board (a stage-1 result, or the clean-boot /
  GETMISCINFO runs).
- **U** — derived from the SGX545 docs / GPL headers; consistent and expected to
  hold, but not yet exercised on the 530.
- **?** — unknown on the 530: a register offset or value that is a gap-13
  unknown (UKERNEL.md §6 names the 545 offset; the 530's TA/3D/DPM/ISP/TSP/PBE/PDS
  registers are unconfirmed), or a behavior not yet seen.

---

## 1. The idea, and why it works on the 530

The stage-1 result (sgx530-state.md): on the SGX530, **an EDM task's writes do
not retire** — temporaries, the `pa` bank and memory stores are dropped; only
ALU writes to the eleven output registers `o0..o10` retire, and `str` to an
`EUR_CR_*` register is **dropped (C)**. So a task cannot program registers,
cannot write the sync heap, and cannot even set `ui32InitStatus` (the DDK init
task's one job). The whole EDM-mediated model is therefore blocked until the
write context is solved (tracks A/B).

But the **host** path is unaffected. Stage 1 confirmed the host writes
`CLKGATECTL`, `SOFT_RESET`, `BIF_CTRL`, the init-script registers, the USE code
bases and the event registers, and the core boots from them (C, sgx530-stage1.md
"The host writes…"; the clean-boot and GETMISCINFO-over-CCB runs). The
write-retire blocker is specific to the EDM task's USSE write path, not to the
CPU's MMIO path.

**Host-kick keeps only the two hardware pipelines on the GPU — the TA
(VDM/TE/MTE tiling) and the 3D (ISP/TSP/PBE pixels), both of which are started
by register writes and run in hardware, not in EDM code — and moves everything
the EDM microkernel did (program registers, wait on events, write sync objects,
advance the CCB) onto the CPU.** The CPU already does all three kinds of access
the EDM cannot: MMIO register writes, DRAM writes, and `EVENT_STATUS` reads /
`EVENT_HOST_CLEAR` acks (it does them in init and in `sgxreset.c`'s HWR path).

Consequence worth stating plainly: in this mode **no EDM task runs at all** — not
even the init task. The microkernel image and the event PDS program are not on
the render path. The host considers init complete because it performed init; it
does **not** wait on `ui32InitStatus` (which a task would have to write, and
can't). This is not an optimization layered on a working driver; on the 530 it is
the only path to a first render while the blocker stands.

---

## 2. What the host must own (the EDM's old responsibilities)

From UKERNEL.md §3/§5, the EDM owned, and the CPU must now own:

1. **The command source.** Instead of the kernel CCB + event-kick doorbell
   (UKERNEL.md §3), the host reads its own submit queue directly — it is the CPU.
   No `EVENT_KICKER`/`EVENT_KICK` ring is needed for rendering. **U.**
2. **The firmware-visible structures.** The host already owns them under the
   UAPI threat model (AGENTS.md §3.3): HW render context, RT data, PB descriptor,
   free lists, sync/status words. In host-kick it also *reads* them to source the
   register values — it is both writer and reader. **C** (host DRAM access).
3. **Per-job register programming** — the TA and 3D sequences of §3/§4 below. **?**
   on the 530 for the TA/3D/DPM/ISP offsets.
4. **Event handling** — poll `EVENT_STATUS`, ack via `EVENT_HOST_CLEAR`,
   demultiplex TA_FINISHED / PIXELBE_END_RENDER / DPM_3D_MEM_FREE / the OOM bits
   (§5). **U.**
5. **Completion** — write the HW device sync list (`ui32WriteOpsComplete += 1`,
   UKERNEL.md §5), tear the render down (clear the in-flight slot), signal the
   client's fence. All CPU DRAM writes. **C** (the writes the EDM task could not
   make, the CPU makes normally).

---

## 3. TA start (the host sequence)

Source: UKERNEL.md §6 "TA start" and §3 "TA kick"; register meanings from the
`sgx545defs.h` map in §6. The host does, for the first kick of a scene:

1. **Point the BIF at the client context.** Write the context's page-directory
   address into `BIF_DIR_LIST_BASE0` (0xc84), pulse `BIF_CTRL` (0xc00) INVALDC
   (bit 3: old|8 then old). On the 530 there is **one** memory context: only
   `DIR_LIST_BASE0` exists, every requestor (there is no separate EDM list)
   translates through list 0, and the firmware heaps are mapped into the client
   PD (sgx530.py `MULTI_MEM_CONTEXTS=False`; `BIF_DIR_LIST_BASE1`/`BANK0` absent).
   So the 530 host-kick skips all `BIF_BANK0` nibble routing the 545 does. **C**
   for `BIF_CTRL`/`DIR_LIST_BASE0` (host writes them); **U** that list 0 alone
   suffices.
2. **Reset the TA unit.** Pulse `SOFT_RESET` (0x080) TA bit, then 0. On the 530
   the TA bit is **0x8** (b3), not the 545's 0x4 — the 2D unit takes b1 and every
   unit above the BIF shifts up (sgx530.py `SOFT_RESET_TA=0x8`, K). **C.**
3. **Honor the kick's cache bits** (TA flags b4–b6, STATE.md §1): the matching
   BIF/USE-cache invalidate pulses (§6 "BIF cache invalidate"; `0xa08`=1 for the
   USE cache, wait `0x138 & 0x27`, ack `0x140`, wait `EVENT_STATUS` b26). **?**
   (offsets/done-bits are 545 values; 530 `EVENT_STATUS2` differs, see §5).
4. **DPM clear for a fresh scene.** `0x694`=`0x698`=2, wait `EVENT_STATUS` 0x240
   (STATE_CLEAR b6 | CONTROL_CLEAR b9), ack `0x134`=0x240 (§6). **?** (DPM offsets
   gap-13).
5. **Free-list load, first kick of a PB only** (§6 "TA free-list load"), from the
   PB descriptor words (STATE.md §6): `0x618=pb[4]&~0xf`, `0x61c=pb[6]`,
   `0x648=pb[7]`, `0x638=pb[8]|pb[9]<<16`, `0x620=pb[10]`, `0x668=pb[13]`,
   `0x624=pb[11]`, `0x628=(pb[12]&0xffff)|0x10000`, `0x664`=the marker mask,
   `0x684`=1, wait `EVENT_STATUS2` **b0**. **?** (offsets); **C** that 530
   `EVENT_STATUS2` has b0 (DPM_TA_FREE_LOAD) (sgx530.py).
6. **Layout registers** from the TA command tail (cmd +0x238..+0x254 → 0x204–0x218,
   0x238/0x23c: `TE_AA`, `TE_MTILE*`, `TE_SCREEN`, VDM control-stream base,
   region-header / tail-pointer bases, the two clamp floats; §3, §7). **?**.
7. **PDS and USE code bases** for the context, then **kick the VDM** to start
   fetching the control stream (§6). The USE code base on the 530 counts
   **256-byte units (addr>>8)**, not the 545's >>7 (confirmed per-core fix). **?**
   for the VDM kick register; **C** for the >>8 code-base rule.

---

## 4. 3D start (the host sequence)

Source: UKERNEL.md §6 "3D start", §5 "DPM 3D memory free".

1. **Point the BIF at the context** as in §3.1 if it changed. **C/U.**
2. **3D free-list load, first kick of a PB only** (§6): `0x600=pb[4]&~0xf`,
   `0x604=pb[6]`, `0x64c=pb[7]`, `0x660=pb[8]|pb[9]<<16`, `0x664`=mask (5 on a
   first frame), `0x680`=1, wait `EVENT_STATUS2` **b1** (DPM_3D_FREE_LOAD, present
   on the 530). **?** (offsets); **C** (b1 exists).
3. **Save the TA's live counters into the PB descriptor** before the 3D
   (`save_ta_state`, §6): `pb[6]=0x700`, `pb[7]=0x710<<16`, `pb[8]`=hi(`0x724`),
   `pb[9]`=hi(`0x72c`). With one shared PB and one context this is inert after the
   first frame (T==D==P). **?**.
4. **Arm the page release.** `0x63c`=3 (0 if render details +0xd4 b3). The
   hardware frees the TA's pages while the 3D runs; nothing is written after the
   3D end to start it (§5, §6). **?**.
5. **ISP / PBE registers** from the render-details block (the
   0x400/0x414/0x41c/0x42c/0x480–0x4c8/0x800/0xa5c–0xa64 group; full source map in
   UKERNEL.md §7), the ISP background depth, the end-of-tile PDS program, and the
   per-render `0x480` sample/visibility bits. **?**.
6. **Background-block write.** The 545 opens a BIF window (`0x128b`) to write the
   RT entry's background block through the client PD at every 3D start. On the
   530, with one memory context the client data is already in list 0, so the host
   writes the background block itself (CPU DRAM, like `HOSTCTL_HOST_BGBLOCK`,
   UKERNEL.md §6) — no window, no EDM. **C** (CPU DRAM write); **U** (that list-0
   mapping covers it).
7. **Kick the 3D.** **?** for the kick register.

---

## 5. Events and acks (TA done, 3D done)

The host polls `EVENT_STATUS` (0x12c) and acks by writing 1s to `EVENT_HOST_CLEAR`
(0x134). Bit meanings are the 545 header's (§6 register map); confirm each on the
530.

| event | `EVENT_STATUS` bit | host action | 530 tag |
|---|---|---|---|
| TA finished | b13 TA_FINISHED (0x2000) | write the TA command's status values to their sync-heap addresses (CPU DRAM); if the scene's last kick, go to the 3D start (§4) | **U** |
| 3D end | b18 PIXELBE_END_RENDER (0x40000) | wait for its pair b0 DPM_3D_MEM_FREE (0x1); then the completion below | **U** |
| out of memory | b1 _MT (0x2) / b2 _GBL (0x4) | the OOM path, §6 | **?** |

**Completion** (UKERNEL.md §5), all CPU writes: walk the render's HW device sync
list and set each destination `ui32WriteOpsComplete` to the client's pending
value + 1; clear the in-flight slot (render-details state +0x2a4 → 2, active flag
+0x12c, write-ops +0x108, RT-data entry released); signal the client fence. **C**
(the EDM-task writes that didn't commit are ordinary CPU DRAM writes here).

**The 3D-end ISP clock poll** (§5): when `CLKGATECTL & 3 == 2` (ISP clock on
demand), spin on `CLKGATESTATUS` (0x008) b0 until 0 before completing. Host-side
register poll. **U.** On the 530 `CLKGATECTL`'s ISP field is b4–5 and
`CLKGATESTATUS` ISP is b4 (sgx530.py), **not** the 545's b0 — adjust the mask.
**C** (the 530 clock-gating field positions).

**Interrupt vs poll.** The DDK enables only SW_EVENT in `EVENT_HOST_ENABLE`
(0x130=0x4000, SRVINIT.md) because the EDM raised SW_EVENT to signal the host.
In host-kick there is no SW_EVENT; the host must either **poll** `EVENT_STATUS`
or **enable the real hardware bits** (TA_FINISHED, PIXELBE_END_RENDER,
DPM_3D_MEM_FREE, the OOM bits) in `EVENT_HOST_ENABLE` so they raise the SGX
interrupt line, and service them in the ISR. Polling is the simplest first cut.
**U** (either works; the enable register is host-written, C).

---

## 6. Out of memory (the hard case)

This is the one place the EDM did real mid-pass work (UKERNEL.md §5 "Out of
memory"), and the hardest part to lift to the host, because it pauses a **running**
TA, renders the partly-binned scene, and resumes. The full DDK sequence is in
UKERNEL.md §5 (stop the TA `0x24c`=1 wait `EVENT_STATUS` 0x100000; save `0x720`
and the counters; `0x69c`=4; pause `0xa74`|=1; TA_TERMINATE `EVENT_STATUS` b12 ack
0x1000; the partial-render 3D start with the region-header redirect `0x50c`=`0x408`;
resume `0xa74`&=~1, `0x69c`=1). All of it is register writes the host can make —
**but** it must interleave with a live TA and is timing-sensitive, and the
offsets are gap-13 unknowns. **?** throughout on the 530.

**Recommended staging:** do **not** implement OOM in the first cut. Create the PB
large enough that small scenes never hit a threshold (the DDK's scenes never
raise an OOM event — UKERNEL.md §5: "None of the DDK traces has an out-of-memory
event"). Size the PB generously, and treat an OOM event as a hard error (log,
reset) until §3/§4 are working. Add the partial-render path only once a plain
frame renders end to end. On the 530 the OOM offsets will need the same
one-at-a-time confirmation as the rest.

---

## 7. 530-specific deltas (collected)

| item | 545 | 530 | tag |
|---|---|---|---|
| `SOFT_RESET` TA bit | 0x4 (b2) | **0x8 (b3)** — 2D takes b1 | C (sgx530.py, K) |
| `EVENT_STATUS2` | b0..b15 | **only b0 (DPM_TA_FREE_LOAD), b1 (DPM_3D_FREE_LOAD)** | C (sgx530.py) |
| OTPM clear wait | `EVENT_STATUS2` b12 | **`EVENT_STATUS` b20 (0x100000)**; b21 is the backup | U (sgx530.py) |
| `USE_CODE_BASE` unit | addr>>7 | **addr>>8** (256-byte units) | C (per-core fix) |
| memory contexts | lists 0–15, EDM on 15 | **one list (list 0)**; `DIR_LIST_BASE1`/`BANK0` absent; firmware heaps mapped in the client PD | C (sgx530.py, K) |
| `CLKGATECTL` ISP field | b0–1 | **b4–5**; `CLKGATESTATUS` ISP b4 | C (sgx530.py) |
| TA/3D/DPM/ISP/TSP/PBE/PDS register offsets | §6 (545) | **unknown (gap 13)** | ? |

The host-side control registers host-kick leans on — `CLKGATECTL` (0x000),
`SOFT_RESET` (0x080), the `EVENT_*` block (0x110–0x134), `EVENT_KICK` (0xac8),
`BIF_CTRL` (0xc00), `BIF_DIR_LIST_BASE0` (0xc84), `USE_CODE_BASE_15` (0xa48) — are
in the GPL `sgx545defs.h` and are written by the host in the 530 init path today
(SRVINIT.md "Init scripts"; sgx530-stage1.md). Those are **C**. The gap-13 unknowns
are the per-pass TA/3D/DPM/ISP offsets, which the implementation confirms one at a
time the way stage 1 did (microkernel.md), using the 545 sequence as the template.

---

## 8. Implementation order (a day-sized first cut)

Minimal target: one context, one clear scene, generous PB, no OOM, no FBO
interleave, polling.

1. **Init without the task.** Run SRVINIT.md parts 1+2 with the §7 deltas
   (`USE_CODE_BASE` >>8, etc.). Do **not** kick the event program and do **not**
   wait on `ui32InitStatus`. Consider the GPU initialized. **C-ish** (host writes
   are confirmed; skipping the task is the new part).
2. **Plumb host register access** for the per-pass sequences — the driver already
   maps the SGX register BAR.
3. **TA start** (§3) → poll TA_FINISHED (§5) → **3D start** (§4) → poll
   PIXELBE_END_RENDER + DPM_3D_MEM_FREE (§5) → **completion** (§5).
4. Confirm each gap-13 register offset on the board as you reach it (stage-1
   method). The 545 sequence is the template; the 530 value is what you verify.
5. Only then: OOM (§6), FBO interleave (§6/UKERNEL.md §6 "two DPM slots"), and
   moving from polling to the hardware-bit ISR.

A plain frame rendering end-to-end on the 530 with no EDM task is the exit
criterion for this track — and the proof that the write-retire blocker is not on
the critical path for a working driver.

---

## 9. Open questions

1. **The kick registers.** The VDM kick (TA) and the 3D kick are `?` on the 530.
   Their 545 offsets/values must be confirmed, or read from the 530 GPL header if
   named there.
2. **List 0 for every requestor.** Does the 530 need any `BANK`-style routing at
   all, or does `DIR_LIST_BASE0` alone serve the TA, 3D and (absent) EDM? (U.)
3. **Event enable on the 530.** Which `EVENT_STATUS` bits the 530 raises to the
   host line, and whether `EVENT_HOST_ENABLE` masks them the same way (U).
4. **Does any pass need the EDM at all?** The premise is that TA and 3D are pure
   register-kicked hardware pipelines. Confirm no sequence (DPM load, OTPM) must be
   issued *by* the EDM rather than the host — they are register writes, so this is
   expected to be fine, but it is unverified on the 530.
5. **Interaction with tracks A/B.** If §9 (or a capture) makes EDM-task writes
   retire, the driver can move back to the EDM-mediated model; host-kick then
   stays as a single-context / recovery fallback. Decide whether it is worth
   keeping once the EDM works.

---

## 10. References

- `docs/UKERNEL.md` §3 (host-kick handlers, TA kick), §5 (completion, the host
  interrupt, out of memory), §6 (register sequences: TA start, 3D start, DPM,
  free-list loads, BIF), §7 (register sources).
- `docs/STATE.md` §1 (the TA command), §6 (the PB descriptor).
- `docs/SRVINIT.md` "Init scripts" (parts 1/2, event enables, deinit).
- `docs/porting/cores/sgx530-state.md` (the write-retire result: outputs retire,
  `str`-to-`EUR_CR` dropped).
- `docs/porting/cores/sgx530-stage1.md` ("The host writes…"; the gap-13 register
  list; the per-core fixes).
- `ukernel/cores/sgx530.py` (the confirmed 530 register deltas).
