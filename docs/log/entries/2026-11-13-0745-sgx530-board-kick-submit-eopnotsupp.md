# SGX530 board kick: microkernel starts, SUBMIT_JOBS blocked on unimplemented cmd defs

**2026-11-13 ~07:45 UTC. Our OMAP3630 test board, kernel 7.3.3, RAM/SD boot. Under lock.**

First end-to-end kick attempt on the 530 through the DRM uAPI: `powervr_sgx_frame`
(tools/gen sgxgen built `-DSGXGEN_DRM`, static armhf, md5 1c5b7a05), the m2-drm driver
with `edm_init=1`, and a microkernel image. 64x64 single render.

## Result: driver + microkernel good; the client submit is rejected in validation

Clean, non-wedge. The whole bring-up path works up to the job submit:

```
powervr_sgx 50000000.gpu: SGX530 has one memory context: clients are not isolated (allow_unisolated=1)
powervr_sgx 50000000.gpu: SGX core id 0x01120000 revision 0x00010205: TI OMAP3630 (SGX530 1.2.5)
powervr_sgx 50000000.gpu: powervr_sgx/sgx530_ukernel.bin: open microkernel dev, interface 1.0, features 0x8
powervr_sgx 50000000.gpu: EDM init: event status 0x00000000, BIF int 0x0, timer 0x00000000
powervr_sgx 50000000.gpu: microkernel started
powervr_sgx 50000000.gpu: parameter buffer: 0x1022 pages
```

- `edm_init` launch (the §9 path) starts the microkernel cleanly on 7.3.3 — **no BIF fault,
  event status 0**. The microkernel start is solid on the current board/kernel.
- Then: `sgxgen: SUBMIT_JOBS failed: -95 (job 0)` → `0/1 frames right` (centre/corner 0).
  **-95 = EOPNOTSUPP.** No render ran; the GPU and the client PDS were never exercised.

## Root cause: the sgx530 command-validation defs are unimplemented stubs

`drm/powervr_sgx/pvrsgx_cmd_defs.c`: `sgx530_render` / `sgx530_ta_kick` / `sgx530_transfer`
carry only `.name`/`.type` — `.supported` is unset (false) — with the comment
"sgx530: ta_kick depends on a value not yet known there". `pvrsgx_cmd_copy()` then hits
`if (!def->supported) return -EOPNOTSUPP;`, so the driver rejects every 530 TA kick / render
in command validation, before the GPU or any PDS is touched.

This settles the earlier open question: the frame's 545-shaped PDS/sgxgen was **not** the
blocker (it never reached the hardware), and the microkernel path is fine. The "value not yet
known there" is exactly what the ukernel annotation (2026-11-13) has now established from
STATE.md §1/§6.

## Fix (scoped, driver-side, 530-only)

Populate the three sgx530 defs with `.supported = true`, `.size`, `.all_size`, and the field
tables. The `sgx545_*_fields` tables validate **core-independent** base uAPI fields
(`base.depth_addr`, `base.pixel_event_pds_addr`, `base.vdm_base`, …), so the 530 reuses them;
the only 545-specific parts are the `zls_control`/`flags_3d` *extension* fields, which sgxgen
does not emit, so the 530 defs carry no extensions. 545 defs untouched → golden byte-identical.

One 530 value to confirm (clean-room, security-relevant for the PDS extent range-check):
`pvrsgx_cmd_core_sgx530.pds_row_bytes` is 0; the 545 is 16 (STATE.md:1217 "PDS data rows,
16 B each"). The 530 lacks the 2-dword PDS interleave, so the row size needs confirming for
the 530 before it goes in `size = rows * pds_row_bytes`. Requested from the ISA side.

## Board

Clean throughout; driver unloaded, lock released, left on its normal boot (7.3.3). No power
cycle needed. Host key matched the owner's on-board reading (same boot). dropbear needs
`scp -O`; the board has no `timeout`.
