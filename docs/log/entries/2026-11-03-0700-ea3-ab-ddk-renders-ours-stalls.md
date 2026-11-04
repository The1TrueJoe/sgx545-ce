# A/B on the EA3: the DDK microkernel renders, ours stalls — the gap is 100% ours

With GPU work moved to the EA3 (10.0.0.113, p1-direct per M2: no GPU problem with the
Oct-26 kernel, pvrsrvkm bound, headless so the GPU stays cold on boot), I ran the
decisive A/B ta3d-state proposed: the SAME tools/gen frame through MY srvinit + MY
frame tool, DDK microkernel vs ours, from clean cold boots.

- **Arm 1, DDK microkernel** (`srvinit -f ddk-fw` + `frame -o bb.bin`): rc=0,
  **"render done"**, 0 lockups. bb.bin = 302400 blue-clear pixels (0x800000ff, the
  frame's {0,0,0.5,1}) + **43200 green-triangle pixels (0x00ff00ff, {0,1,0,1})**.
  A correct render.
- **Arm 2, ours** (`srvinit -o uk-rdcopy.bin` + same frame): TIMED OUT, lockup_TA
  (not lockup_3d), do_3d marker absent, rgn1 word 0 = 0 (kick-2 terminate TA never
  wrote the tile coords). The copy block applied (rd+0xd4=0x400 in the dump).

**Conclusion: the gap is 100% our microkernel firmware.** The p1 stack, MY srvinit
init, and the client frame/PDS/texture data are all proven correct — the DDK µK
renders the triangle through exactly them. srvinit-init is NOT the problem (an
earlier open question). This is a clean, reproducible A/B rig: any ours fix can be
tested against the DDK control on the same box/frame.

## Cross-box: ours fails at a DIFFERENT phase per box

- EA1 (fat kernel): ours reached do_3d, lockup_3d.
- EA3 p1 (Oct-26): ours lockup_TA, kick-2 terminate TA never finishes, do_3d not
  reached — the pre-read-offset symptom, even though uk-rdcopy has the read-offset +
  0x274 fixes.

So ours has a timing/ordering FRAGILITY: it gets further on the EA1 than on the EA3.
The lockup_TA (kick-2 terminate) on the EA3 is the earlier, cleaner symptom to chase.
Suspect = the kick-2 / ta_finished / terminate path (the kdiff TA-finish bookkeeping
ours still omits: pb+0x14, rtentry+0x34, rtdata+0x4, cmd+0x58, and the read-offset
advance TIMING — ours advances at the kick, the DDK at TA finish). Next: get
ta3d-state's read on why ours' kick-2 TA doesn't terminate, then one EA3 slot to test
against the DDK control. Data: data/2026-11-03-0700-ea3-ab-ddk-render.bb.bin (DDK),
2026-11-03-0700-ea3-ddk-arm1.dmesg.log, and 2026-11-03-0530-* (ours lockup_TA earlier,
pre-restage; the ours arm this round is ea3-ours-dmesg.log in scratchpad).
