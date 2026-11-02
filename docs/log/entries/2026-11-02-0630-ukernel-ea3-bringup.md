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

The event entry self-populates its secondary attributes (`mov sa0/sa1/sa2/sa16,
r`) so later handlers can read the control block through them. A USSE task
cannot write its own secondary attributes on this hardware — they are inputs
provided by the launching PDS program, not task-writable. The offline emulator
models `mov sa, r` as a plain register write, which is why the whole suite
passes offline while init stalls on hardware. (The DDK populates the
microkernel's `sa` from a PDS DMA before the task runs; openHC's "bare DOUTU +
self-populate `sa`" shortcut is invalid on the real core.)

## Fix (next, offline, then re-test here)

Two options; the first is self-contained in the microkernel:
- **A — drop secondary attributes.** Keep the control-block base in a temp from
  the loader-patched `ctlblk` limm (already done) and derive host-control / CCB
  / kernel-CCB into temps with `ld.a.d base,#n` in each handler. No `mov sa`, no
  load with `sa` as the address base. Re-validate `run_ours`/`test_emu`, then
  re-run on the EA3.
- **B — PDS-populate `sa`.** Restore the DDK's emit-triggered PDS DMA (pds1)
  that fills the task's `sa` from the control block before entry, so the
  microkernel only reads `sa` (srvinit's bring-up, `pds_ours`).

## Box / method notes

- Ran on the normal p1 cold boot (sgx545ce present); no test kernel RAM-booted,
  no reboot/kexec after GPU use — the box was Bench power-cycled between every
  attempt for a clean cold GPU (the open-microkernel init leaves the device half
  initialised). `flock /tmp/gpu.lock`, `sync` before each cut.
- srvinit's `SGX_DEVINITPART2` logs one `invalid param size ... out 16/8` error
  then succeeds on the 16-byte retry (the code already handles it); not the
  blocker.
