# Contributing

Thank you for helping! This project builds an open driver stack for the PowerVR SGX GPUs:
the Linux kernel driver, the Mesa driver and compiler, and an open microkernel. It started
on the SGX545, and support for other SGX cores is welcome.

## Before you start

- **Open an issue first** for anything larger than a fix, so we can agree on the approach.
  For a new GPU core, use the "New SGX core" issue template.
- **Branch from `dev`** and send pull requests to `dev`. `main` holds releases.

## The rules every contribution follows

1. **Nothing proprietary in the repository.** Nothing from Imagination's or a chip vendor's
   closed drivers: no source, no binaries, no disassembly listings or decompiler output.
   Everything here must stay redistributable under its open license, and acceptable
   upstream.
2. **Hardware facts come with evidence.** When a change depends on how the hardware behaves,
   say how you know: a test on real hardware, a capture, or a documented source. The
   process is in [docs/CLEANROOM.md](docs/CLEANROOM.md). Shipping code is written from
   `docs/`, so new hardware knowledge goes into `docs/` first.
3. **Sign off your commits** (`git commit -s`). Your `Signed-off-by` line certifies the
   [Developer Certificate of Origin](https://developercertificate.org/): that you wrote the
   change, or have the right to submit it under the file's license. The kernel driver goes
   to Linux, which requires it.
4. **Keep each file's license.** Every file starts with an SPDX line; new files use MIT,
   except under `drm/`, which is GPL-2.0-only as Linux requires. See [LICENSE](LICENSE).
5. **Test on hardware** and say what you ran. Changes that touch the GPU need at least the
   checks for their area in [docs/porting/](docs/porting/).

## Style

- Kernel code follows the Linux coding style and passes `checkpatch.pl --strict`.
- Mesa code follows Mesa's style (`.clang-format` in each directory).
- Commit messages: a short subject in the subsystem's style, then what changed and why.
- Comments are short; background belongs in `docs/`.
- Documentation and messages are in American English.

## Adding a GPU core

Adding a core means a set of table entries and configuration, plus evidence from real
hardware, not a fork of the driver. [docs/porting/](docs/porting/README.md) has the
overview and a checklist for each part.
