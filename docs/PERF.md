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

Numbers: pending (next EA1 slot).
