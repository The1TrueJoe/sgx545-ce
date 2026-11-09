# Cedarview test stick v3: live Debian

One boot answers everything a Cedarview tester can tell us. A live Debian 13 system on a
USB stick, with exactly the packages [docs/INSTALL.md](../../docs/INSTALL.md) installs, runs
the GPU tests, tries the blit's candidate fixes, brings up an Xfce desktop and runs a
smoke test with screenshots. Everything goes into one results file on the stick. The
computer's own disks are never written to.

| file | what |
|---|---|
| `build.sh DEBS TESTS OUTDIR VERSION [XFERBANK_FW]` | the image, as root on a Linux runner: mmdebstrap of trixie with `mesa/tests/desktop/packages.txt` and `packages.txt`, the kernel, firmware and Mesa packages installed with apt, live-boot's squashfs on one FAT32 partition `SGXKIT`, GRUB for BIOS, x86_64 EFI and i386 EFI |
| `kit-live-run` | the run on tty1 (systemd `kit.service`): system facts, the GPU, the tests, the blit variants, the desktop, the results |
| `kit-desktop`, `kit-session` | Xorg on vt7 and an Xfce session as `desk`, which runs `kit-smoke` |
| `kit-smoke` | the desktop's smoke test: glxinfo, gears, glmark2, a browser on `test.html`, a dragged window; a screenshot of each, checked for a blank screen |
| `99-kit-readonly.rules`, `initramfs-hook` | every non-USB disk read-only from the initramfs on |
| `qemu-test.sh` | the CI check: boots the image in QEMU next to a disk that must stay unchanged |
| `SERIES` | the kit's version for testers (`v3`), on the screen and in the results file name |

CI: `.github/workflows/cedarview-live.yml` takes the newest `powervr-sgx-deb-cedarview`
(kernel), `powervr-sgx-cedarview` (static tests) and `mesa-sgx-trixie` (firmware and Mesa
packages), builds microkernel variant B from `ukernel/`, and keeps the image as the
`sgx545-cedarview-kit-v3` artifact for 90 days. There are no GitHub releases.

## The run

1. System facts, and whether the internal disks are read-only.
2. gma500 handing the GPU over, and powervr_sgx starting the open microkernel.
3. `powervr_sgx_test` and `powervr_sgx_frame -n 10`.
4. The blit, then its candidates, stopping only if the GPU is lost:
   - plain;
   - the texture-heap source (`-T`);
   - microkernel variant B (the transfer bank), by swapping the firmware and reloading the
     module, after which the default comes back.

   After each failure, the devcoredump and the driver's status are saved. The results are
   saved to the stick here once already.
5. The desktop, with the smoke test and screenshots `screenshots/NN-name.png`:
   1. the desktop;
   2. glxinfo;
   3. gears;
   4. glmark2;
   5. the browser;
   6. a window being dragged.

   A blank or one-colour screenshot is a FAIL.
6. `results-kitv3-<model>-<date>.tar.gz` on `SGXKIT`, with `summary.txt`, dmesg, the journal
   and the Xorg log. Then a "done, attach this file" screen on tty1, and the desktop stays
   on vt7.
