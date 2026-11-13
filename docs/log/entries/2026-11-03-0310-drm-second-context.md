# powervr_sgx: a second render context hangs on its third frame; the in-place restart fails

Two RAM-boot slots on the CE5310 board after the first frames (`2026-11-03-0210-drm-first-frame`),
2026-11-03 ~02:45 (driver a7503fe) and ~03:10 UTC (f61191a), DDK microkernel with the
kernel heaps shared, nothing flashed, no power-cycle; the box went back to its normal boot
after each.

## The frame-2 hang depends on which process it is, not on the size

The first-frame entry reported 720x480 hanging on frame 2 "on a clean boot". Every one of
those 720x480 runs was the **second frame process** on its driver load. Run as the first
process, 720x480 renders all ten frames:

| driver | process 1 | process 2 | process 3 |
|---|---|---|---|
| 4401757 (before the 0x10 change) | 256x256 10/10 | 720x480 2/10 | |
| a7503fe | 720x480 10/10 | | |
| f61191a (released objects kept) | 720x480 10/10 | 720x480 2/10 | 720x480 0/10 |

The second process renders frames 0 and 1 and times out on frame 2 (`ccb 27, consumed 27`:
the microkernel took the TA command, the render never finished). A third process gets
nothing, because by then the restart has failed (below).

**The TA-flag 0x10 change in a7503fe was not shown to be the fix.** No first-process
720x480 run was made before it, and the hang it was aimed at turned out to be the
second-context hang, which a7503fe still has.

## Keeping the first context's objects does not fix it

f61191a keeps every released kernel object (HW render context, CCB, PB, free list, RT data)
mapped until the next firmware restart instead of freeing it when the context is destroyed.
The second process still hangs on its frame 2, so a freed and reused structure is not the
cause. The `reap_after_render` A/B was dropped as moot.

Candidates still open:

- microkernel state tied to the previous render context that the DDK clears with its
  cleanup commands (render context, RT dataset, PB) before the host frees them (G15); the
  driver sends none;
- the per-context sync counters (`sync3d`, the device sync list) starting from values the
  microkernel does not expect for a new context;
- BIF directory-cache invalidation when the context's directory changes.

## The in-place restart still fails

f61191a also restarts from a fresh copy of the microkernel image with both arenas cleared,
as a module load does. After the timeout:

```
job timed out (ccb 27, consumed 27); resetting
microkernel did not start (init status 0x0, event status 0x20000000)
microkernel did not restart: device lost
firmware running: 0 lost: 1 resets: 1 irqs: 36
```

A module reload still brings the GPU back, and unloading leaves `SOFT_RESET` at 0x3f. What
differs from a load is now the MMU (the firmware, idle and reset directories are reused,
not reallocated), the interrupt and scheduler state, and the pages themselves.

Data: `docs/log/data/2026-11-03-0245-drm-720-frames.txt` (runs by process order, before and
after the 0x10 change), `2026-11-03-0310-drm-second-context.txt` (f61191a: dmesg, debugfs
`status`).
