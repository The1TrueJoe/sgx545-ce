# SGX530 board: the TA starts and stalls; the vendor TA command values don't change it

**2026-11-14 06:50-07:01 UTC. Our OMAP3630 test board, kernel 7.3.3. Under lock. Clean,
recovered by HWR; modules unloaded.**

## The TA starts

The stock 530 image with edm_init=1, `probe.sh`. `status` was read idle, then 1 s into the
kick, while the job was still pending:

| | idle | mid-hang |
|---|---|---|
| `CLKGATESTATUS` | 0x00110000 | **0x00111000** (TA b12 on) |
| `EVENT_STATUS2` | 0 | 0x10 |
| `BIF_TA_REQ_BASE` | 0 | 0x08400000 |
| `BIF_MEM_REQ_STAT` | 0 | 2 reads outstanding |
| control block 0x28/0x29 | 0 / 0 | 0xd / 1 |

The TA clock gate is on auto (`CLKGATECTL` 0x00212222), so a set TA status bit means the TA
is running. The microkernel programmed the TA side, the TA started, and it sits with two
memory reads that never complete. There is no BIF fault and no event.

## The invalidates raise no named event bit

`inval_probe` (debugfs, off by default) writes each named PDS invalidate register with the
GPU idle: `PDS_INV0`/`1`/`2`/`3` and `PDS_INV_CSC`. Neither `EVENT_STATUS` nor
`EVENT_STATUS2` changed within 1 ms, twice. So the 530's invalidate done bits, if there are
any, aren't in the named event registers. The SGX545's are in 0x138, which the 530's
header doesn't name.

## The vendor's TA command values: null

The vendor stack's 64x64 TA command differs from ours in four per-kick words: 16x16 tiles
(TE screen 0x3003 against our 0x3001), screen size (h-1)<<12 | (w-1), TE AA 0, and VDM
control 0x1a2. With all four, plus the client's relative VDM addresses, 16-pixel region
clip, two-DOUTA terminate and state-emit bit 21, the board gives the same hang as before.
The coredump's `ta_cmd` confirms the values reached the command.
