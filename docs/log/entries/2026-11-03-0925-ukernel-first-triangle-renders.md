# First TA/3D triangle renders on the open microkernel (byte-identical to the DDK)

The open SGX545 microkernel now completes a full TA/3D render on hardware (CE5300 board p1,
`srvinit -o`), and the output is **byte-for-byte identical to the vendor DDK's**:
same color-buffer md5 `56f8466aa4cf3b08cfbd166b63dd88ec`, 43200 green triangle
pixels (RGB 0,255,0) + 302400 blue-clear (0,0,128) over the visible 720x480. This
is M1's render milestone — the open microkernel drives init, the host kick, both TA
kicks, the TA->3D handover and the 3D to completion, producing the reference frame.

Two HW-only bugs were in the way, both hidden by the offline oracle because it
answered a hardware register as 0 (the same class of miss each time):

## 1. Directory-list clobber (routing)

The HW DDK register baseline showed `EUR_CR_BIF_BANK0` (0xc78) go 0x0f -> 0x1f (TA)
-> 0x101f (3D) during a healthy render, and `CLKGATESTATUS` (0x8) reach 0x000fffa7.
ta3d-state traced the DDK's slot allocator: it reads DIR_LIST_BASE0
(0xc84); when that is non-zero and != the context PD (on HW it holds the kernel PD,
0x01d2x000), it puts the client PD in **slot 1** (DIR_LIST_BASE1 = 0xc38) and routes
the TA/3D requestors to list 1 via BANK0. Our microkernel instead wrote the client
PD into DIR_LIST_BASE0, **clobbering the kernel page directory** the 3D/ISP/PBE still
read through -> the 3D stalled (lockup_3d, BIF_FAULT 0). The emulator read 0xc84 as 0
so run_ours matched. Fix (ukernel.py do_ta): write 0xc38 (not 0xc84), BANK0 = 0x1f
(TA->list 1); do_3d BANK0 = 0x101f (3D->list 1) with the bg-store EDM pulse
0x1011->0x101f; k_transfer 0xc38. Seed `install_hw_model` regs.v[0xc84]=0x01d77000.

HW after the routing fix: BANK0 and DIR_LIST now correct (c84 kernel PD intact,
c38 = ctx PD, c78 0x1f->0x101f), the 3D STARTS (CLKGATESTATUS 0x000fffa7) and reads
memory cleanly (BIF_MEM_REQ_STAT 0, no fault). But it still never retired a tile
(0x728 frozen at 0x19, isp_end_tile no) and `EVENT_STATUS2` raised bit 6 (TRIG_3D),
which the DDK never does.

## 2. 0xa74 init bits (TA/ISP config)

A DDK-vs-OURS poll of the DPM 3D-state registers (DDK renders, ours hangs) showed
the loaded 3D state (0x600/0x604/0x64c/0x660/0x664/0x680) is identical -- the DPM
handover was already correct. The one config register that differed was **0xa74**:
srvinit powers it up with 0x00208200 (b9/b15/b21); the DDK keeps those bits and OR's
the command's TA-flags on top (0x50208200, then 0x57e08200 at the 3D). Our do_ta
CLEARED 0xa74 to 0 first, then wrote only cmd+0x14 (0x50000000) -- dropping the
0x208200 config the 3D needs, which is why the 3D triggered a partial render
(TRIG_3D) and never retired a tile. The emulator never seeded a74 (started 0) so
run_ours matched 0x50000000 on both. Fix (ukernel.py do_ta): read-modify-write 0xa74
(OR the flags into the existing value) instead of clear+overwrite; seed
`install_hw_model` regs.v[0xa74]=0x00208200.

## Evidence

- HW (CE5300 board p1, uk-a74.bin): `srvinit -o` -> "SGX545 microkernel started"; `frame -o`
  -> "render done"; color buffer md5 `56f8466a...` == the DDK's (`srvinit -f`) on
  the same frame, bit for bit. Data: docs/log/data/2026-11-03-0925-ce5300-first-
  triangle.bin (the open uK's color buffer).
- Offline: run_ours fake/v1/v2/xfer/xfer-rect all MATCH the DDK value-for-value with
  0xc84 + 0xa74 seeded; test_emu 5/5; test_commands OK.

## Still open (not blockers for the single triangle)

- lockup_TA variance: ~half the cold boots still re-kick the TA before do_3d (the
  box-timing race ta3d-state's race.py models). The render lands on a do_3d boot;
  a claim-the-CCB-entry-before-the-flag serialization fix is wanted so every boot
  reaches do_3d.
- 0xa74 is OR'd (correct for the first render). The DDK does a masked field-REPLACE
  per field (TA b27-31, 3D b22-26, clear b0-3 at TA finish); convert to masked
  replace for frame 2+ and transfers, alongside the EDM->side-index bracketing of
  client-memory writes.
- A GPU-primer service occasionally wins the cold-boot race (srvinit returns 129);
  hold it off or gate on "microkernel started".
