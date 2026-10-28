# sgx545-ce

An open driver stack for the PowerVR **SGX545** in the Intel Atom CE5300/CE5310
(Control4 EA-1 / EA-3), the same core as Cedarview's GMA 3600. The kernel side is
Intel's GPL DDK 1.7 ported to modern Linux; the userspace side is new, written from
reverse engineering and running natively on x86_64, with no Intel or Imagination
userspace code. It replaces the closed i386 Cedarview DDK in
[openHC](https://github.com/The1TrueJoe/openHC).

| Directory | What it is |
|---|---|
| [`kernel/`](kernel/) | `pvrsrvkm`, the SGX545 DRM/services kernel driver (GPL). openHC builds it in-tree. |
| [`srvinit/`](srvinit/) | Open GPU bring-up: loads the microkernel at boot in place of the DDK's `libsrv_init`. |
| [`mesa/`](mesa/) | The GL driver: a Mesa gallium driver (EGL + GLES2), kept as an overlay on a pinned Mesa release. |
| [`libpvr/`](libpvr/) | The PVR services client (bridge ioctls, memory, kicks) shared by srvinit, the Mesa winsys and the tools. `abi/` holds the struct layouts and bridge IDs generated from the kernel headers. |
| [`docs/`](docs/) | Hardware reference written from the reverse engineering, and the [build log](docs/log/BUILDLOG.md). |
| [`tools/`](tools/) | Reverse-engineering and test tooling: tracer, decoders, (dis)assemblers, frame generator, hardware tests, and DDK-side helpers for A/B testing. |

**Status.** On an EA-3 the GPU is started by `srvinit` and draws frames generated
entirely by our code, pixel-exact against the DDK. The Mesa driver is in progress;
the build log tells the story so far.

## Firmware

The one piece that is still Imagination's is the 53,096-byte SGX microkernel.
It is not in this repository: `srvinit/fetch-firmware.sh` extracts it from the
owner's own copy of the DDK.

## Licensing

`kernel/src/` is GPL-2.0 (Imagination Technologies, as published by Intel).
Everything else here is ours.
