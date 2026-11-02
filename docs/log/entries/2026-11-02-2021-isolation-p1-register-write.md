# P1's open half on hardware: a client can't repoint translation by `str`, but the store wedges the box

The P1-P4 run (`docs/log/entries/2026-11-02-0710-isolation-p1p4.md`) left one question open and
flagged it critical (gap G1): a client USSE task can *read* `EUR_CR_*` registers, but does a
*write* to an **effective** register take effect? Joseph decided D9 on it — run the register-write
test first, on the EA1, before committing to shader validation — naming `BIF_DIR_LIST_BASE0`
(0xc84), the page-directory base the BIF requestors translate through: if a client can `str` its
own directory there, the per-VM page directory the whole uAPI leans on is worthless.

Answer, on the EA1 (core rev 1.0.14): **the write does not change the register — a client cannot
repoint translation this way — but the store is not harmless: it locked up the GPU and the box
rebooted into a lockup loop.** So register *stores* from client code are a denial of service, not a
translation escape. Reads stay the real problem for secrecy.

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

## The store to `BIF_DIR_LIST_BASE0`: ignored as a value, fatal as an act

`p1_str_dirbase` read 0xc84, wrote a page-aligned sentinel (0x0a5a5000, inside RAM so a directory walk
faults rather than bus-hangs), read it back, and ended:

```
reg 0xc84 0x1d6b000 0x1d6b000      host before == after: unchanged
render done                        the frame completed
dump R 0 00000000 01d6b000 01d6b000 00000000
   R[1] read before the write, R[2] read back after -> both 0x01d6b000, not the sentinel
```

The register value did not change — by the host read and by the shader's own read-back. A client
cannot overwrite the page-directory base and point translation at a directory of its own; the
page-directory boundary (what the new driver gets by owning the PD and not mapping kernel heaps, the
P2/P3 fix) is **not** defeated by a `str` to 0xc84.

But seconds after the iso process exited the EA1 went unreachable and rebooted, then entered a
lockup → watchdog → reboot loop (uptime seen resetting 170 s → 50 s). The DDK's lockup dump is in the
data file: Host-control and TA/3D-control blocks printed, and `SGX Kernel CCB WO:0xB4 RO:0xB3` — a
command queued that the GPU never consumed, the signature of a hung core. A warm reboot doesn't clear
it; the SGX silicon state is stuck, and the EA1 has no smart plug, so it needs a power cycle. The
controls — the two code-base stores and the plain `ldr` of 0xc84 — all ran clean, so the store to the
BIF register is what destabilised it: the write perturbs BIF state (a pending/shadow latch applied on
the next flush) enough to hang the core, without the committed register value ever reading back changed.

## What this means for the uAPI (D9, G1)

The write-first test Joseph asked for is answered, and it narrows the threat without lifting it:

- A client register **store does not grant a translation escape** — it can't repoint `DIR_LIST_BASE0`.
  So the page-directory design survives this specific attack: a client can't swap in its own directory.
- A client register **store is a denial of service** — a `str` to a BIF register hangs the GPU and
  takes the box down (here, into a reboot loop needing a power cycle).
- A client register **load leaks** — P1 already showed reads of `EUR_CR_*` work (`CORE_ID`, `TIMER`,
  and the PD physical base at 0xc84), side channels and secrets.

So D9 option 1 (kernel shader validation) still stands, and this scopes it to **full `ldr`/`str`
coverage**: client USSE (and the PDS it drives) must be forbidden any register-file access — `ldr`
because reads leak, `str` because stores wedge the machine — not because a store would otherwise
breach the page directory, which it does not. The cost Joseph weighed in D9 (Mesa generates PDS per
draw; the validator needs the full ISA in the kernel) is unchanged; the design reason is now precise.

## Box

EA1 (10.0.0.110 — the EA1's address now; the fleet note's 10.0.0.127 is stale). Ran as a second client
beside the WPE compositor under `flock /tmp/gpu.lock`; mesa-gallium and open-ukernel warned first.
The destructive store left the GPU hung across warm reboots; reported to the coordinating session to
have Joseph power-cycle it. Stopped at the first wedge, not retried, per the no-plug rule.
