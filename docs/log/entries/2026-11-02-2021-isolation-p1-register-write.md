# P1's open half on hardware: a client can't repoint translation by `str` (the store grants no escape)

The P1-P4 run (`docs/log/entries/2026-11-02-0710-isolation-p1p4.md`) left one question open and
flagged it critical (gap G1): a client USSE task can *read* `EUR_CR_*` registers, but does a
*write* to an **effective** register take effect? Joseph decided D9 on it — run the register-write
test first, on the CE5310 board, before committing to shader validation — naming `BIF_DIR_LIST_BASE0`
(0xc84), the page-directory base the BIF requestors translate through: if a client can `str` its
own directory there, the per-VM page directory the whole uAPI leans on is worthless.

Answer, on the CE5310 board (core rev 1.0.14): **the write does not change the register — a client cannot
repoint translation this way.** The store grants no translation escape. Whether a client register
store is *otherwise* harmful (a denial of service) was **undetermined on the first run** (the box
was concurrently being wedged and rebooted by another session's dEQP chunk, so the reboot that
followed the store couldn't be attributed to it) and was then **settled by an isolated re-run: this
store is benign** — it wedges nothing. That holds for the three registers actually tried
(`BIF_DIR_LIST_BASE0` and the two `USE_CODE_BASE_14`/`_15` controls); it is **not** a general claim
that client register stores are harmless — the microkernel itself writes registers with `str`
(UKERNEL.md §8), so stores land in some context, and which registers or task types drop a client
store is unknown. (An earlier draft called the store a DoS and said the box needed a power cycle —
both wrong; corrected below after reconciling timelines with the mesa-gallium session, then
confirmed by the isolated re-run at the end.) Register *reads* stay the firm problem for secrecy.

## How it was tested

Same rig as P1-P4 (`tools/iso/iso.c`, an ordinary services client; the frame path of
`tools/gen/sgxgen.c`). `ldr`/`str` reach the register file by word index (byte offset / 4), so
`str #0x321` addresses 0xc84; the disassembler confirms `str #801` before the run. iso.c reads the
named register from the host (BAR0 of 01:02.0) before and after the frame; `sgx_flush` has a 3 s
timeout, so the host after-read runs even if the GPU wedges, and the CPU/BAR path is independent of
GPU translation. `tools/iso/suite_p1_write.py` is the safe pre-flight; `tools/iso/suite_p1_dirbase.py`
is the one destructive store, run alone under `flock /tmp/gpu.lock`. Raw output:
`docs/log/data/2026-11-02-2021-iso-p1-regwrite.txt`.

## The controls: register stores don't land, reads do

`suite_p1_write` first re-confirmed the rig here (green `0xff00ff00`), client memory (the `+1` store
offset again: `st #0` → R[1]), and the P1 read (`ldr` of `CORE_ID` → 0x01150000). Then the controls,
writing a sentinel to a register nothing in a frame uses and reading it back from both the shader and
the host:

| test | register | wrote | host before/after | shader read-back |
|---|---|---|---|---|
| `p1w_str_base14` | `USE_CODE_BASE_14` 0xa44 | 0x12340 | 0x0726582a / 0x0726582a | **0x0726582a** |
| `p1w_str_base15` | `USE_CODE_BASE_15` 0xa48 | 0x15150 | 0x06600000 / 0x06600000 | **0x06600000** |
| `p1w_bif_read` | `BIF_DIR_LIST_BASE0` 0xc84 | (read only) | 0x01d6b000 / 0x01d6b000 | 0x01d6b000 |

In both write controls the register is unchanged and the shader's own read-back returns the register's
real value, not the sentinel: the `str` is dropped. `BANK0` (0xc78) changed across the frame
(0x606f → 0x707f) with no client write — the GPU's own per-kick bank selection, a G2 datum. The client
shader read `DIR_LIST_BASE0` = 0x01d6b000 directly (P1 read, for the BIF register specifically).

## The store to `BIF_DIR_LIST_BASE0`: ignored as a value

`p1_str_dirbase` read 0xc84, wrote a page-aligned sentinel (0x0a5a5000, inside RAM so a directory walk
faults rather than bus-hangs), read it back, and ended:

```
reg 0xc84 0x1d6b000 0x1d6b000      host before == after: unchanged
render done                        the frame completed
dump R 0 00000000 01d6b000 01d6b000 00000000
   R[1] read before the write, R[2] read back after -> both 0x01d6b000, not the sentinel
```

The register value did not change — by the host read and by the shader's own read-back, in a frame
that completed. A client cannot overwrite the page-directory base and point translation at a directory
of its own; the page-directory boundary (what the new driver gets by owning the PD and not mapping
kernel heaps, the P2/P3 fix) is **not** defeated by a `str` to 0xc84. This is the firm result, and it
comes straight from the frame's own before/after reads, independent of anything else on the box.

## Was the store a denial of service? Undetermined — the run was confounded

Minutes after, the CE5310 board went unreachable and rebooted. The first write-up read that as the store wedging
the box into a reboot loop needing a power cycle. Reconciling with the mesa-gallium session's logs showed
both wrong. (The CE5310 board's clock runs ~5 min 11 s fast, so box-read times — the lock mtime, `uptime`, box
`dmesg` — are ~5 min ahead; the table is real UTC, from mesa's Mac clock.)

| real UTC | event |
|---|---|
| before ~20:04 | read-only probe: box healthy, WPE holds `card0`, **no `/tmp/gpu.lock`, no dEQP** — I was off the box |
| ~20:04:20 | mesa's dEQP chunk (200 tests, the untested RGB write-mask change) starts, takes `/tmp/gpu.lock` and holds it until the box dies |
| ~20:06 | its last log write, mid `fbo.render.stencil.npot_tex2d_rgb_stencil_index8` — no progress after; **box wedged** |
| 20:15:50 | mesa's SSH goes dead (timeout, then refused) |
| ~20:17 | **reboot** (box clock 20:22, uptime 0); mesa's dead runner releases the lock |
| ~20:17 | **I take the lock (box-clock mtime 20:22) and run both suites** — safe clean, then the one store, `render done`, register unchanged |
| ~20:17:35–20:18:06 | ~70 SGX recoveries while my test (and WPE) ran; mesa had nothing running (its chunk died with the box) |
| ~20:19:30 | **reboot again** (box clock 20:24:30); stable after |
| ~20:46 | Joseph power-cycles the CE5310 board (cutting a fresh mesa chunk started ~20:45:25) — 29 min after my store, unrelated to it |

So the **first wedge and the ~20:17 reboot were mesa's dEQP, before my store ever ran** — its flock was
held continuously ~20:04:20 to the box's death, so nothing of mine could overlap. My store ran just after
that reboot, on a box fresh out of mesa's hang via a warm reboot and already unstable — dozens of SGX
recoveries in the window, WPE also rendering. The ~20:19:30 reboot followed my test but **can't be
attributed to the store**: the GPU state right after a warm reboot from a hang is uncertain, and the DDK
lockup dump I first quoted (`SGX Kernel CCB WO:0xB4 RO:0xB3`, in the data file) I read at `uptime` ~170 s
on that boot — i.e. the ~20:19:30 reboot, in a window shared with the box's pre-existing instability, not
a dump of the store's own frame. `/var/log` is tmpfs and every boot wipes it, so no dmesg from the store's
frame survives. The box **self-recovered via that reboot — no power cycle was needed for it** (Joseph's
later ~20:46 cycle was for mesa's next chunk); the earlier "reboot loop needing a power cycle" was a
misread of two reboots.

What the controls show: `str` to the unused `USE_CODE_BASE_14`/`_15` and the plain `ldr` of 0xc84 (in
`suite_p1_write`) returned clean in the **same ~20:17 window, on the same warm-rebooted GPU** as the store
— so they didn't visibly destabilize it, but they ran on the same uncertain-state GPU and don't isolate
the store either. The honest position: the store's **value effect is known (none — no escape); its side
effect (DoS or benign) is unknown** on this run.

## Isolated re-run (Joseph's go, 2026-11-02 ~21:36 real UTC): the store is benign

Joseph approved the isolated re-run to settle the DoS half. open-ukernel's ACTIVE_POWER check had
just cold-booted the box, giving a clean known-good GPU; I took the slot under `/tmp/gpu.lock`, with
WPE stopped and nothing else on the GPU (`tools/iso/suite_p1_dirbase_iso.py`: a sanity frame, then
the one store). Data: `docs/log/data/2026-11-02-2136-iso-p1-dirbase-isolated.txt`.

- Boot was known-good first: uptime ~6 min, **0 SGX recoveries**, lock free, GPU idle.
- `sanity_frame` rendered the green triangle (`0xff00ff00`) — the GPU was clean right before the store.
- The store: `reg 0xc84 0x1d69000 0x1d69000` (unchanged), `render done`, read-back `0x01d69000` not
  the sentinel — the write is ignored, as before.
- After, idle, WPE still off: uptime climbed **411 → 537 s continuously (no reboot)** with **HWR 0**
  the whole ~2.5 min, no lockup/fault in dmesg. Then WPE restored, box left as found.

So on a known-good idle GPU this store **wedges nothing**: no reboot, no recovery. The store to
`BIF_DIR_LIST_BASE0` is **dropped and harmless**, the same as the `USE_CODE_BASE_14`/`_15` stores —
three registers tried, all dropped. This is **not** a general result for client stores, though: the
microkernel is a USSE program that writes registers with `str` (UKERNEL.md §8, e.g. acknowledging
`EVENT_STATUS2`), so stores land in *some* context; nothing here says which registers or task types
drop a client store, and untested ones (`SOFT_RESET`, `EVENT_KICK`, clock/power) could be
destructive. The first run's reboot was the concurrent dEQP wedge, confirmed not this store.

## What this means for the uAPI (D9, G1)

- A client store to `BIF_DIR_LIST_BASE0` **grants no translation escape** — it can't repoint the
  directory. The page-directory design survives this attack: a client can't swap in its own. **(firm)**
- That store, and the `USE_CODE_BASE_14`/`_15` controls, are **dropped and not a DoS** — on a known-
  good idle GPU they change nothing and wedge nothing. Three registers, all dropped. **(firm for
  these three, isolated re-run)**
- **But not general:** the microkernel writes registers with `str` (UKERNEL.md §8), so client-form
  stores land in some context. Which registers or task types drop a client store is **unknown**;
  `SOFT_RESET`, `EVENT_KICK` and the clock/power controls are untested and could be destructive. **(open)**
- A client register **load leaks** — P1 showed reads of `EUR_CR_*` work (`CORE_ID`, `TIMER`, the PD
  physical base at 0xc84), side channels and secrets. **(firm)**

So D9 option 1 (kernel shader validation) forbids client register **loads** (`ldr`) — reads leak,
the firm reason — **and** **stores** (`str`). The store ban isn't because a store is known harmful
(for the three tested it isn't) but because we can't yet show *every* client store is dropped, and
the microkernel proves stores can land; forbidding `str` is the same scan as `ldr`, so it's free.
Relax it only once we know *why* a client store is dropped — e.g. a per-task-type gate. The cost
Joseph weighed in D9 (Mesa generates PDS per draw; the validator needs the full ISA in the kernel)
is unchanged.

## Box

CE5310 board (board — the CE5310 board's address now; the fleet note's board is stale). Ran as a second client
beside the WPE compositor under `flock /tmp/gpu.lock`; mesa-gallium and open-ukernel warned first. The
box's instability in this window was a concurrent dEQP chunk's, not established as the store's; it
self-recovered by 20:24 with no power cycle. Stopped after the one store, not retried then, per the
no-plug rule; the isolated re-run (above) later ran with Joseph's go, on a clean cold boot with WPE
off, and found the store benign — the box was left exactly as found (WPE restored, 0 recoveries).
