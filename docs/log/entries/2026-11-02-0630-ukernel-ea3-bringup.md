# EA3 hardware bring-up: the open microkernel runs, but `mov sa` self-population fails

First M1 hardware run of the open microkernel on the EA3 (cold boot, sgx545ce
driver, `srvinit -o`). The microkernel **loads, is kicked, and executes** — but
`SGXInitialise` times out waiting for init (`PollForValueKM: Expected 0x1 but
found 0x0`), Host control HC-0 (ui32InitStatus) stays 0.

## What ran, pinned with in-microkernel sentinels

`srvinit -o` reached native `SysFinalise` (SGX revision 1.0.14 read), so the
device init and the microkernel upload/kick are fine. To localise the stall I
wrote sentinels to control-block words that appear in the kernel's SGX debug
dump (T3C-C0..E0), at successive points of the event entry's start:

| point in the event entry | marker | result |
|---|---|---|
| entry reached (after the loader-patched `ctlblk` limm) | ctlblk+0xe0 | **written** (0x59FA5) |
| after `mov sa0, r1` | ctlblk+0xd0 | **written** |
| after the first `ld.a.d r2, r1, #1` | ctlblk+0xd4 | **written** |
| after `mov sa2, r2` | ctlblk+0xdc | not written |
| after `mov sa16, r2` | ctlblk+0xd8 | not written |
| after the InitStatus read via `sa1` | ctlblk+0xe4 | not written |
| `init` reached / `init_set` reached | ctlblk+0xe8/+0xec | not written |

So the task runs from the entry through `mov sa0` and a temp (`r2`) load, then
**dies at the second/third secondary-attribute write** (`mov sa1`/`mov sa2`),
before the InitStatus read and the init branch. The loader-patched control-block
base is correct (the first sentinel proves the microkernel reads `ctlblk` and
writes where expected).

## Root cause

The event entry self-populated its secondary attributes (`mov sa0/sa1/sa2/sa16,
r`) and stalled at the write. It is **not** a blanket "a task can't write `sa`"
limit: the DDK microkernel writes `sa` (disassembly shows `mov sa9` and an
indexed `mov sa[idx]`) and runs on hardware. The difference is *when*. The DDK's
event program is a bare `DOUTU` (pds0) that enters the microkernel; the
microkernel **emits** to trigger pds1, whose `DOUTD` DMAs the control block into
`sa` and re-enters it — only in that re-entered context does the DDK write/read
`sa`. openHC skipped the emit/DMA and wrote `sa` immediately in the initial
bare-`DOUTU` context, where it faults. (Corrected 2026-11-02: an earlier note
here mis-stated this as "tasks cannot write `sa`"; the emulator modelled `mov
sa` as a plain register write, so the suite passed offline.)

## Fix

- **B — drop `sa` (chosen, offline-validated).** Carry the control-block base in
  a reserved temp (`r17`) from the loader-patched `ctlblk` limm and derive
  host-control / CCB / kernel-CCB into temps with `ld.a.d r17,#n`. No `mov sa`,
  no load with `sa` as the address base — so it does not depend on the emit/DMA
  `sa` setup at all. `run_ours` (5 frames) and `test_emu` still match the DDK.
  One hardware unknown: whether the bare-`DOUTU` context allocates `r17` — a
  one-word sentinel on the next slot settles it (and if short, bump the DOUTU
  register count in `pds_ours`).
- **A — emit-triggered DMA (alternative).** Replicate the DDK: a pds1 `DOUTD`
  for the `-o` path that fills `sa`, the microkernel emitting to trigger it. The
  DDK-proven mechanism, but needs the emit protocol (open-srvinit's `pds_ours`).

## Box / method notes

- Ran on the normal p1 cold boot (sgx545ce present); no test kernel RAM-booted,
  no reboot/kexec after GPU use — the box was Bench power-cycled between every
  attempt for a clean cold GPU (the open-microkernel init leaves the device half
  initialised). `flock /tmp/gpu.lock`, `sync` before each cut.
- srvinit's `SGX_DEVINITPART2` logs one `invalid param size ... out 16/8` error
  then succeeds on the 16-byte retry (the code already handles it); not the
  blocker.
