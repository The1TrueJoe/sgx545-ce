# M1 exit on the open microkernel 8158141: stopped in the first chunk

CE5300 board RAM boot of m2-drm 3d0604b (CI run 42510583614), 2026-11-08 18:44 to 19:00 UTC, nothing
flashed, no power cycle. powervr_sgx loaded with `firmware=powervr_sgx/sgx545_ukernel.bin`,
dmesg "open microkernel ukernel-8158141". Mesa: um-re d2357fd with the compiler from 868b2eb,
EGL surfaceless on `renderD128`.

## What ran

| run | result |
|---|---|
| `sgx545_frame_test -c`, 20 processes x 30 frames at once, 3 rounds | 1800 frames, all the same image, no job timeouts |
| dEQP-GLES2, `drive-deqp.py --chunk 50 --hold-lock --max-recoveries 0` | stopped after chunk 1: 2 job timeouts, 12 `color_clear` fails that pass on the DDK microkernel |
| the 19 `functional.color_clear` tests, one deqp-gles2 process | 2 timeouts; the first 7 pass, then `subclears_rgba` and almost everything after it fails (max diff RGBA(178,163,173,128)) |
| the same 19, one process per test | all pass |
| all `color_clear` (43 pass, 7 skip in the baseline), same Mesa, DDK microkernel | identical to the baseline, no timeouts |
| `sgx545_frame_test -n 600`, one process, fresh boot, open microkernel | job timeout at frame 40, both tries |
| the same on the DDK microkernel | 600 frames, all the same, 1.86 ms a frame |

## Two microkernel bugs

- **A render job hangs after about 40 frames on one context.** The winsys alone shows it, with no
  Mesa. Many short-lived contexts adding up to the same number of jobs don't. BIF_FAULT is 0 in
  the devcoredump.
- **After its own reset, the open microkernel runs nothing.** Every first job times out again
  ("ccb 3, consumed 2"). Reloading powervr_sgx with the open image doesn't help ("microkernel
  did not start ... device lost"). Reloading it with the DDK image on that GPU runs 600 frames,
  so the hardware is fine and the open microkernel's start misses a reset the DDK's does.

powervr_sgx itself behaved: it caught every timeout, reset, and wrote devcoredumps. The
devcoredumps, dmesg and caselist went to open-ukernel.

## Not yet shown

- The rest of dEQP-GLES2 and Minecraft on the open microkernel: a hang ends the run, so both
  wait for a fixed image.
- `drive-deqp.py`'s `CHUNK_HOOK` reads debugfs; mount it on the box first.
