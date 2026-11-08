# Cedarview test stick

A USB image that a Cedarview owner (Atom D2500/D2550/D2700/N2600/N2800, GMA 3600/3650)
boots to test powervr_sgx on their machine, with no kernel knowledge needed. The tester's
side is [docs/TESTING-CEDARVIEW.md](../../docs/TESTING-CEDARVIEW.md).

| file | what |
|---|---|
| `build.sh KERNDIR OUTDIR VERSION [FIRMWARE]` | the image, as root on a Linux runner: Debian bookworm minbase (mmdebstrap) as a squashfs root, m2-drm's `powervr-sgx-cedarview` kernel and modules, GRUB for BIOS, x86_64 EFI and i386 EFI |
| `kit-init` | PID 1: tmpfs for everything writable, other disks read-only, then `kit-run`, then a shell or power off |
| `kit-protect` | `blockdev --setro` on every block device but the stick; run at boot and before each step |
| `kit-stick` | the stick's disk, found by the image's fixed MBR disk id |
| `kit-run` | the test run and the results file |
| `grub.cfg`, `README-STICK.txt` | the boot menu, and the note on the stick's FAT partition |

CI: `.github/workflows/cedarview-kit.yml` builds it on every push to `kit/cedarview/` and on
a published release tagged `cedarview-kit-*`, which gets the image attached. By default it
takes the newest `powervr-sgx-cedarview` artifact. That artifact is kept for 14 days, so
the image carries its own copy of the kernel.

## The image

- MBR with disk id `5c45c545`, so old BIOS-only netbooks boot it.
- Partition 1: FAT32 `SGXKIT`, type 0x0c (Windows gives it a drive letter). It holds GRUB,
  the kernel and the results.
- Partition 2: the squashfs root, `root=PARTUUID=5c45c545-02`. There is no initramfs: the
  kernel has USB storage and squashfs built in.

## What the run does

1. System facts: DMI names (no serial numbers), CPU, `lspci -nnvvv -s 00:02.0`.
2. `modprobe gma500_gfx`, and whether it published `gma500_gfx.sgx.*` on the auxiliary
   bus.
3. `modprobe powervr_sgx`. The driver reads the core id and revision and refuses a core it
   doesn't know before writing any register. A known core gets the open microkernel
   (`sgx545_ukernel.bin`, built in CI from `ukernel/`, secure mode), never Intel's.
4. Only when the microkernel started: `powervr_sgx_test` (the uAPI tests),
   `powervr_sgx_frame -n 10` and `powervr_sgx_blit -n 4`. The first job timeout ends this
   step. Last, on a GPU that was fine, `powervr_sgx_hang`: a deliberate fault and the
   recovery from it, with exactly one reset.
5. `results-<model>-<date>.tar.gz` on `SGXKIT`, with dmesg, lspci, /proc/iomem,
   /proc/interrupts and the driver's status and devcoredumps. MAC addresses and serial
   numbers are removed.

- **v1** stopped at step 3: Cedarview's core (SGX545 1.0.13) wasn't known yet.
- **v2** knows it and runs steps 3 and 4 on the open microkernel.
- **Later:** our Mesa, kmscube, glmark2, Xorg and a dEQP subset.
