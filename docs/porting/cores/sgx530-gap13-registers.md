# SGX530 gap-13 register confirmation: cheapest-first worksheet

**For sgx530-kernel, to narrow the longer pole of host-kick
([sgx530-host-kick.md](sgx530-host-kick.md)) and of EDM bring-up generally.**
Which per-pass registers the 545 docs name but the 530 has not confirmed, and the
cheapest order to confirm each on the OMAP3630 board.

The problem (fact sheet [sgx530.md](sgx530.md), "Register map differences"): the
GPL header names only the registers the host kernel touches. **The TA, 3D, DPM,
ISP, TSP, PBE and PDS state registers the per-pass sequences program are in
neither core's header** (gap 13). On the SGX545 we know their offsets from the
DDK microkernel binary and hardware captures (UKERNEL.md §6/§7). On the 530 those
offsets are the strongest hypothesis but are **U** — unconfirmed.

**One encouraging fact:** every confirmed 545→530 offset move is in the
**0x00–0x2c identity/clock block** (CLKGATESTATUS, CORE_ID, CORE_REVISION, the
designer fields) plus `PDS_INV3` and the dead `EVENT_KICK1–3` range. No move has
been seen at **0x200 or above**, the pipeline block (it is simply absent from both
headers, so there is no evidence either way). So the 545 pipeline offsets are a
good working hypothesis; the Tier-C checks below confirm or refute them fast, and
a failure there is the signal that the pipeline block is shifted and needs a sweep.

Tags as in the host-kick spec: **C** confirmed on the 530, **U** 545-derived,
**?** unknown offset.

---

## Tier A — already known (offset in the 530 GPL header). No board check.

These are **K**: the 530 header gives the offset, and the host already writes them
in init/reset today (SRVINIT.md, sgx530-stage1.md "The host writes…"). Use them as
is; only the **field** positions differ, and those differences are already captured
in `ukernel/cores/sgx530.py` (SOFT_RESET TA = b3, CLKGATECTL ISP = b4-5,
`EVENT_STATUS2` = b0/b1, OTPM wait on `EVENT_STATUS` b20).

`CLKGATECTL` 0x000, `CLKGATESTATUS` 0x004 (moved), `SOFT_RESET` 0x080,
`EVENT_HOST_ENABLE2/CLEAR2/STATUS2` 0x110/0x114/0x118, `EVENT_STATUS` 0x12c,
`EVENT_HOST_ENABLE` 0x130, `EVENT_HOST_CLEAR` 0x134, `EVENT_KICKER` 0xac4,
`EVENT_KICK` 0xac8, `USE_CODE_BASE(n)` 0xa0c+4n, `PDS_EXEC_BASE` 0xab8, `PDS`
0xabc, `PDS_INV0/1` 0xad0/0xad4, `PDS_INV2/3` 0xad8/0xadc, `PDS_INV_CSC` 0xae0,
`PDS_PC_BASE` 0xb2c, `BIF_CTRL` 0xc00, `BIF_INT_STAT` 0xc04, `BIF_FAULT` 0xc08,
`BIF_DIR_LIST_BASE0` 0xc84, `BIF_TA_REQ_BASE` 0xc90, `BIF_3D_REQ_BASE` 0xcac,
`BIF_ZLS_REQ_BASE` 0xcb0, `BIF_MEM_REQ_STAT` 0xca8.

Note the cache-invalidate helpers `PDS_INV0` (0xad0) and `PDS_INV_CSC` (0xae0)
that the §6 TA-load sequence pulses are **in this tier** — known offsets.

---

## Tier B — exercised by the 530 init script already. One read-back to confirm.

SRVINIT.md part 2 has the **host** write these at init, and the init writes run on
the 530 (the clock/event machinery comes up). The offsets are therefore already
poked without fault; a single **write-nonzero / read-back** closes the confidence
(a value that reads back is backed; one that reads 0 after a nonzero write may not
exist — fact sheet, "A register that reads 0 after a nonzero write may not exist").

