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
init entry. For the 530 that event program keeps its **bare-DOUTU** shape — it only
changes to the §9 DOUTU words and the 8-word data layout. **No second-stage DOUTD
and no `sa` population are needed** (confirmed from the driver source and the probe
runs, below):

1. **The event program** (driver-built PDS0): a bare DOUTU — the 8-word interleave
   carrying the three DOUTU words at data words 0, 1, 8 (DS0[0]=0x0020000F,
   DS0[1]=0x04000000, DS1[0]=0x00000001), one MOVS→DOUTU, HALT; `0xa6c`=3,
   `0xa70`=0x00013d01; the data arena mapped `EDMPROTECT`. Launches the microkernel's
   init entry at **offset 0** (code base 15, bit 21 → word0 0x0020000F). This is the
   exact program the §9 probes used (`pvrsgx_pds.c`, the `stage1_vlaunch` branch),
   just pointed at the real init entry instead of a probe. **C** — the probes ran it.
2. **The init task** derives its pointers from **r17 = ctlblk** (loader-patched,
   `a.patch(17,"ctlblk")`), **not** from `sa`. The event program is a bare DOUTU with
   no DMA, so `sa` is empty on entry; the handlers read host-control / CCB pointers
   from r17, exactly as the existing microkernel does (ukernel.py's event-entry
   comment). The init task reads those pointers (`ld`), installs the steady-state
   event-program address and control words (0xa68/0xa6c/0xa70) with `str`, tests the
   HWR flag, and **ends** — it does **not** write `INIT_COMPLETE`, because that store
   drops on the 530 and the host does not wait on `ui32InitStatus` (it considers init
   done because it performed the register program and the kick). **C** — r17, `ld`,
   `str` all confirmed under §9; skipping the status wait is the one change.

**Why no DOUTD / no `sa`:** the write-context that makes temps/loads/`str` retire is
**entirely the §9 DOUTU words** (word2=1 + word0 bit21), not a DMA. All three probes
(xorr5, ldret, limmwr) ran under the bare-DOUTU §9 launch with `sa` empty and
r17-derived pointers, and every one retired. `pa`, where a handler needs it (e.g. the
3D background block), is written by a **per-handler DOUTA**, not an init-wide DMA.

`ukernel/` provides the init-entry USSE code at event offset 0 and the body entries;
the **driver** builds the (bare-DOUTU) event PDS and the launch words. (This is the
same split as §9's board test: the probe images carry only USSE code; the driver
carries the PDS/launch — so the SGX545 golden is untouched.)

---

## 3. The microkernel's reduced job (stateless: no committing stores)

Each handler becomes: **load the command into temps → program registers → kick →
wait on events → ack**, with every memory write removed.

**The microkernel is stateless.** Because `st.a.d` drops, the microkernel cannot
persist *anything* between events — not the sync objects, and not its own
coordination state: the CCB **read offset** (advanced in `kick_adv` via `st.a.d`),
the TA-in-flight / render-in-flight markers, the DPM slot markers. All of that is
host-owned. Each event is **self-contained**: the handler reads the current job and
the host-maintained state (`ld`), programs registers (`str`), kicks, and ends. It
writes no memory. For a single-context, one-job-at-a-time path this is enough,
because the event *type* plus the current CCB command fully determine the action —
the DDK's markers exist for multi-render bookkeeping that a single job does not need:

| event | handler action (all `ld` + `str`, no `st`) |
|---|---|
| host kick (`EVENT_KICK`) | read the command at the host's CCB read offset → `do_ta` (program + kick the TA) |
| `TA_FINISHED` | read the same current command's render details → `do_3d` (program + kick the 3D) |
| `PIXELBE_END_RENDER` + `DPM_3D_MEM_FREE` | the render is done — the **host** IRQ signals the fence and advances the CCB read offset |

So the **host advances the CCB read offset** (on the completion IRQ), not the
microkernel; the read offset the microkernel reads on the next kick is the host's.
This is the one piece of §4 to get exactly right — without it the microkernel
re-reads the same command forever.

- **Load the command.** `ld` the kick's client fields and the control-block words
  the handler needs into temporaries (`ldret` = C). The base pointer is **r17 =
  ctlblk** (loader-patched); other addresses are read from there or built with `limm`
  (C). No `sa` is involved.
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

