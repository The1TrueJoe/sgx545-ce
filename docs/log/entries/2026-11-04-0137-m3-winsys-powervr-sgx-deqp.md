# The Mesa driver runs on powervr_sgx: dEQP-GLES2 identical to the M0 baseline

M3. CE5310 board RAM boot of m2-drm c95e32d (CI run 41752797996), DDK microkernel, 2026-11-04
01:37 to 02:37 UTC, nothing flashed, no power-cycle. Mesa 26.2.5 with the overlay at um-re
bf0cd9c.

## What changed in the winsys

`mesa/src/gallium/winsys/sgx545/drm/` used to build every structure the microkernel reads
and talk to pvrsrvkm through libpvr. It is now one frame recorder (`sgx545_frame.c`: rings,
PDS programs, USSE finalizing, TA state, VDM) over a kernel backend, `struct sgxws_kif`:

- `sgx545_powervr_sgx.c`, over the powervr_sgx uAPI:
  - GEM objects mapped at client-chosen addresses in the heaps from `HEAP_INFO`;
  - USSE code in code objects: the rings keep a CPU copy, and new code goes up with
    `UPLOAD_CODE` before each kick, only past what was uploaded before;
  - `CREATE_CONTEXT` and `CREATE_RT_DATASET` in place of the PB, HW context, RT data and
    render details;
  - one `SUBMIT_JOBS` render job per frame, with a `CMD_RENDER` stream and two `CMD_TA_KICK`
    streams written by the packers `docs/uapi/gen_stream.py user` generates from the stream
    XML;
  - completion on one timeline syncobj per device;
  - dma-buf import and export.
- `sgx545_pvr.c`, the libpvr code that was there before, moved out unchanged. It is built only
  where `mesa/build.sh` links libpvr in, so a downstream distribution can keep its stack.

`docs/WINSYS.md` says how the backend is picked (the kernel driver's name) and what each
does.

The end-of-tile programs moved out of the per-frame USSE ring, which rewrote code a slot at a
time. Under D12 code may only be appended while it is mapped. They are now made once per
target into two code pools used in turn. A full pool is replaced by a fresh code object at the
same address once no render runs code from it.

## On the hardware

`sgx545_frame_test` (the winsys without Mesa) drew the probe's clear and triangle at 720x480:
- pixel-identical to m2's `powervr_sgx_frame`;
- 300 frames, every one the same;
- 2.8 ms a frame;
- no job timeouts.

At first the triangle was missing. The test predated two fields the driver now fills: the
target's layout, which the test left at 0 (twiddled), and the draw's clip rectangle, which it
left empty. bf0cd9c gives it what the driver gives.

dEQP-GLES2 through EGL surfaceless on `renderD128`, run with `drive-deqp.py --chunk 50
--hold-lock`, `RECOVER=none`:

| | pass | fail | skip | warn | wedges | job timeouts |
|---|---|---|---|---|---|---|
| powervr_sgx (this run) | 16566 | 150 | 445 | 4 | 0 | 0 |
| M0 baseline, pvrsrvkm (`run-run34e1`, 5886029) | 16566 | 150 | 445 | 4 | | |

All 17,165 results are equal, test by test. The non-pass lines are in
`data/2026-11-04-0137-m3-deqp-powervr_sgx-nonpass.csv`. The run took an hour at about
5 tests/s.

`drive-deqp.py` now counts powervr_sgx's "job timed out" as a recovery, along with the DDK's
"Recovery triggered".

## Not yet shown

- GBM, kmsro and dma-buf sharing are built but not run on hardware:
  - `mesa/patches/0006` pairs a KMS-only device (simpledrm, card0 on m2's kernel) with
    powervr_sgx's render node;
  - the driver's scanout buffers are its own, exported to the display device;
  - m2's PRIME run proved both directions at the kernel level, with imported buffers
    correct only when listed in the job's BO list, which the winsys always does.
- X11 and DRI3 for Minecraft need glamor on our driver (EGLImage and more), not started.
- The run used the DDK microkernel. The open one is M1.
