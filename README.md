# powervr_sgx

> **Status:** this is an out-of-tree driver, maintained for the people who use it on Cedarview
> and CE5300 hardware. It was written with heavy use of an AI coding assistant. An RFC for the
> kernel driver and a draft merge request for the Mesa driver were posted in November 2026; Mesa
> declined the driver over its AI-generated code and provenance, so I withdrew both, and the
> driver is not headed upstream. Material derived from the vendor's closed drivers has been
> removed from the tree.

An open graphics driver stack for the PowerVR **SGX** GPUs. It supports the **SGX545**: the
GPU in Intel's Cedarview Atoms (N2600, N2800, D2500, D2550, D2700, "GMA 3600/3650") and in the
Intel Atom CE5300/CE5310. Other SGX cores are welcome ([docs/porting/](docs/porting/README.md)).
Every part runs from code in this repository. No Intel or Imagination binary is needed, in
the kernel, the firmware or userspace.

| Part | Where | What it is |
|---|---|---|
| Kernel driver | [`drm/`](drm/) | `powervr_sgx`, a new DRM driver (GEM, VM, a `drm_sched` job queue, runtime PM, devcoredump). On Cedarview it takes the GPU over from `gma500` through an auxiliary device. |
| Firmware | [`ukernel/`](ukernel/) | An open microkernel for the GPU's embedded processor, written in USSE assembly and built with our own assembler. It replaces Imagination's microkernel. |
| GL driver | [`mesa/`](mesa/) | `sgx`, a Mesa gallium driver: OpenGL 2.1, OpenGL ES 2.0, EGL, GBM, GLX and DRI3 for X, with a NIR to USSE compiler. Kept as a patch series on Mesa 26.2.5. |
| Packages | [`.github/workflows/`](.github/workflows/) | Debian 13 packages built in CI: `mesa-sgx`, `sgx545-firmware`, and a kernel for Cedarview. |
| Test sticks | [`kit/`](kit/) | A live Debian USB image that installs the packages, runs the GPU tests and a desktop session, and saves the results for a Cedarview tester ([guide](docs/TESTING-CEDARVIEW.md)). |
| Tools | [`tools/`](tools/) | The ISA assembler and disassembler, the offline microkernel emulator, decoders, frame and blit generators, and hardware tests. |
| Documentation | [`docs/`](docs/) | The hardware as we understand it ([ISA](docs/ISA.md), [state](docs/STATE.md), [microkernel](docs/UKERNEL.md), [uAPI](docs/UAPI.md)), and the [build log](docs/log/BUILDLOG.md). |

`kernel/` and `libpvr/` are the earlier path: Intel's GPL DDK kernel driver (`pvrsrvkm`)
ported to modern Linux, and our own client library. The new stack does not use them.

## Status

Tested on two CE5300 boards and, by testers, on Cedarview computers.

- **OpenGL ES 2.0:** dEQP-GLES2 passes 16,587 tests with 129 failures, 0 GPU hangs and 0 resets
  on our kernel driver and microkernel. That is no test worse than the same Mesa on Intel's
  kernel driver and microkernel (16,566 passes).
- **Desktop:** Xorg (modesetting with glamor) and Xfce run on the GPU, with Firefox, glxgears
  and glmark2. A GPU hang in one program is recovered without taking down the desktop.
- **Games:** Minecraft 1.7.10 runs in its world for 15 minutes without a GPU recovery.
- **Cedarview:** the microkernel starts and draws frames on Cedarview's SGX545 (revision
  1.0.13). Some Cedarview panels stay dark with `gma500`; the test stick records why.
- **Known issues:** see [docs/MAIN-MERGE.md](docs/MAIN-MERGE.md#known-issues-to-state-with-the-merge).
  The largest is the parameter-buffer out-of-memory path, which is still in progress; a 16 MiB
  buffer keeps real workloads clear of it.

The [build log](docs/log/BUILDLOG.md) tells the story so far.

## Installing

[docs/INSTALL.md](docs/INSTALL.md) installs the packages on Debian 13 and removes them again.
Debian's own kernel and Mesa stay installed. The packages come from CI runs; there are no
releases yet.

## Testing on a Cedarview computer

[docs/TESTING-CEDARVIEW.md](docs/TESTING-CEDARVIEW.md) walks a tester through the USB stick:
write it, boot it, attach the results file to the issue.

## Licensing

- `drm/` (the `powervr_sgx` kernel driver): GPL-2.0-only; its uAPI header is
  GPL-2.0-only WITH Linux-syscall-note, or MIT.
- `kernel/src/`: GPL-2.0, Imagination Technologies, as published by Intel.
- `libpvr/`: GPL-2.0-only (built against and generated from `kernel/src`'s headers).
- Everything else here (the microkernel, the Mesa driver, the tools, the test sticks): MIT.

## Contributing

Contributions are welcome, including support for other PowerVR SGX cores. Start with
[CONTRIBUTING.md](CONTRIBUTING.md) and [docs/porting/](docs/porting/README.md).
