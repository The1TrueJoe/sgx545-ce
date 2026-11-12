# powervr_sgx

> **Working on this repo?** Read [AGENTS.md](AGENTS.md) first: the end goal (upstream Linux, Mesa and
> linux-firmware support for Cedarview), the standards the work must meet, and the milestones.

An open graphics driver stack for the PowerVR **SGX** GPUs. It supports the **SGX545**: the
GPU in Intel's Cedarview Atoms (N2600, N2800, D2500, D2550, D2700, "GMA 3600/3650") and in the
Intel Atom CE5300/CE5310. Every part runs from code in this repository. No Intel or
Imagination binary is needed, in the kernel, the firmware or userspace.

## GPUs

One kernel driver, one Mesa driver and one microkernel source are meant to cover the whole
SGX family: a new core is added as data in each of them, not as a copy of the code.

| Core | ISA | Example hardware | Status |
|---|---|---|---|
| SGX545 | USSE | Intel Cedarview (Atom N2600, N2800, D2500, D2550, D2700); Intel Atom CE5300/CE5310 | **Supported** |
| SGX530 | USSE | TI OMAP3 (OMAP3430, OMAP3630, DM3730), TI AM335x and AM3517 | **In bring-up** on an OMAP3630 board: the kernel driver finds and powers the core and reads its ID; the microkernel does not start yet |
| SGX535 | USSE | Intel Poulsbo (GMA 500, Atom Z5xx) | Wanted |
| SGX540 | USSE | TI OMAP4430/4460, Ingenic JZ4780 | Wanted |
| SGX544 | USSE2 | TI OMAP4470, OMAP5 and AM57xx, Allwinner A31 and A83T | Wanted; needs USSE2 support in the compiler and assembler |
| SGX543, SGX554 | USSE2 | Mostly phones and tablets | Wanted, as SGX544 |

The cores marked USSE share an instruction set with the SGX545, so most of a port is that
core's facts and its platform glue. USSE2 is a different instruction set and a larger
project. If you have one of these and want to help, see [Contributing](#contributing) and
the porting guide, [docs/porting/](docs/porting/README.md).

| Part | Where | What it is |
|---|---|---|
| Kernel driver | [`drm/`](drm/) | `powervr_sgx`, a new DRM driver (GEM, VM, a `drm_sched` job queue, runtime PM, devcoredump). On Cedarview it takes the GPU over from `gma500` through an auxiliary device. |
| Firmware | [`ukernel/`](ukernel/) | An open microkernel for the GPU's embedded processor, written in USSE assembly and built with our own assembler. It replaces Imagination's microkernel. |
| GL driver | [`mesa/`](mesa/) | `sgx`, a Mesa gallium driver: OpenGL 2.1, OpenGL ES 2.0, EGL, GBM, GLX and DRI3 for X, with a NIR to USSE compiler. Kept as a patch series on Mesa 26.2.5. |
| Packages | [`.github/workflows/`](.github/workflows/) | Debian 13 packages built in CI: `mesa-sgx`, `sgx545-firmware`, and a kernel for Cedarview. |
| Test sticks | [`kit/`](kit/) | A live Debian USB image that installs the packages, runs the GPU tests and a desktop session, and saves the results for a Cedarview tester ([guide](docs/TESTING-CEDARVIEW.md)). |
| Tools | [`tools/`](tools/) | The ISA assembler and disassembler, the offline microkernel emulator, decoders, frame and blit generators, and hardware tests. |
| Documentation | [`docs/`](docs/) | The hardware as we understand it ([ISA](docs/ISA.md), [state](docs/STATE.md), [microkernel](docs/UKERNEL.md), [uAPI](docs/UAPI.md)), and the [build log](docs/log/BUILDLOG.md). |

`kernel/`, `srvinit/` and `libpvr/` are the earlier path: Intel's GPL DDK kernel driver
(`pvrsrvkm`) ported to modern Linux, with our own bring-up and client library. They stay for
comparisons against the DDK; the new stack does not use them.

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
  The parameter-buffer out-of-memory handler, listed there as in progress, is now done and
  confirmed on the hardware.

The [build log](docs/log/BUILDLOG.md) tells the story so far.

## Installing

[docs/INSTALL.md](docs/INSTALL.md) installs the packages on Debian 13 and removes them again.
Debian's own kernel and Mesa stay installed. The packages come from our apt repository at
`https://telaak.dev/powervr_sgx`, or as `.deb` files from the
[releases](https://github.com/The1TrueJoe/powervr_sgx/releases) (the first is v0.1.0).

## Testing on a Cedarview computer

[docs/TESTING-CEDARVIEW.md](docs/TESTING-CEDARVIEW.md) walks a tester through the USB stick:
write it, boot it, attach the results file to the issue.

## Licensing

- `drm/` (the `powervr_sgx` kernel driver): GPL-2.0-only; its uAPI header is
  GPL-2.0-only WITH Linux-syscall-note, or MIT.
- `kernel/src/`: GPL-2.0, Imagination Technologies, as published by Intel.
- Everything else here (the microkernel, the Mesa driver, the tools, the test sticks): MIT.

## Contributing

Contributions are welcome, including support for other PowerVR SGX cores. Start with
[CONTRIBUTING.md](CONTRIBUTING.md) and [docs/porting/](docs/porting/README.md).
