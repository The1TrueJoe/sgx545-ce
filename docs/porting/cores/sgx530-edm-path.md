# SGX530 bring-up: the §9 launch in real init, with host-owned memory

**For sgx530-kernel to implement. The architecture, decided on hardware
(2026-11-12): the open microkernel runs as a real EDM task on the SGX530 under the
§9 vendor launch, with the kernel driver owning every firmware-visible structure
and signalling completion from the host IRQ.** Host-kick
([sgx530-host-kick.md](sgx530-host-kick.md)) stays the fallback but is not needed.

Tags for the 530: **C** confirmed on the OMAP3630 board, **U** derived, **?** open.

---

## 1. Why this path works (the §9 write-path facts)

Under the §9 launch the EDM task can do everything needed to **drive** the
hardware, and the one thing it cannot do (commit arbitrary memory stores) is the
host's job anyway in the uAPI model.

| capability | under §9 | evidence |
|---|---|---|
| temporaries retire (compute, scratch) | **yes** | C — `xorr5` = 0x00a90000 |
| `limm` (load constants) | **yes** | C — `limmwr_r5` → EVENT_TIMER 0xa5a |
| `str` (program `EUR_CR` registers) | **yes** | C — same run |
| `ld` deposits into a temp (read CCB/ctlblk) | expected yes | ? — `sgx530_ldret` pending |
| `pa` writes | via **DOUTA** (attribute path), not ALU | C — standalone DOUTA writes pa |
| `st.a.d` commit arbitrary memory | **no** (silently drops) | C — `stor5`; survivable (host owns memory) |

So the task **computes, loads constants, loads memory into temps, and programs
registers** — enough to point the BIF, program the DPM, and kick the TA and 3D. It
does **not** persist state to memory; the host does.

**Minimal launch (bisect, C):** `word2 = 1` **and** `word0 bit21`; `word1` is free
for temp retire. Keep the vendor's full set so the launch is SGXMKIF-compatible and
in case a later store path wants `word1` bit26:
- DOUTU word 0 = **0x0020000F** (code base 15, bit21; offset 0 for the init entry;
  body entries encode their offset in bits 4–18 per vendor-setup §3).
- DOUTU word 1 = **0x04000000**, word 2 = **1**.
- Event PDS in the **8-word interleave**; `0xa6c` = 3, `0xa70` = 0x00013d01.
- Data arena mapped with `EDMPROTECT`.

---

## 2. The launch, folded into real init

Today `srvinit`/the driver kicks a bare-DOUTU event program at the microkernel's
init entry. For the 530 the event program and the DOUTU become the §9 ones, and the
init task gains the vendor's two-stage body (vendor-setup §5):

1. **The event program** (driver-built PDS0): the 8-word interleave carrying the
   three DOUTU words at data words 0, 1, 8 (DS0[0], DS0[1], DS1[0]); `0xa6c`=3,
   `0xa70`=0x00013d01. Launches the microkernel's init entry (code base 15, offset
   0 → word0 0x0020000F). **Driver-side; ? on the exact per-context values.**
2. **The init task, stage 1:** reads the control-block / CCB / host-control
   addresses from `sa` (they arrive via the DMA below), installs the event-program
   address and control words (0xa68/0xa6c/0xa70), tests the HWR flag, writes
   `INIT_COMPLETE`… **except** `INIT_COMPLETE` is a memory store, which drops on the
   530. So the host does **not** wait on `ui32InitStatus`; it considers init done
   because it performed the register program and the kick. **C-ish** (the host
   writes work; skipping the status wait is the change, shared with host-kick §8).
