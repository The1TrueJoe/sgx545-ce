# EA1 control: DDK µK renders 10/10, ours 4/10 — the EDM faults on a client address

Decisive A/B on one EA1 cold-GPU slot (S50 rig, same frame-dump both runs): the DDK
microkernel (`srvinit -f ddk-fw`) rendered **10/10 with zero HWRs**; ours (uk-9e48757,
the committed frame-1 fix) rendered **4/10 with 3 BIF faults**. Same rig, same frame,
same DDK/sgx545ce kernel driver — so the rig, the box, and the frame are cleared. The
bug is ours.

Data: docs/log/data/2026-11-03-2240-ea1-ddk-vs-ours-baseline.log.

## The fault

All three HWRs: CID 0xF (BIF dir-list 15 = the EDM), BANK0 = 0x101F (3D phase),
BIF_INT_STAT 0x000D0400 (real page fault, MEM_REQ_STAT 0). Fault addresses this run
0xC011F000 / 0xC00E900F / 0xC015500F; a prior run 0x4031F000 / 0x40142000 / NULL. All
in the client GENERAL heap (>=0x3E000000) and all **page-aligned** — the signature of a
general-heap *buffer base* (sgxgen aligns allocations to 0x1000), not a sync word inside
a buffer.

The EDM translates through dir-list 15. The DDK's driver sets list 15 = the **kernel PD**
(our mainline powervr_sgx does the same: pvrsgx_device.c pvrsgx_reset()), which maps the
kernel heap + syncinfo but NOT the client general heap. So any EDM write to a client
general-heap address faults. The DDK µK, under the identical PD, never writes there — it
targets only kernel-mapped (syncinfo) sync words. Ours writes a page-aligned general-heap
base, so one of our completion-write addresses is wrong.

## Localized to the EDM's client-address writes

Ours has exactly these writes to client-provided addresses, all from the EDM:

- render_done (3D side, matches BANK0 0x101F): `*sync0 = 0` with sync0 = `*(rd+0xD0)`;
  `*sync1 = 0` with sync1 = `*(rd+0x240)`; `*complete = pending+1` with
  complete = `*(*(rd+0xE0)+0x14)`.
- ta_finished (TA side, matches the prior NULL fault): count at `cmd+0x5c`, that many
  `{addr,value}` pairs at `cmd+0x118`, `*addr = value`.

One of these offsets picks up a page-aligned general-heap buffer base instead of the
syncinfo sync word the DDK writes. Handed to ta3d-state (who has the DDK command-layout
trace) to say which offset is wrong; the `*(rd+0xD0)` / `*(rd+0x240)` destination-sync
reads are the prime suspects for a page-aligned value.

## Next

Either ta3d-state names the wrong offset from the DDK trace, or a gated RDADDR_LOG logs
the three render_done addresses + the first ta_finished pair to scratch on the next EA1
slot, to see which equals the fault. EA1 restored to normal by the baselines script.
