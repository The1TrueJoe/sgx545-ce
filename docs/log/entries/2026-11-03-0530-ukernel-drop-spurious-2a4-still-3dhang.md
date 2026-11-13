# Dropped the spurious do_3d rd+0x2a4 write (ta3d-state diff) — still lockup_3d

ta3d-state ran the open microkernel's do_3d through the same frame in its DDK-uK
emulator (fw 73dce088, breakpoints on base4.bin) and diffed every 3D-start write
against the DDK. Result: **our 3D-start register programming matches the DDK value
for value** — B (0x604/0x64c/0x660, pb+0x18..0x24), C (the 3 bgblock words at the
same address, BANK0 0->0xf pulse), D3_CONST/D3_RD (0x0cac..0x073c), 0xa74=0x57c00000,
the TPC flush and 0x428 are all identical. So the hang is NOT in the 3D-start writes.

The one "ours only" write it found: **do_3d wrote mem rd+0x2a4 = 1**, which the DDK
never does in that phase (0x2a4 is 0xb8 past the end of the 0x1ec-byte render details,
so it clobbers a word of the following kdata the 3D reads). Dropped it. Kept the
teardown rd+0x2a4 = 2, which run_ours proved the DDK *does* write, but in the `done`
(completion) phase, not at 3D-start. Offline: test_emu/test_commands/run_ours 5/5
MATCH. Build 8184 B / 1023 instr (UK_SENTINEL=1), md5 6c97905a.

## HW: unchanged — byte-identical dump

cold boot (see note below), `srvinit -o uk-no3da4.bin` rc=0 ("SGX545 microkernel
started"), `frame -o bb.bin` -> render TIMED OUT, bb.bin all-zero, 30 HWR.

- T3C-D0 `0x3200AB10 0x00000002 0x00000000 0x0000C01C` — do_ta ran (count 2, 0xC01C)
- T3C-E0 `0x00007AF1 0x00003DED 0x00000000 0x00000000` — ta_finished + do_3d reached,
  render_done marker (0x4D04) absent
- EVENT_STATUS 0 — **pbe_end_render: no, isp_end_tile: no** (the 3D never walked a
  single tile, let alone finished)
- EVENT_STATUS2 0x40 — lockup_3d: yes, lockup_ta: no
- BIF_FAULT 0, BIF_BANK0 0x0f

So the spurious write was a real latent bug but not this hang: the 3D kicks and then
the ISP stalls before the first tile, no BIF fault. With the 3D-start registers
proven correct, the suspect is the **TA->3D memory handoff** — the tiled output the
TA's DPM produced that the ISP walks (region headers at 0x408, tile-pointer lists,
3D DPM page count 0x728), not do_3d's programming.

## Next (ta3d-state leads #2/#3)

1. Transfer test: a 3D-only render (no TA) on the CE5310 board. If it completes, the 3D/PBE/
   end-event path works and the problem is what the TA left in memory.
2. Dump the region headers (0x408 base, expect `00000000 00001000 00000000` 0x80
   apart, tail pointers `8040xxxx`) and 0x728 at the lockup; compare with the DDK.

Data: `data/2026-11-03-0530-ce5310-no2a4-still-3dhang.dmesg.log`. rd+0x2a4 drop held
uncommitted with A+B+C+D; committed render path stands through f852218.

## Note: cold boot got harder

Holding S60sgx + S95ohc-display no longer yields a cold GPU. webd now auto-manages
the panel (OHC_DISPLAY_ENABLE=1 in /opt/ohc/display.conf) and the display service
primes the microkernel via `/opt/sgx-i386/run.sh sgxinit` before launching the
webview. For a cold GPU, also hold `/opt/sgx-i386/run.sh` + `bin/sgxinit` and set
OHC_DISPLAY_ENABLE=0, then restore all of it after. (native srvinit does not use
run.sh, so holding it is safe.)
