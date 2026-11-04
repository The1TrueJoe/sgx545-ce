# powervr_sgx on the open microkernel: first boot, no DDK code anywhere

M2 and M1 meet. EA1 RAM boot of b24d0aa (ea1-v1), 2026-11-04 ~03:33 UTC. The
microkernel is `ukernel/` at um-re 273225c (9736 bytes, built by `ukernel/build.py`),
wrapped into `powervr_sgx/sgx545-openhc-ukernel.bin` by `drm/tools/mkukfw.py` with
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

## Next

- The same steps on 3298009: one frame, then the per-VM PB and its type-5 cleanup
  with `cleanup=1`.
- The transfer's directory load, on the microkernel side.

Data: `docs/log/data/2026-11-04-0330-drm-open-ukernel.txt`.