`0x810` `0x814` `0x818`, `0x13c`, `0xa7c` `0xa80`, **`0xa74`** (TA pause/control,
init value 0x208200), `0x4e8`, `0xa78` `0xaac` `0xabc`, **`0x800` `0x804`** (the
TA-load's `0x804` cache bit is here), `0xa00`, `0xb30`.

`0xa74` matters most to host-kick (it is the OOM TA pause); it is already written
at init, so its offset is the most trustworthy of the per-pass set.

---

## Tier C — self-validating against a K status bit. Cheapest real confirmation.

Poke the candidate **request** register and watch a **known** `EVENT_STATUS` /
`EVENT_STATUS2` bit (Tier A). A positive edge confirms the request offset *and* its
meaning, with **no render needed**. Run these after the BIF/PD is set up (a valid
directory list may be needed for the DPM ones to fire).

| poke | watch (K bit) | confirms | tag |
|---|---|---|---|
| `0xa08` = 1 (USE-cache invalidate) | `EVENT_STATUS` b26 MADD_CACHE_INVALCOMPLETE | the §6 cache-invalidate request; also read `0x138 & 0x27` (done mask) and ack `0x140` | ? → C |
| `0x684` = 1 (TA free-list load) | `EVENT_STATUS2` **b0** DPM_TA_FREE_LOAD | the TA DPM-load trigger | ? → C |
| `0x680` = 1 (3D free-list load) | `EVENT_STATUS2` **b1** DPM_3D_FREE_LOAD | the 3D DPM-load trigger | ? → C |
| `0x24c` = 1 (OTPM clear) | `EVENT_STATUS` b20 0x100000 | the OTPM clear (530 waits on b20, not the 545's STATUS2 b12) | ? → C |
| `0x694`/`0x698` = 2 (DPM clear) | `EVENT_STATUS` 0x240 (STATE_CLEAR b6 \| CONTROL_CLEAR b9) | the DPM state/control mode register; modes 1/4 give 0x90 / 0x20\|0x100100 | ? → C |

If any of these fails at the 545 offset while the K watch-bit is otherwise
reachable, the pipeline block is shifted — escalate to a bounded offset sweep
around the 545 value, logged like the stage-1 attempts.

`0x138` (invalidate-done mask) and `0x140` (its ack) ride along with the `0xa08`
check and are confirmed by the same edge.

---

## Tier D — write/read-back existence only (no semantics without a pass).

Write a plausible nonzero value, read it back. A value that sticks means the
offset is backed; it does **not** confirm the register does what §6 says. Use this
to rule offsets in or out before spending a render on them.

- **DPM page counters** `0x700` `0x710` `0x724` `0x72c` `0x720` — also read them
  live during/after a pass to cross-check the §6 counter semantics.
- **Free-list base / threshold writes** `0x618` `0x61c` `0x648` `0x638` `0x620`
  `0x668` `0x624` `0x628` (TA) and `0x600` `0x604` `0x64c` `0x660` (3D).
- **Region-header / tail-pointer bases** `0x62c` `0x634` `0x674`.
- **Page-release arm** `0x63c`.
- `0x630` (DPM slot) is the exception: the 545 note says it may not read back, so
  read-back can't confirm it — it falls to Tier E.

---

## Tier E — only a correct render confirms it. Most expensive; the critical path.

These are confirmed only by a TA/3D pass against a known-good command stream, by
comparing the render output or the completion events. Do them last, as a block,
once Tiers C/D have the supporting machinery standing.

- **The two kicks** — the VDM kick (TA) and the 3D kick registers are the single
  biggest `?`; nothing runs without them, yet they are confirmed only by
  `EVENT_STATUS` b13 TA_FINISHED and b18 PIXELBE_END_RENDER appearing.
  **Isolate them first with a degenerate pass:** program the minimum TA state and
  kick candidate offsets until TA_FINISHED fires, before attempting a real scene.
  This separates "found the kick register" from "programmed the pipeline right."
- **TA layout group** `0x204`–`0x218`, `0x238`, `0x23c` (TE_AA, TE_MTILE*,
  TE_SCREEN, VDM control-stream base; sources in UKERNEL.md §7).
- **ISP/PBE 3D-start group** `0x400` `0x404` `0x408` `0x414` `0x41c` `0x42c`
  `0x480` `0x4bc` `0x4c4` `0x4c8` `0x800`(Tier B offset) `0xa5c` `0xa60` `0xa64`
  (UKERNEL.md §7 gives each one's command/render-details source word).
- **OOM-only** `0x69c` `0x50c` `0x220` `0x224` `0x234` `0x658` `0x734` `0x740` —
  deferred with OOM itself (host-kick §6), so lowest priority of all.

---

## Suggested order of attack

1. **Tier A/B first (an afternoon):** confirm the field diffs and read back the
   init-written set (`0xa74`, `0x800`, `0x804`…). No new machinery.
2. **Tier C (cheap, high value):** the five self-validating pokes. These confirm
   the DPM-load, OTPM and cache-invalidate offsets — the spine of both the TA and
   3D start sequences — with no render. A failure here tells you early if the
   pipeline block is shifted.
3. **Tier D:** read-back the free-list and counter offsets you'll need next.
4. **Tier E:** the degenerate-kick probe to pin the VDM/3D kick registers, then
   the TA layout and ISP/PBE groups via a real frame. OOM last.

Each confirmed offset is logged and marked **C** (the stage-1 method,
microkernel.md), turning the host-kick spec's `?` tags into `C` as you go.

## References
- [sgx530-host-kick.md](sgx530-host-kick.md) — the sequences these registers serve.
- [sgx530.md](sgx530.md) — the fact sheet: the K register map and gap 13.
- `docs/UKERNEL.md` §6 (the register sequences), §7 (each register's source word).
- `docs/SRVINIT.md` "Init scripts" — the part-2 writes (Tier B).
- `ukernel/cores/sgx530.py` — the confirmed 530 field differences.