- **The kernel CCB ring** and **both** its offsets. The host bumps the write offset
  on submit, and — crucially — **advances the read offset on the completion IRQ**,
  because the microkernel's `kick_adv` `st.a.d` drops. The microkernel only reads the
  read offset to find the current command; it never advances it.
- **The control block, sync objects, RT data, PB descriptor, free lists** — the
  host writes and reads them; the microkernel reaches the control block through
  r17 = ctlblk (loader-patched), so no launch-time DMA into `sa` is needed (§2).
- **Completion / fences:** the host IRQ reads `EVENT_STATUS` and signals
  `dma_fence`; it writes the sync-heap words.
- **Recovery:** the `drm_sched` per-job timeout is the watchdog; a job that doesn't
  raise its completion event is declared hung → devcoredump + reset.
- **The bare-DOUTU event program and launch words** (§2), and the per-context
  register setup the kick needs.

---

## 5. Division of labour

| piece | owner | note |
|---|---|---|
| microkernel USSE code (init entry, handlers, reduced to load/str/kick/ack) | `ukernel/` | no `st.a.d`; `pa` via DOUTA |
| event PDS (bare DOUTU, 8-word interleave, §9 DOUTU words) | driver | §9 launch; no DOUTD / no `sa` |
| the 530 TA/3D/DPM/ISP register offsets | driver, confirmed one at a time | gap-13 worksheet |
| CCB ring, control block, sync objects, PB, free lists | driver (host) | host-owned memory |
| completion → `dma_fence`, recovery | driver (host IRQ + drm_sched) | no microkernel store |

---

## 6. Implementation order

1. **Confirm the two open register-file facts** on the board: `sgx530_ldret`
   (loads retire) and `sgx530_limmwr_r5_5c3` (str/limm, fresh constant — rules out
   the EVENT_TIMER residual). Both needed before relying on the path.
2. **Init without the status wait:** point the §9 bare-DOUTU event program at the
   real init entry (r17 = ctlblk patched); do not wait on `ui32InitStatus`. No DOUTD,
   no `sa`. Update `ldret` (step 1) confirmed; now `str`-install the steady-state
   event program and end.
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
- **`pa` via per-handler DOUTA:** the handlers that need a primary attribute (the 3D
  background block) issue a DOUTA; `?` on the exact 530 DOUTA control word, worked out
  when that handler is built. (This replaces the vendor's init-wide DOUTD, which the
  open microkernel does not use.)

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

## 6a. Which handlers actually need reduction (from reading the code)

Not all handlers need the stateless reduction — most of the kick path is
command-driven and survives the marker drops. The reduction target is the
completion rendezvous.

- **`do_ta` / `ta_finished` / `do_3d` — work AS-IS for the kicks (U; confirm on the
  board).** `ta_finished` starts the 3D on the TA command's `bLastInScene` flag
  (cmd+0x04 bit 1), not a microkernel marker — `test.and.ne p0, r2, #2; (p0) ba
  do_3d`. `do_ta`/`do_3d` program the hardware with `str` (C). Their marker *writes*
  (TA-in-flight, DPM slot, TD_ENTRY) drop, but for one job at a time that is the
  correct "always fresh" state, so the dropped markers do not change the kick. So the
  existing image should kick TA+3D on the 530 with no USSE change; the board confirms.
- **The completion rendezvous DOUBLE-COMPLETES on the 530 (C from the code).** `do_3d`
  arms `WAIT_3D = WAIT_FREE = 1` with `st.a.d` (drops), so both stay 0. `pixel_done`
  clears `WAIT_3D` and reads `WAIT_FREE` (0 → "free already done" → complete);
  `free_done` clears `WAIT_FREE` and reads `WAIT_3D` (0 → "writeout done" → complete).
  Both halves complete → double `dma_fence` signal + double teardown. **This is the
  handler to reduce.**

**The completion reduction (replaces the "reduce do_ta" plan):** take completion on
the host. The reduced `pixel_done` (PIXELBE_END_RENDER = render done) acks its event
and raises SW_EVENT (`str` EUR_CR_EVENT_STATUS = 0x4000, commits); `free_done`
(DPM_3D_MEM_FREE = pages freed) just acks. Neither runs the rendezvous or
`render_complete`. The host's SW_EVENT IRQ signals the oldest job's `dma_fence` and
advances the CCB read offset — exactly once per render. This is completion-coupling
option (a), sized to one SW_EVENT per render so there is no double-complete.
