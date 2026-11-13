# Transfer handler: SGX_SUBMITTRANSFER as a 3D-only render

The kick handler's transfer case (type 1) is no longer a stub. A transfer is a
3D render whose only object is a background that samples the source and writes
the destination (docs/TEX.md §3): `k_transfer` points the BIF at the transfer
context, invalidates the 3D-side caches, programs twenty ISP/PBE registers from
the transfer command's words 0..15, and kicks the 3D core — the same completion
path a render uses.

`run_ours.py` now drives transfers too and diffs them against the DDK:
`[XFER1 vs DDK 'transfer 1'] DDK 21 struct regs, ours 21`, MATCH. The twenty
command-sourced registers and their word indices are in docs/UKERNEL.md §3.

## Unblocking it: an offline transfer trace

The transfer was blocked because no capture snapshots a populated TRANSFERCMD.
It is unblocked now without a capture: `blitest` (tools/tex) builds every
transfer word from `sgxtex` and was proven on the CE5300 board pixel-for-pixel, so a
`blitest` run is a faithful transfer. Built it as a **native arm64 binary**
(like the native `frame-fake`) and gave `libpvr` a snapshot before each
`SGX_SUBMITTRANSFER` and a UBuf trace of the transfer-context registration, so
`fromtrace.py`/`run.py` read it like any frame. Running the DDK microkernel
over that trace is the oracle; the command-word → register map was read
straight off it and confirmed stable across seven transfers of different sizes.

Tool changes (not the microkernel): `libpvr/pvr.c` `trace_snap` takes a reason
(1 kick / 5 xfer) and `pvr_submit_transfer`/`pvr_register_transfer_ctx` emit the
snapshot and UBuf; `tools/gen/fakepvr.c` answers the two transfer ioctls;
`tools/tex/blitest.c` opens a trace and device node from `SGX_TRACE`/`SGX_NODE`.

## Completion: safe now, sync update next

On hardware the 3D-end event fires after a transfer and would otherwise run the
render teardown with stale scratch. `k_transfer` records the transfer in a new
control-block marker (+0x54); `render_done` checks it and routes a transfer to
`xfer_done`, which advances the context's CCB past the 0x1c0-byte command and
raises the host interrupt, leaving the render path untouched. The transfer's
destination sync-object update (from its `SGXMKIF_TRANSFERCMD_SHARED`) is the
documented next increment — until then a transfer's client sees the blit
submitted and the queue advance, but not yet the write-ops-complete signal.

## Checks

- `run_ours.py gen/xfer.trace --ddk FW` — XFER1 21/21 MATCH.
- `run_ours.py gen/{fake,v1,v2}.trace --ddk FW` — TA/3D still MATCH, teardown 12/12.
- `test_emu.py` — adds `transfer completion` (CCB advanced, marker cleared,
  render path skipped); all OK.
- `test_commands.py FW` — all OK.
