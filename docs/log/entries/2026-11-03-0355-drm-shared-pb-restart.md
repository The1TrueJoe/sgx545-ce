# powervr_sgx: one PB per VM serves several render contexts; in-place restart survives 14 forced hangs

Two items from the M2 list, on one EA1 RAM boot (c9a1af8, 2026-11-03 ~03:52 UTC, DDK
microkernel, nothing flashed, no power-cycle).

## One parameter buffer per VM

The DPM numbers PB pages from the 3D-params heap base, so a VM's PB has to start there
(`2026-11-03-0340-drm-per-vm-pb`). A second render context in the same VM therefore
can't have a PB of its own. The VM now owns one. Its first render context creates it
(that size wins, as in the DDK's `SGXFindSharedPBDescKM`), the VM's other render contexts
share it, and the VM keeps it until the client closes the VM.

`powervr_sgx_frame` gained `-c N` (frames round robin on N render contexts in one VM) and
`-C` (the context replaced after every frame, same VM). All at 720x480:

```
-c 2 -n 20       20/20 frames right
-c 3 -n 30       30/30 frames right
-C -n 10         10/10 frames right
-n 10            10/10 frames right
-c 2 -n 20 -r    20/20 frames right   (new RT dataset every frame)
27/27 passed     (uAPI suite)
firmware running: 1 lost: 0 resets: 0 irqs: 270
```

**One-kick scenes work too.** `-k 1` (TA flags 0x73, one kick that also renders) gave
5/5 rounds right. The earlier one-kick failures came after the misplaced PB had hung the
GPU, so they were contamination, as suspected.

## In-place restart

A deliberate hang: `SGXGEN_FAULT=1` makes `tools/gen` point the triangle's vertex DMA at
an unmapped general-heap address. The TA page-faults and the job times out after 2 s.
The rounds:

- six of {one faulting frame, then 5 good frames};
- four of {two faulting frames back to back, then 6 good frames on 2 contexts};
- then the uAPI suite and 9 frames on 3 contexts.

```
job timed out (ccb 851, consumed 851); resetting
microkernel started again
... (14 times)
27/27 passed
9/9 frames right
firmware running: 1 lost: 0 resets: 14 irqs: 553
restarts-ok=8 timeouts=8 failed=0   (second batch; the first had 6 of 6)
```

**14 of 14 restarts came back, and every frame after them was right.** The driver
quiesces before restarting (deinit script, cores held in reset, bus mastering
dropped, `restart_quiesce`). The earlier failures followed hangs caused by the misplaced
PB. Whether the quiesce or the PB fix is what made restarts reliable was not separated.

## Next

- Transfer jobs.
- IGT negative cases toward the M2 exit criterion, including this forced-hang recovery
  as a test of its own.
- "One client can't corrupt another" needs the open microkernel (D8: under the DDK
  microkernel every VM maps the kernel heaps), so it is blocked on M1 and isn't run in
  compat mode.

Data: `docs/log/data/2026-11-03-0355-drm-shared-pb-restart.txt`.
