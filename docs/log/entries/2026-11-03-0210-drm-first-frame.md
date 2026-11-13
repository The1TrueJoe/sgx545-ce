# powervr_sgx renders its first frames: kernel-built scenes from client streams, 10/10 at 256x256

![first frame](../img/2026-11-03-0210-drm-first-frame.png)

The first real TA/3D frames through the new DRM driver. CE5310 board (CE5310, core rev 1.0.14),
2026-11-03 ~02:10 UTC, RAM-booted `7.1.8-m2drm` (driver de42b2f), DDK microkernel with the
kernel heaps shared into the VM (`share_kernel_heaps`, see
`2026-11-03-0135-drm-first-ta`), nothing flashed, no power-cycle.

## The frame

`drm/tests/powervr_sgx_frame.c` uses `tools/gen`'s frame builder on the DRM uAPI: client
memory through `CREATE_BO` and `VM_MAP` into the client heaps, a render context, an RT
dataset, and the clear plus one triangle recorded into a VDM stream. One `SUBMIT_JOBS`
render job carries a `CMD_RENDER` stream (pixel-event and background PDS programs, background
depth) and two `CMD_TA_KICK` streams (the scene's VDM stream, then the terminate words). The
kernel builds everything the microkernel reads: HW render context, CCB and its two TA
commands, PB, free list, PB descriptor, render details, RT data, region headers, tail
pointers, background object block, and the sync words.

```
frame 0: render done, centre ff00ff00 (want ff00ff00), corner ff000080 (want ff000080)
sgxgen: render done after 7999 us
firmware running: 1 lost: 0 resets: 0 irqs: 3
```

- **Pixels exact**: the triangle's green, the clear's blue, at the expected places.
- **The render-done interrupt works**: the microkernel raises SW_EVENT at the end of each
  render (3 per frame), and the out-fence signals from it.
- **10 of 10 frames right** at 256x256 on a clean boot, the triangle alternating between
  green and yellow so a stale frame cannot pass: 30 interrupts, no resets.

## Not yet

- **720x480 (4x4 macrotile mode) hangs on the third frame**, reproducibly on a clean boot
  (correction, `2026-11-03-0310-drm-second-context`: every such run was the second frame
  process; as the first, 720x480 runs ten frames):
  frames 0 and 1 render, frame 2 times out. 256x256 (2x2 mode) runs 10 frames. The frame-2
  render reuses RT data entry 1 and region-header array 1 for the first time in mode 1.
- **Restarting the firmware after a hang fails** ("microkernel did not start", device
  lost); reloading the module recovers. After a timeout the context is guilty and its
  later jobs get `-ECANCELED`, as the uAPI says.
- **One-kick scenes (TA flags 0x73)** failed, but only on a GPU already through failed
  restarts; unconfirmed. Two-kick scenes are the captured form and the one that works.

## Why two kicks

The first attempts (`2026-11-03-0135-drm-first-ta`) used one kick that also renders (0x73),
the form the Mesa winsys sends to the DDK's kernel driver. A field-by-field A/B against the
same frame on the DDK path (`tools/gen/frame`, `SGXGEN_DUMP`, data
`2026-11-03-0200-ddk-ab-dump.txt`) found the kernel-built structures identical but for the
kick form, and switching to the DDK path's two kicks gave the first frame.

Data: `docs/log/data/2026-11-03-0210-drm-first-frame-render.txt` (the kernel-built
structures of the first frame, debugfs `render`), `2026-11-03-0215-drm-10-frames-render.txt`.
