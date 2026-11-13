# After a reset only the PB needs setting up again; devcoredump; IGT reserved bits

M2, following 0550. CE5310 board RAM boots of f52a569, 2026-11-03 ~06:00 UTC, DDK microkernel,
nothing flashed, no power-cycle. One deliberate device loss, recovered by a plain reboot
into the same RAM kernel.

## Which post-reset set-up matters

0550 set up four things again after a reset. The debug mask `powervr_sgx.reset_rearm`
switched each of them, and one `powervr_sgx_hang` round ran with each left out in turn:

| left out | result |
|---|---|
| HW transfer context bit 0 | all right |
| HW render context +0x20/+0x24 (render details in flight) | all right |
| RT dataset as new | all right |
| **PB as new** | the render after the reset hangs (A, and a new context C); the second restart fails (init status 0) and the device is lost, as in 0530 |
| HW render context bit 0 | all right |
| everything except the PB, 5 rounds | 5/5 right; then 10/10 frames on 2 contexts, blits right, uAPI 33/33 |

So the 0550 explanation was half right. Bit 0 of the HW render context's word 0 is
cleared by the microkernel, but a fresh microkernel doesn't need it set again. What
breaks is the PB: the hung TA took pages off the VM's free list, and nothing gives them
back. The driver now resets only the PB after a reset, and the mask is gone.

## devcoredump

On a timeout, before the restart, the driver writes `/sys/class/devcoredump/devcd<N>/data`
(`select WANT_DEV_COREDUMP`; writing to the file frees it). It contains:
- a header (job type, kernel CCB sequence, reset number);
- the `status` text and registers;
- the host control block, TA3D control block, kernel CCB control and the whole
  kernel CCB;
- the hung job's command and what it names. For a render: HW context, PB descriptor,
  free list, TA command, render details, RT data, sync words, background block, region
  headers, tail pointers and the TA's blocks. For a transfer: HW transfer context and
  the 0x1c0-byte command.

A forced TA page fault gives about 27 KB, with `BIF_FAULT = 0x3e800032`, the unmapped
vertex address `SGXGEN_FAULT` uses. `powervr_sgx_hang` now checks that every forced
hang leaves a dump; all 11 in this slot did. The header named the render job "null" in this run;
the type labels were in the wrong order, and that is fixed.

## IGT: reserved bits, VAs outside the VM

The uAPI suite now has 33 tests, 33/33 on the CE5310 board:

| test | checks |
|---|---|
| `reserved-bits` | every reserved bit of every flags word, and every padding word, one at a time, on an otherwise valid call, all -EINVAL: `CREATE_BO` (flags bits 1-63, padding), `GET_BO_MMAP_OFFSET`, `CREATE`/`DESTROY_VM_CONTEXT`, `VM_MAP` (flags, padding), `VM_UNMAP`, `CREATE_CONTEXT` flags, `DESTROY_CONTEXT`, `GET_CONTEXT_STATE`, `CREATE_RT_DATASET` flags, `DESTROY_RT_DATASET`, `SUBMIT_JOBS` padding, job padding, NULL/render/transfer job flags, TA kick flags, sync op flags bits 4-30 and handle types 2-15, BO ref flags bits 1-31; afterwards nothing was queued and the objects still work |
| `submit-other-vm` | render and transfer streams whose addresses are mapped only in another VM of the same file, or were mapped and then unmapped, or lie in the kernel's 3D-params range: all -EINVAL |

## Runtime PM

The SGX PCI function (`8086:089b` at 01:02.0) has no capability list (status word 0), so
no PCI PM capability: the PCI core never moves it out of D0. Runtime PM for this driver
is the microkernel power-off and restart alone, with the PB reset above covering what
lives through it.

Data:
- `docs/log/data/2026-11-03-0600-drm-rearm-bisect.txt`;
- `docs/log/data/2026-11-03-0600-drm-devcoredump.txt`.
