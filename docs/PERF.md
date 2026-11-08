# Performance

## Kernel driver (powervr_sgx)

What a job costs on the CPU side, from a submit to its fence, measured with the
driver's tracepoints (`powervr_sgx:pvrsgx_kick`, `pvrsgx_irq`, `pvrsgx_job_done`,
`pvrsgx_flush_imports`) together with drm_sched's (`drm_sched_job_queue`,
`drm_sched_job_run`, `drm_sched_job_done`).

    drm/tools/pvrsgx-trace.sh OUT COMMAND...    # on the box: tracefs around a command
    drm/tools/pvrsgx-latency.py OUT             # per stage, per job type

The stages:

| stage | what it is |
|---|---|
| queue → run | drm_sched: the job's dependencies, its entity's turn, the scheduler's worker waking |
| run → kick | the driver: the microkernel command built, imports flushed, the kernel CCB written and the doorbell rung |
| kick → irq | the GPU and the microkernel, to the interrupt that reports the job |
| irq → done | the interrupt thread finding the job done, imports flushed again |
| done → finished | the hardware fence's callback signalling drm_sched's finished fence, which userspace waits on |

### Baselines (EA3, 2026-11-08)

RAM boot of 46c8212 (ea3-v2). Each job type was traced back to back, so the device
stays powered: no runtime-PM resume is in these numbers. A render is the frame
test's 256x256 clear and triangle, and a blit is the blit test's 64x64 copy. No
imports are mapped, so no flushes are counted. Medians in microseconds, p90 in
brackets.

| stage | render, DDK microkernel | render, open microkernel | blit, DDK | NULL, DDK | NULL, open |
|---|---|---|---|---|---|
| queue → run | 40 (84) | 43 (51) | 56 (79) | 71 (79) | 65 (81) |
| run → kick | 16 (23) | 19 (23) | 10 (20) | 18 (43) | 18 (18) |
| kick → last irq | 268 (271) | 214 (227) | 52 (59) | no interrupt | 21 (37) |
| last irq → done | 16 (31) | 30 (52) | 22 (33) | – | 21 (26) |
| done → finished | 2 (3) | 3 (4) | 3 (3) | 5 (5) | 32 (36) |
| **queue → finished** | **344** (412) | **315** (355) | **142** (179) | **132** (168) | **155** (177) |
| interrupts per job | 3 | 1 | 1 | 0 | 1 |

What the kernel adds: the scheduler 40-70 us, the kick 10-20 us, and completion
20-30 us after the job's last interrupt. That is under 100 us a job, against
200-270 us of GPU and microkernel time for even a trivial render. The DDK
microkernel raises three interrupts a render (TA done, 3D done, render done), each
of them a wake of the interrupt thread. The open one raises one. A NULL job on the
DDK microkernel completes when the CCB is consumed, so no interrupt, and the driver
finds it by polling.

Not yet measured:
- the import flush, which needs a run with simpledrm buffers (PRIME);
- pvrsrvkm on the same jobs. The DDK services path has no equivalent tracepoints,
  and the comparison there is end to end (mesa-gallium's per-frame numbers).

Trims to consider, with these numbers:
- wake the interrupt thread only when the oldest active job is done (it is two of
  the DDK microkernel's three interrupts);
- `queue → run`: drm_sched's worker wakeup is the largest kernel share. It is
  roughly constant, and only worth attacking if submits ever queue back to back.