3. **The init task, stage 2 (the vendor's second PDS program):** a **DOUTD** DMAs
   the 0x128-byte TA/3D control block into the task's **secondary attributes**, and
   attribute writes seed PA0–PA2. This is what gives the running task its CCB /
   control-block pointers and its `pa` bank — via the DMA/attribute path, which
   works, not via `st`/ALU, which don't. **U; the DOUTD/DMA control word from
   vendor-setup §5 (0x01e0003f).**

`ukernel/` provides the init-entry USSE code at event offset 0 and the body
entries; the **driver** builds the event PDS, the DOUTD program, and the launch
words. (This is the same split as §9's board test: the probe images carry only USSE
code; the driver carries the PDS/launch — so the SGX545 golden is untouched.)

---

## 3. The microkernel's reduced job (no committing stores)

Each handler becomes: **load the command into temps → program registers → kick →
wait on events → ack**, with every memory write removed.

- **Load the command.** `ld` the kick's client fields and the control-block words
  the handler needs into temporaries (needs `ldret` = C). The addresses come from
  `sa` (the DMA'd control block) and from constants via `limm`.
- **Program registers.** `str` the BIF (DIR_LIST_BASE0 + INVALDC), the DPM
  (free-list load, state/control, OTPM), the ISP/PBE 3D-start group, and the TA/3D
  kick — all the sequences in UKERNEL.md §6, which are register writes. `str`
  works (C). The 530 register offsets are the gap-13 set
  ([sgx530-gap13-registers.md](sgx530-gap13-registers.md)).
- **`pa` via DOUTA.** Where a handler needs a primary attribute (e.g. the 3D
  background block), it uses the attribute/DOUTA path, not an ALU write.
- **Wait and ack.** `ldr` reads `EVENT_STATUS`; `str` writes `EVENT_HOST_CLEAR`.
  Both are register ops; both work.
- **No `st.a.d`.** Every memory store the DDK microkernel does — sync-object
  writes, control-block status (render-in-flight, DPM markers), wait-timeout IDs —
  is **removed** and owned by the host (§4).

**Completion** is the hinge of the no-store design. The DDK microkernel writes the
sync objects then raises SW_EVENT. On the 530 instead:
- the hardware raises the real completion events (`EVENT_STATUS` PIXELBE_END_RENDER
  b18, DPM_3D_MEM_FREE b0; TA_FINISHED b13);
- the microkernel may still `str` SW_EVENT (0x4000) to nudge the host, or the host
  takes the hardware completion interrupt directly;
- the **host IRQ handler** reads `EVENT_STATUS`, signals the `dma_fence`, advances
  its own job accounting, and writes any sync-heap words a client waits on.
No microkernel memory write is on this path. Progress reporting is covered in
[sgx530-stage1.md](sgx530-stage1.md) "Reporting progress without stores": done_seq
is the host counting completion IRQs, the CCB ring is host-owned, liveness is the
`drm_sched` timeout, and a mid-job tag (only if several jobs are in flight) is a
one-word register mailbox via `str`.

---

## 4. What the host (kernel driver) owns

Everything firmware-visible, per the uAPI threat model (AGENTS.md §3.3), now also
because the microkernel cannot write it:

- **The kernel CCB ring** and its read/write offsets. The microkernel processes one
  kick; the host tracks submit/retire by fence, not by a microkernel-written offset.
- **The control block, sync objects, RT data, PB descriptor, free lists** — the
  host writes and reads them; it DMAs the control block into the task's `sa` at
  launch (§2.3).
- **Completion / fences:** the host IRQ reads `EVENT_STATUS` and signals
  `dma_fence`; it writes the sync-heap words.
- **Recovery:** the `drm_sched` per-job timeout is the watchdog; a job that doesn't
  raise its completion event is declared hung → devcoredump + reset.
- **The event program, DOUTD, and launch words** (§2), and the per-context register
  setup the kick needs.

---

## 5. Division of labour

| piece | owner | note |
|---|---|---|
| microkernel USSE code (init entry, handlers, reduced to load/str/kick/ack) | `ukernel/` | no `st.a.d`; `pa` via DOUTA |
| event PDS (8-word interleave), DOUTU words, DOUTD control-block DMA | driver | §9 launch, vendor-setup §5 |
| the 530 TA/3D/DPM/ISP register offsets | driver, confirmed one at a time | gap-13 worksheet |
| CCB ring, control block, sync objects, PB, free lists | driver (host) | host-owned memory |
| completion → `dma_fence`, recovery | driver (host IRQ + drm_sched) | no microkernel store |

---

## 6. Implementation order

1. **Confirm the two open register-file facts** on the board: `sgx530_ldret`
   (loads retire) and `sgx530_limmwr_r5_5c3` (str/limm, fresh constant — rules out
   the EVENT_TIMER residual). Both needed before relying on the path.
2. **Init without the status wait:** fold the §9 event program + DOUTU + the
   DOUTD control-block DMA into the real init; do not wait on `ui32InitStatus`.
3. **A minimal TA/3D kick** from the EDM task: load the client command into temps,
   `str` the BIF/DPM/ISP sequences (confirming each gap-13 offset as you reach it),
   kick, and let the host IRQ signal completion. Strip the microkernel's `st.a.d`
   writes; move their state to the host.
4. **A plain frame** end to end: EDM task drives TA+3D, host owns memory and signals
   the fence. This is the exit criterion — the open microkernel rendering on the 530.
5. Then the fuller path (committed stores via the SA route, `stor_sa`) only if a
   later feature needs in-microkernel memory writes; the single-context path does not.

---

## 7. Open items

- **`ldret`** (loads retire) and **`limmwr_r5_5c3`** (str/limm, fresh constant):
  queued for the next board slot. The path assumes both pass; `str`/`limm` are
  already C from the 0xa5a run, the 5c3 run just removes the residual doubt.
- **`stor_sa`** (does an SA-held store commit): informative but off the critical
  path — run it in the same slot, but the EDM path does not depend on it.
- **gap-13 register offsets:** the TA/3D/DPM/ISP writes the microkernel `str`s are
  the longer pole; confirm cheapest-first per the worksheet.
- **DOUTD control word / `sa` layout** for the control-block DMA (vendor-setup §5):
  `?` on the exact 530 encoding; the driver works it out at init bring-up.

## 8. References
- [sgx530-stage1.md](sgx530-stage1.md) — the write-path results, the bisect, the
  progress-without-stores design.
- [sgx530-state.md](sgx530-state.md) — the one-page state, updated for §9.
- [sgx530-vendor-setup.md](sgx530-vendor-setup.md) — the §9 launch, the DOUTD DMA.
- [sgx530-host-kick.md](sgx530-host-kick.md) — the fallback, and the shared
  init-without-task and gap-13 mechanics.
- [sgx530-gap13-registers.md](sgx530-gap13-registers.md) — the register-offset
  confirmation order.
- `docs/UKERNEL.md` §3/§5/§6 — the handlers and register sequences the EDM task runs.
