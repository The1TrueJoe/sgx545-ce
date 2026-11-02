# P1's open half on hardware: a client can't repoint translation by `str` (the store grants no escape)

The P1-P4 run (`docs/log/entries/2026-11-02-0710-isolation-p1p4.md`) left one question open and
flagged it critical (gap G1): a client USSE task can *read* `EUR_CR_*` registers, but does a
*write* to an **effective** register take effect? Joseph decided D9 on it — run the register-write
test first, on the EA1, before committing to shader validation — naming `BIF_DIR_LIST_BASE0`
(0xc84), the page-directory base the BIF requestors translate through: if a client can `str` its
own directory there, the per-VM page directory the whole uAPI leans on is worthless.

Answer, on the EA1 (core rev 1.0.14): **the write does not change the register — a client cannot
repoint translation this way.** The store grants no translation escape. Whether a client register
store is *otherwise* harmful (a denial of service) came out **undetermined on this run**, and this
entry says why: the box was concurrently being wedged and rebooted by another session's dEQP chunk,
so the reboot that followed the store can't be attributed to it. (An earlier draft of this entry
called the store a DoS and said the box needed a power cycle — both wrong; corrected below after
reconciling timelines with the mesa-gallium session.) Register *reads* stay the firm problem for
secrecy.

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

Minutes after, the EA1 went unreachable and rebooted. The first write-up read that as the store wedging
the box into a reboot loop needing a power cycle. Reconciling the box clock with the mesa-gallium
session's logs showed both of those were wrong:

| UTC | event |
|---|---|
| ~20:08 | read-only probe: box healthy, WPE holds `card0`, **no `/tmp/gpu.lock`, no dEQP** — I was off the box |
| 20:09 | mesa-gallium's dEQP chunk starts, takes `/tmp/gpu.lock` (one flock round the whole runner) |
| ~20:11 | its runner stops making progress (a "ring at 0x37000000 full" retry, then a hang mid `fbo.render.stencil.npot_tex2d_rgb_stencil_index8`) — **the box is wedged** |
| 20:15:50–20:22:04 | box SSH dead |
| ~20:21 | **reboot** (box back 20:22:04, uptime 0); mesa's wedged runner dies, lock released |
| ~20:22 | **I take the lock (mtime 20:22) and run both suites** — safe suite clean, then the one store, `render done`, register unchanged |
| 20:22–20:23 | ~70 SGX recoveries while my test (and WPE) ran; mesa had nothing running |
| ~20:24:30 | **reboot again**; stable since (0 recoveries until mesa's next chunk at 20:45) |

So the **first wedge and the 20:21 reboot were mesa's dEQP, before my store ever ran.** My store ran at
20:22 on a box that had just come up from that wedge, already unstable — dozens of SGX recoveries in the
window, with WPE also rendering. The 20:24 reboot followed my test but **can't be attributed to the
store**: the box was already thrashing, and the DDK lockup dump I first quoted (`SGX Kernel CCB WO:0xB4
RO:0xB3`, in the data file) I read at ts ~170 s on the 20:21 boot — i.e. the 20:24 reboot, in a window
shared with the box's pre-existing instability, not a dump of the store's own frame. `/var/log` is tmpfs
and every boot wipes it, so no dmesg from the store's frame survives. The box **self-recovered by 20:24;
no power cycle was needed** — that earlier claim was a misread of two reboots as a loop.

What the controls *do* still show: `str` to the unused `USE_CODE_BASE_14`/`_15` and the plain `ldr` of
0xc84 all ran clean earlier — but those ran before mesa's chunk destabilised the box, so they don't
isolate the store either. The honest position: the store's **value effect is known (none — no escape);
its side effect (DoS or benign) is unknown** on this run.

## What this means for the uAPI (D9, G1)

- A client register **store grants no translation escape** — it can't repoint `DIR_LIST_BASE0`. The
  page-directory design survives this attack: a client can't swap in its own directory. **(firm)**
- Whether a client register **store is a denial of service** is **undetermined** — the one run was
  confounded by a concurrent dEQP chunk that had independently wedged and rebooted the box. **(open)**
- A client register **load leaks** — P1 showed reads of `EUR_CR_*` work (`CORE_ID`, `TIMER`, the PD
  physical base at 0xc84), side channels and secrets. **(firm)**

So D9 option 1 (kernel shader validation) stands, scoped by what is firm: forbid client register
**loads** (reads leak — the solid reason), and forbid **stores** conservatively (they grant no escape;
their DoS safety is unproven, not disproven). Settling the store half needs an isolated re-run — the
store alone, on a box known good, nothing else on the GPU — which is Joseph's call. The cost Joseph
weighed in D9 (Mesa generates PDS per draw; the validator needs the full ISA in the kernel) is unchanged.

## Box

EA1 (10.0.0.110 — the EA1's address now; the fleet note's 10.0.0.127 is stale). Ran as a second client
beside the WPE compositor under `flock /tmp/gpu.lock`; mesa-gallium and open-ukernel warned first. The
box's instability in this window was a concurrent dEQP chunk's, not established as the store's; it
self-recovered by 20:24 with no power cycle. Stopped after the one store, not retried, per the no-plug
rule; an isolated re-run is Joseph's call.
