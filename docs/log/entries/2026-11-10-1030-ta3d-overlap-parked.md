# TA/3D overlap: achievable, but parked

The microkernel already keeps the GPU as busy as the DDK. Overlapping a
render's TA with the previous render's 3D — the obvious next scheduling win —
turns out to buy almost nothing on this workload, and the one bug blocking our
overlap resisted six fixes. So overlap is parked behind `UK_OVERLAP` (off by
default), documented, and the frame-rate work moves to the GPU work itself.

## What the control experiment settled

The vendor DDK firmware (`sgx545-ukernel-1.7.862890.bin`) was loaded under our
`powervr_sgx` kernel through its DDK-compat path and run on the CE5310 board at
`SGX_SLOTS=4`. It ran `overlap=1` cleanly — ftrace showed `hw job count:2` for
382 windows, 160 of 265 kicks issued while the previous job was still running,
zero timeouts, into steady state. So overlap **is** achievable on this
hardware and kernel; whatever blocks ours is in our microkernel, not the
platform.

But the same run showed the ceiling. Matched to the vendor at join+90s steady
state:

| µK | fps (stand/walk) | GPU busy | inter-job gap | all-idle |
|---|---|---|---|---|
| ours serial | 13.5 / 12.0 | 97% | 0.61 ms | 3% |
| vendor serial | 13.5 / 10.3 | 95% | 0.88 ms | 6.5% |
| vendor overlap=1 | 13.7 | 97% | 0.48 ms | 3.8% |

Our serial microkernel equals the vendor's and idles *less*. The frame is
GPU-work bound — TA ≈ 41 ms + 3D ≈ 28 ms, the same on both — so overlap on the
vendor buys 13.5→13.7 fps. (An earlier "27% idle, ~35% headroom" reading was a
warm-up sample at join+35s compared against the vendor's steady state; the
matched sample retracts it.)

## The bug we did not close

On the first overlapped pair our queued 3D's `0x428` kick is written but never
latches `TRIG_3D`; the 3D never starts and the microkernel idles with the
render's rendezvous armed. Everything observable at the kick is valid — DPM
slot, free-list, region headers, idle pipe, no BIF fault, clean EVENT_STATUS.
Six DDK-faithful fixes (invalidate order, TPC-flush placement, the early BIF
cache request, `0xa08`-once, the INVALDC-on-PD-hit skip) brought our queued-start
sequence and full register state at the kick to a register-for-register match
with the DDK's — and it still hangs. The difference is something the offline
emulator cannot model and that only appears when a 3D is genuinely in flight on
the hardware.

`docs/UKERNEL.md` §11 has the per-fix detail, the ruled-out theories, and the
one lead left if anyone reopens it: m2's alternating BIF directory lists (route
the queued render's 3D to list 2, `DIR_LIST_BASE2` written from the context PD).

## Where it leaves the frame rate

The microkernel is at parity with the DDK, serially and (as far as the vendor
proves) at the overlap ceiling. The ~13.5 fps is set by the per-frame GPU work.
Getting beyond it is the Mesa driver and USSE compiler's problem — the pixel
shaders' F16 / two-phase work and the vertex side — not the microkernel's.
