# Secure mode: client VMs without the firmware's heaps, every test right

EA3 (ea3-v2) RAM boot of 3d0604b, 2026-11-08 ~18:58 UTC. The open microkernel is um-re
f36ddd2, wrapped with feature bit 1 (secure mode). Nothing flashed, and p1 was clean
afterwards.

f36ddd2 gates the 3D start's background-block copy, which ran the microkernel on the
job's directory, on host control word 15 bit 0. powervr_sgx sets that bit and writes
the three words itself (3d0604b). An image that declares secure mode makes
`share_kernel_heaps` (left at -1) default to off. So no client page directory maps
the firmware's code or data heaps: the driver's "every VM maps the firmware's heaps"
warning no longer appears.

| run | result |
|---|---|
| one frame; 10 on two contexts; 3 at 720x480 | right |
| 5 processes, each frame from runtime suspend | 5/5 |
| a new context for every frame | 5/5 |
| blits | right |
| uAPI suite | 39/39 |
| `powervr_sgx_hang -n 2` (forced hangs, restarts) | all right |

`BIF_FAULT` 0 and `BIF_BANK0` 0x101f throughout.

With the DDK microkernel the sharing stays: it runs the same copy on the job's
directory and has no flag to skip it. P2/P3 (UAPI.md 7.2) are closed for the open
microkernel. A negative test is still to come: a client job that reads a firmware
address must fault.
