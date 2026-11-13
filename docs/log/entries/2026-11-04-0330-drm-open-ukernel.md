# powervr_sgx on the open microkernel: frames render, no DDK code anywhere

M2 and M1 meet. CE5310 board RAM boot of b24d0aa (ce5310-v1), 2026-11-04 ~03:33 UTC. The
microkernel is `ukernel/` at um-re 273225c (9736 bytes, built by `ukernel/build.py`),
wrapped into `powervr_sgx/sgx545_ukernel.bin` by `drm/tools/mkukfw.py` with
its event entry at 0 and one patch site (the TA3D control block at +0x8). Its feature
bits are 0: cleanup is the plain contract, and it is not in secure mode, so every VM
still maps the firmware's heaps. Nothing flashed, no power-cycle.

## What ran

| step | result |
|---|---|
| load | the driver found the open image first, checked its header and CRC, patched the one site, built the event program from its entry table and started it: "microkernel started" |
| GETMISCINFO check | passed: the open microkernel answers with the same version, build and option words the driver checks |
| runtime PM | suspended after load (host control power status 0x8); a NULL job from suspended resumed it, which is a full restart, in 10 ms |
| NULL jobs | 600 in the uAPI suite, all right |
| uAPI suite | 39/39 |
| hang recovery | two job timeouts (below): devcoredump, restart, "microkernel started again" both times, and the suite passed afterwards |
| one frame | not sent: the driver's bug, below |
| one blit | timed out: the microkernel's, below |

## The frame: the driver refused its own TA command

The open microkernel's host-kick table is the identity, so a command's entry is its
type, and the TA command (type 0) has entry 0. `pvrsgx_ccb_send` took entry 0 to mean
"this image has no handler for the type" and refused it with -EINVAL. The job's fence
carried the error, and the frame test, which only waits on the syncobj, printed
"render done" over an empty buffer. The render context's CCB had been written and
never read, and its first-use bit was still set. Fixed in 3298009: only the DDK
image's empty entries are refused. 679d88e logs a job that can't be sent.

## The blit: the transfer ran in the empty directory

The transfer command was never consumed (kernel CCB read offset 1 of 2). The BIF
reported a fault at 0x3e001000, the blit's first general-heap address, with directory
list 0 still on the driver's idle directory. powervr_sgx keeps list 0 on an empty
directory between jobs (the DDK microkernel points it at the job's context directory,
HW context word 2). So the open microkernel's transfer path starts the transfer
without loading the context's directory. Why this never showed under srvinit and the
DDK services is a guess: perhaps list 0 already held a directory that maps the blit there.
This is the
open microkernel's to fix (`docs/UKERNEL.md` §3, TA kick and transfer: "points the
BIF at the context's page directory"). The TA path may well need the same change.

## Second run: frames render

3298009 fixed the driver's half. The microkernel's (um-re e78a17c) sets `BIF_BANK0` for
a transfer, which sends TA/3D requests to directory list 1, the job's directory, as
its render path already did. It doesn't touch list 0. On the RAM boot of 3298009 with
that image (~04:50 UTC):

| run | result |
|---|---|
| one frame, one kick and two kicks | right |
| 10 frames in one process; 10 on two contexts sharing the VM's PB; 3 at 720x480 | all right |
| 5 processes, each frame started from runtime suspend (a full restart) | 5/5 |
| a new context for every frame | 5/5 |
| uAPI suite | 39/39 |
| `cleanup=1`: new contexts, new RT datasets, 5 processes (RC, RT and PB cleanups) | 15/15 frames right; every cleanup completed |
| blit | the pixels are right, but the job never completes |

So the whole render path runs on open code:
- powervr_sgx builds every structure;
- the open microkernel runs TA and 3D;
- the client's programs come from sgxgen;
- one shared context and one PB per VM work;
- the cleanups complete. On the DDK microkernel the RT and PB cleanups never did.

The multi-process alternation the open microkernel shows under srvinit (4 or 5 of
10) does not appear here. The empty idle directory in list 0 was no problem: no
fault, so renders don't need list 0 to map anything (`idle_kernel_pd` not needed).

The blit's completion is the open microkernel's known gap (UKERNEL.md §10): its
transfer-done handler writes the status word but not the destination sync, whose
write-ops-complete powervr_sgx waits for. The job timed out and the restart
recovered, twice.

## Next

- The transfer's destination sync, on the microkernel side; then blits and the
  mixed and hang tests.
- Secure mode: which structures the cores read through a client's directory.
- Cleanup v2, once the microkernel speaks it.

Data: `docs/log/data/2026-11-04-0330-drm-open-ukernel.txt`.
