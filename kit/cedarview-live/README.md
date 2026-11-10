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
`sgx545-cedarview-kit-<series>` artifact (`sgx545-cedarview-kit-v3.4` for v3.4, from `SERIES`) for 90 days. There are no GitHub releases.

## The menu

Four GRUB entries run the same test with the kernel's DRM debug log on (`drm.debug=0x6
log_buf_len=4M`). The kernel turns display self-refresh off by default (gma500 0005). 1 as it
is, 2 `gma500_gfx.cdv_dsparb=0x3f01` (pipe B gets almost the whole display FIFO), 3
`gma500_gfx.cdv_sr=1` (self-refresh on, the old behavior, for comparison), 4 `cdv_dsparb=0x3f01`
with `video=LVDS-1:e`.
Never `video=LVDS-1:d` (it turns the panel off). Each run records its entry (`kit.entry=N`) and
the full command line (`cmdline.txt`), and puts the entry in its results file name. The dark
panels are intermittent, so testers are asked for entry 1 three times and entry 2 three times,
with a count of dark boots for each; 3 and 4 only if 1 and 2 are both dark every time. The menu shows before Linux starts, so a tester whose
panel goes dark can still go on: wait about 5 minutes, power off, boot again.

## The run

1. System facts, and whether the internal disks are read-only.
2. gma500 handing the GPU over, and powervr_sgx starting the open microkernel. Then the
   display state, read-only (`kit-display`: `display-start.txt`, again as
   `display-x-start.txt` right after X starts and before the modeset retry, and as
   `display-after-desktop.txt`): each DRM connector's status, enabled, dpms and modes; the
   backlight's values; gma500's kernel lines and display registers (`cdv_display_regs`);
   the atomic state (`/sys/kernel/debug/dri/*/state`) and `modetest -c -p`
   (CRTCs, planes and their framebuffers), so a lit-but-black scanout reads differently
   from a CRTC or plane that is off. With `xrandr.txt` from inside X, this shows what the display side believes,
   since the test cannot see whether the panel actually lit up.
3. `powervr_sgx_test` and `powervr_sgx_frame -n 10`.
4. The blit, then its candidates, stopping only if the GPU is lost:
   - plain;
   - microkernel variant B (the transfer bank), by swapping the firmware file and
     reloading the module, after which the default comes back. Plain on the default
     image against plain on variant B is the decisive pair;
   - extras that decide nothing (the texture-heap source `-T` fails on the CE5300 with
     either image): `-T` on variant B, then `-T` on the default, last so that a hang there
     cannot hide variant B's result;
   - when the test build has it, `-2`: a second client in a fresh process at the same
     addresses after the first exits (one client must not see another's leftovers).

   After each failure, the devcoredump and the driver's status are saved. The results are
   saved to the stick here once already.
5. The desktop. First, once, the dark-panel workaround: a full modeset of the LVDS output
   (`xrandr --output LVDS-1 --off; sleep 1; xrandr --output LVDS-1 --auto`, with the name
   xrandr reports), with gma500's read-only debugfs register dump (`cdv_display_regs`) and a screenshot before and
   after (`display-regs-before.txt`, `display-regs-after.txt`, `screenshots/00-*-modeset.png`;
   whether they differ goes in the summary). No raw MMIO from the stick. The results go to
   the stick (`kit-save`, written as `NAME.part` and renamed) as soon as X is up and every
   15 s during this step, each time with a fresh register dump (`display-regs-latest.txt`).
   From just before X starts until the first desktop screenshot (or 20 s after X), gma500's
   registers are sampled every 0.5 s into `display-regs-timeline.txt`, to see when pipe B's
   FIFO underrun bit sets and whether the palette is zero. All of this is in each save,
   so a tester who powers off a dark desktop still leaves the X-time logs and registers. Then the smoke test
   and screenshots `screenshots/NN-name.png`:
   1. the desktop;
   2. glxinfo;
   3. gears;
   4. glmark2;
   5. the browser;
   6. a window being dragged.

   A blank or one-color screenshot is a FAIL, and so is one that is the same as the one before (a frozen screen).
6. `results-kitv3-<model>-e<entry>-<date>.tar.gz` on `SGXKIT`, with `summary.txt`, dmesg, the journal
   and the Xorg log. Then a "done, attach this file" screen on tty1, which also asks the
   tester to say whether they could see the screen; the desktop stays on vt7.
