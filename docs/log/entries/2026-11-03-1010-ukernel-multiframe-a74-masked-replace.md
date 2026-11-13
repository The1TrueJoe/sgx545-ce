# Multi-frame: 0xa74 masked field-replace; renders stay byte-identical across frames

Toward the M1 exit (multi-frame, then the full dEQP run), the first step is making
0xa74 correct beyond the first frame. d3119a4 OR'd the command's flags into 0xa74,
which is right for one render but accumulates stale field bits from frame 2 on (and
across different draw flags). The DDK does a masked field-REPLACE, each field with
its own mask, so srvinit's init bits (0x00208200, outside every mask) persist while
each field is rewritten (ta3d-state, from the firmware):

- do_ta TA kick (pc 0x0986):   0xa74 = (0xa74 & ~0xf8000000) | cmd+0x14   (TA field b27-31)
- do_3d 3D start (pc 0x1244):  0xa74 = (0xa74 & ~0x07c00000) | rd+0x13c   (3D field b22-26)
- k_transfer start (pc 0x13a1):0xa74 = (0xa74 & ~0x07c00000) | cmd+0x194  (3D field, transfer)
- ta_finished (pc 0x15b7):     0xa74 &= ~0xf                              (clear b0-3 between scenes)

(The assembler has no immediate-AND, so each is `limm rN, #<mask>; and; or`.)
Removed 0xa74 from run_ours' PLUMBING set so the oracle now COMPARES the 0xa74
sequence per phase (it was never checked before, which is how the clear-to-0 bug in
d3119a4 slipped through). Offline: run_ours fake/v1/v2/xfer/xfer-rect all MATCH with
0xa74 compared; test_emu 5/5; test_commands OK.

## HW (CE5300 board p1, uk-mf.bin md5 a45a421c): multi-frame renders stay byte-identical

On a boot that rendered frame 1, ran 10 consecutive frames (frame-dump x10 after one
srvinit -o). 6/10 rendered "done", and **every rendered frame's color buffer is
byte-identical to frame 1** (md5 56f8466a == the DDK reference triangle): frames
1,2,3,7,8,10. No cross-frame corruption, and a timed-out frame does not poison the
next (frames 4-6 timed out, 7-8 rendered correctly again — the kernel HWRs the stuck
TA and the next frame renders clean). So the open microkernel renders repeatedly and
the image is stable across frames.

## The 4/10 timeouts are the per-frame TA race (next step, not this fix)

The 4 TIMED-OUT frames (4,5,6,9) are the lockup_TA race ta3d-state's race.py models:
kick 2 arriving while kick 1's TA is still running. It is per-FRAME (every frame has
the two-kick sequence), so it caps the consecutive-render rate (~60% here), not the
image. "10 frames in a row" and the full dEQP run need the claim-the-CCB-entry-
before-the-flag serialization fix so every frame reaches do_3d. That is the next step.
