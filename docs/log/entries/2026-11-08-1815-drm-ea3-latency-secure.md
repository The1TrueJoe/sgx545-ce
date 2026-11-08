# The EA3: latency baselines, the transfer still incomplete, secure mode blocked by BANK0

EA3 (ea3-v2) RAM boot of 46c8212, 2026-11-08 ~18:15 UTC. The run went first to the
DDK microkernel, then to the open one (um-re 0e7eb7a). Nothing flashed, and p1 was
clean afterwards. The RAM-boot hook now carries the bootstrap's `ohc.*` options
(a7d3df1), so the EA3 kept its switch and network.

## Latency

`docs/PERF.md` has the table. A trivial render goes from queue to finished in 344 us
on the DDK microkernel and 315 us on the open one. Under 100 us of that is the
kernel's: scheduler, kick and completion. The DDK microkernel raises three
interrupts a render, the open one a single interrupt.

## The blit on 0e7eb7a

The pixels are right and the kernel CCB is consumed. But the transfer context's CCB
read offset never moves, and the destination sync's write-complete is never
written. The command names it correctly: 1 dst sync, write pending 0, at 0x2d000004.
The BIF also logged a fault at 0x0000000f. So the open microkernel's transfer-done
path doesn't reach the sync, possibly through a null pointer. That's for the
microkernel.

## Secure mode

With `share_kernel_heaps=0` (no firmware heaps in client directories), every render
timed out. The fault is at 0x300020d1, inside the microkernel's own code. `BIF_BANK0`
read 0x1011, which sends the EDM (the microkernel itself) to directory list 1, the
job's directory, instead of list 15. While the heaps are shared, the job's
directory maps the microkernel too, so this never showed. One of the microkernel's
handlers sets the EDM's bank index to 1. With that fixed, secure mode should need
nothing else from the cores, by open-ukernel's account of the client heaps they read.

Data: `docs/log/data/2026-11-08-1815-drm-ea3-latency-secure.txt`.
