# SGX530 board: the vendor stack renders the 64x64 scene, and its stream doesn't unblock ours

**2026-11-14 05:15-06:30 UTC. Our OMAP3630 test board. Under lock. Clean: back on its normal
SD boot.**

## The vendor stack renders it

The board was cold-booted once into a capture kernel: the GPL openpvrsgx 1.14.3699939 module
plus a kick hook, with the version-matched vendor user space. A 64x64 pbuffer was cleared to
(0, 0, 0.5, 1) and one red triangle drawn over the lower-left half. The readback is right.
The hook copied the kick's command and the GPU-visible pages, and they were read with our
own layouts. The facts are in `docs/porting/cores/sgx530-state.md` (UPDATE 2026-11-14) on
`sgx530`. In short:

- VDM PDS addresses are offsets from the PDS exec base on the 530. They are absolute on the 545.
- The vertex shader launches have word 0 bit 21 clear and word 2 = 0, so the vertex
  write-retire hypothesis is refuted.
- The state-emit launches have bit 21 set. The pixel launches have word 2 = 0x20.
- The TA terminate is two DOUTAs: `(0x2000, 0)` and `(0x00030003, 0x100)`, the last tile.

## Our stack with those changes: null

`sweep.sh`, the stock 530 image with edm_init=1 and SGXGEN_PDS_8WORD=1. Four variants, one
module load each:

| variant | result |
|---|---|
| control | render timeout, region headers 0 |
| `SGXGEN_VDM_REL=1` | same |
| + `SGXGEN_TERM530=0x00030003` | same |
| + `SGXGEN_STATE_BIT21=1` | same |

Every run: BIF_FAULT 0, EVENT_STATUS 0, EVENT_STATUS2 0x10, CLKGATESTATUS 0x00111000. The
control block has word 0x28 = 0xd and word 0x29 = 2: the microkernel's bounded wait 0xd gave
up once per TA kick. Wait 0xd is `invalidate()`'s TA-side done wait (`0x138` bits `0x27`,
the 545's), before the TA start. Next: the microkernel's 530 TA kick (do the done bits
differ, and does the TA start at all), with the TA/VDM status registers added to the coredump.
