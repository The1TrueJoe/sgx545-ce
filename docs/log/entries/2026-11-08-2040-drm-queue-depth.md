# Deep queues: no hang at 6, 32 or 64 renders in flight

EA3 (ea3-v2) RAM boot of 3d0604b with `powervr_sgx_frame` from 3190030, 2026-11-08
~20:40 UTC. Nothing flashed, and p1 was clean afterwards.

The question came from Minecraft with Mesa's `SGX545_SLOTS=6`: one job's commands were
all consumed, but it never completed. `-q N` submits a frame's render N times in one
SUBMIT_JOBS, so N jobs wait in the queue and run back to back on one RT dataset: each
`run_job` follows the previous completion at once.

| run | DDK microkernel | open microkernel fd1e151 |
|---|---|---|
| `-n 20` | 20/20 | 20/20 |
| `-q 6 -n 90` | 90/90 | 90/90 |
| `-q 32 -n 90` | 90/90 | 90/90 |
| `-q 64 -n 90 -s 720x480` | 90/90 | 90/90 |
| `-q 32 -n 270 -c 3` | 270/270 | 270/270 |
| `-q 16 -n 600 -c 8 -k 1` | 600/600 | 600/600 |
| CCB commands, resets | 45241, 0 | 45281, 0 |

`-n` stops at 90 because the test's own PDS ring does not wrap.

The kernel runs one job on the GPU at a time (`credit_limit` 1), so queue depth only
changes how soon `run_job` follows a completion. That back-to-back path is clean on both
microkernels for these scenes. The Minecraft hang is in the jobs' contents, not in the
depth itself.

The same run showed a second bug: after the reset, the guilty context's queued jobs
still ran and hung one by one (1 + 5 at a depth of 6). 440895f cancels them; that fix is
not yet tested on hardware.
