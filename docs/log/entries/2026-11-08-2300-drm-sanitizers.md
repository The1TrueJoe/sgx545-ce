# The trimmed driver under KASAN, UBSAN and DMA-API debug; a hang with jobs queued behind it

EA1 (ea1-v1) RAM boot of m2-drm a8dff09 built with `drm/ci/debug.fragment`, on
the open microkernel fd1e151 (secure mode), 2026-11-08 ~22:55 UTC. Nothing
flashed; the box went back to its normal boot afterwards.

That build had KASAN (generic, inline, stack), UBSAN (bounds, shift, bool,
enum) and DMA-API debug. Lockdep, kmemleak and debug objects were missing:
they need `CONFIG_DEBUG_KERNEL`, which the board config leaves off. The
fragment now sets it (3ca68ab) for the next run.

| run | result |
|---|---|
| uAPI suite | 39/39 |
| frames: 20; 16 deep x 60; 8 deep x 120 on 3 contexts; 720x480; a new RT dataset or context every frame | all right |
| blits | all right |
| PRIME both ways with simpledrm | right (the control frames, which skip the sync, are stale as designed) |
| hang recovery, devcoredump, runtime PM from suspend | right |
| a faulting render with four renders queued behind it | one timeout, one reset (count 1 -> 2), 2.1 s; the queued jobs cancelled (440895f) |

One finding: DMA-API debug warned that a shmem object's scatterlist had a
128 KiB segment where the device allows 64 KiB (the PCI default). The GPU's
MMU maps page by page, so the driver now sets no segment limit, on the PCI
function and on gma500's auxiliary device (3ca68ab).

No KASAN or UBSAN report.

## Second run: lockdep and kmemleak (3ca68ab)

EA1 RAM boot of m2-drm 3ca68ab with the fragment's `CONFIG_DEBUG_KERNEL`, so
this time lockdep (`PROVE_LOCKING`), kmemleak and debug objects were in, as
well as KASAN, UBSAN and DMA-API debug. Open microkernel fd1e151.

| run | result |
|---|---|
| uAPI suite | 39/39 |
| frames: 20; 16 deep x 60; 8 deep x 120 on 3 contexts; a new RT dataset or context every frame | all right |
| blits, PRIME both ways | all right |
| hang recovery; a faulting render with jobs queued behind it | right; one reset (1 -> 2), 2.2 s |
| kmemleak, two scans after the tests | 0 unreferenced |

No lockdep, KASAN, UBSAN or DMA-API report. Lockdep accepts the completion
path inside `dma_fence_begin_signalling()` and the runtime resume from
`run_job`.

Then a soak: three processes rendering at once (`powervr_sgx_frame -q 4 -n 40
-c 2` in a loop) for five minutes. A few minutes in, the box lost its
userspace: ssh dropped, and since then nothing listens on any port while the
kernel still answers ping. The EA1 has no remote power switch, so it waits for
a hand power cycle. After the cycle it came up on its normal boot with
nothing kept from the lost one: `/var/log` is tmpfs and there is no pstore.
So the cause (the OOM killer under KASAN and kmemleak, or a driver fault) is
still open; the next soak runs with the kernel log sent off the box
(netconsole) and fewer clients first.
