# Mixed submits work; a context that survives a reset does not; the 3D before and after

M2 item 4. CE5310 board RAM boots of abd4ca8, 2026-11-03 ~05:25 UTC, DDK microkernel, nothing
flashed, no power-cycle.

## uAPI suite: 31/31

Three new error-path tests:

| test | checks |
|---|---|
| `context-state-invalid` | `GET_CONTEXT_STATE` with nonzero padding or an unknown handle: -EINVAL |
| `submit-other-file` | another open file's context, syncobj or BO in a submit: -ENOENT |
| `submit-wait-unsignalled` | a wait on a syncobj that never had a fence: -EINVAL, and nothing queued |

## Render and transfer in one submit

`drm/tests/powervr_sgx_hang.c` runs sgxgen (frames) and sgxtex (blits) in one VM. The
mixed case is one `SUBMIT_JOBS` with two jobs:
1. a render on context A, signaling A's syncobj;
2. a transfer that waits on that syncobj and copies A's render target to another buffer.

The copy equals the frame in every pixel (16384/16384, 128x128), on the first boot
and in 5 more rounds. The in-submit dependency holds: the transfer reads the finished
frame, not a half-rendered one or the cleared buffer.

## Forced hang: the guilty context is handled, the survivors are not

Context B draws from an unmapped vertex address (`SGXGEN_FAULT`).
- The job times out after 2 s, and the kernel resets and restarts the microkernel.
- B reads back `GUILTY`, the reset count goes 0 -> 1, and A and the transfer context
  stay `OK`, all as the uAPI says.

The next render on A then hangs too. So does a render on a context C created after
the reset, in the same VM. The second restart fails (init status 0, `EVENT_STATUS`
TIMER only) and the device is marked lost.

The earlier restart runs (0355: 14/14) never hit this, because every frame after a
reset came from a new process with a new VM, PB and contexts. Here A, its RT dataset
and the VM's PB all lived through the reset. Two things the restart does not redo:
- **The HW render context's word 0.** The kernel writes 0x21; after the context's
  first kick the microkernel has cleared bit 0 (0x20, every dump since 0330). A
  freshly started microkernel that finds bit 0 clear has no reason to load the
  context's PB state again.
- **The VM's PB.** B's TA stopped part-way through taking pages from the free list,
  and nothing puts them back. C shares the PB, so C is affected as well.

Fix: before restarting, write every live context's HW context word 0 back to its
first-use value and reset every PB to its state at creation. It goes on the next
CE5310 board slot.

## For the open microkernel: what the DDK microkernel writes around the 3D

`powervr_sgx.snapshot=1 snap_gap_ms=200` makes debugfs `render_before` keep the
kernel-built structures as a frame's last kick is written. With the 200 ms gap that is
after the first kick's TA and before the kick that starts the 3D. `render` shows them
after the frame. `SGXGEN_DUMP_PROGS=1` adds the client's pixel-event, end-of-tile and
background programs. Two frames on one context; the words the 3D changed:

| structure | word | before -> after | |
|---|---|---|---|
| render details | +0xd4 | 0 -> 0x400 (frame 1), 0x4400 (frame 2) | the TA command's 3D flags word (+0x1c) |
| render details | +0xe8, +0xf0 | 0 -> sync3d +4, sync3d +0xc | the 3D sync's write/read-complete addresses |
| render details | +0xec | 0 -> 1 (frame 2 only) | |
| RT data | the frame's entry +0x04 (+0x54, then +0x1c) | render details -> 0 | |
| RT data | the frame's entry +0x34 (+0x84, then +0x4c) | 0 -> 0x10, then 0x1010 | |
| RT data | +0x4 | 1 -> 2, 3 -> 4 | |
| entry words | entword1, then entword0 | 0 -> 1 | |
| background block | +0, +4, +8 | 0 -> the TA command's 0x08/0x0c/0x10 | written in frame 1, kept |
| HW render context | +0x20, +0x24 | render details -> 0 | |
| TA3D control | +0x50/+0x64 | the RT-data entry -> 0 | |
| TA3D control | +0x70/+0x74 | 0 -> the entry, render details | |
| `rd_sync`, `hwsync_access`, the command's ready flag (+0x58) | | 1 -> 0 | |
| sync3d +0xc, done +4 | | +1 each | |

The client's programs (pixel event, end of tile, background) are the same before and
after: the microkernel does not write into them.

Data:
- `docs/log/data/2026-11-03-0525-drm-mixed-hang.txt` (uAPI suite, hang run);
- `docs/log/data/2026-11-03-0530-drm-3d-before-after.txt` (two frames with two kicks,
  one frame with one kick, every dump in full; a diff table at the top).
