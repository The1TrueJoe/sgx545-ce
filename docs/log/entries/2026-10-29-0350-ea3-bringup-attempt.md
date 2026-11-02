# First EA3 slot: the loader works, the core comes up, init does not complete

First hardware test of openHC's open microkernel, on the shared EA3 (slot
agreed with the DSP, both b53 and the SGX replayer sessions; Bench recovery
pre-cleared). `srvinit -o /lib/.../ukernel.bin` on a clean boot, GPU not yet
initialised.

## What worked

- `srvinit -o` loaded our microkernel and built every buffer. The kernel's
  TA/3D control dump shows the control block exactly as srvinit wrote it:
  self `0x32003000`, host control `0x32003100`, CCB control `0x32002000`,
  kernel CCB `0x32000000`, HWPerf/TA-sig/3D-sig `0x32004000 / 0x3200a420 /
  0x3200b000`.
- The DEVINITPART2 8-byte OUT was refused by this driver's old table and
  srvinit's 16-byte retry went through, as designed.
- `SysFinalise: SGX revision = 1.0.14` — the kernel reset the core and ran
  both init scripts; srvinit's setup is right.

## What did not

`SGXInitialise` polled `ui32InitStatus` and timed out at 0 — our microkernel
never set `PVRSRV_USSE_EDM_INIT_COMPLETE`. The host control block dumped all
zero. No lockup, no hang, no recovery, no `scheduling while atomic`: a clean
failure. The box rebooted cleanly in ~20 s and was handed back GPU-idle.

## Most likely cause (to pin down offline)

Our microkernel's init entry writes `ui32InitStatus` to the host control
address it reads from `sa1` (TA3D control word 1), which the event program is
supposed to have DMAed into the secondary attributes. Our event program
(`pds_ours`) does that DMA (DOUTD) itself, then DOUTU-starts the entry — but
the DDK's event program is a bare DOUTU, and the DMA is a *separate* PDS
program the microkernel triggers by emitting an event. If the event-kick
context does not run the DOUTD the way a microkernel-emitted kick does (the
`0xa6c`/`0xa70` event-program control words may gate it), `sa` is not
populated and the init write lands at a stale address — exactly the observed
"control block correct, InitStatus still 0".

Next, offline: model or trace the event-program/EDM path to confirm how the
microkernel's secondary attributes get populated on an event kick, and either
fix `pds_ours` or pass the host-control address by DOUTA (PA register, the
proven pds3/pds4 pattern) instead of a DMA. Then take another slot. No
hardware change was left on the box.
