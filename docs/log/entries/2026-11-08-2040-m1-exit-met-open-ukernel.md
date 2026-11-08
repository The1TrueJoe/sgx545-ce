# M1 exit met: dEQP-GLES2 on the open microkernel, no regression

CE5300 board (EA-3), RAM boot of m2-drm 3d0604b (CI run 42510583614), 2026-11-08 19:31 to
20:34 UTC, nothing flashed, no power cycle. powervr_sgx loaded with the open microkernel
f0dc24d (open-ukernel's "load the DPM free lists once per PB" fix, raw image md5 c87d5b90),
wrapped with features 0. dmesg: "open microkernel ukernel-plgate-f0dc24d, interface 1.0,
features 0x0". Mesa: um-re d2357fd with the compiler from 868b2eb, EGL surfaceless on
`renderD128`.

## Before the run

The winsys test, `sgx545_frame_test -c` (every frame read back and compared):
- 600 frames in one process: 0 differ, no job timeouts. 8158141 stopped at frame 40, on the
  DPM free-list leak (2026-11-08-1900).
- 20 processes x 30 frames at once: all the same image, no job timeouts.

## dEQP-GLES2

`drive-deqp.py --chunk 50 --hold-lock --max-recoveries 0`, `RECOVER=none`, the debugfs status
after every chunk:

| | pass | fail | skip | warn | job timeouts | resets |
|---|---|---|---|---|---|---|
| open microkernel f0dc24d (this run) | 16587 | 129 | 445 | 4 | 0 | 0 |
| M0 baseline, pvrsrvkm + DDK (`run-run34e1`, 5886029) | 16566 | 150 | 445 | 4 | | |

- Test by test, no result got worse.
- 21 tests went from Fail to Pass, all `functional.fbo.render.resize.*` (`rbo_*` and `tex2d_*`,
  every format). That is this Mesa build, not the microkernel: the M3 run on the DDK microkernel
  had them failing on the earlier Mesa.
- The status after the last chunk: firmware lost 0, resets 0, microkernel lockups 0.

The run took an hour at about 5 tests/s, the same as on the DDK microkernel.

## Not yet shown

- Minecraft on the open microkernel: it hangs early with an MMU fault at the top page of the 3D
  parameter heap (2026-11-08-2230). dEQP's scenes never reach it.
- After that fault, the open microkernel does not restart ("device lost"), and only a reboot
  brings it back. The DDK image restarts on the same GPU.
