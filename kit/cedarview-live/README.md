# Cedarview test stick v5: live Debian, one hands-off boot

One boot answers everything a Cedarview tester can tell us, with nothing for the tester to
do: flash, boot, walk away, attach one file. A live Debian 13 system on a USB stick, with
exactly the packages [docs/INSTALL.md](../../docs/INSTALL.md) installs, runs the GPU tests,
tries the blit's candidate fixes, then brings up X once per display/desktop variant and runs
a smoke test with screenshots in each, every step under a time limit, and turns the computer
off. Everything goes into one results file on the stick. The computer's own disks are never
written to.

| file | what |
|---|---|
| `build.sh DEBS TESTS OUTDIR VERSION [XFERBANK_FW]` | the image, as root on a Linux runner: mmdebstrap of trixie with `mesa/tests/desktop/packages.txt` and `packages.txt`, the kernel, firmware and Mesa packages installed with apt, live-boot's squashfs on one FAT32 partition `SGXKIT`, GRUB for BIOS, x86_64 EFI and i386 EFI |
| `kit-live-run` | the run on tty1 (systemd `kit.service`): the step list, the progress marker and resume, the progress screen, the save and the power-off |
| `desktop-variants` | one X attempt per line: gma500 parameters, environment, full or short smoke test |
| `kit-xvariant` | one desktop attempt: the parameters, X under a watchdog (stall and time limits), the hang capture, X stopped whatever its state |
| `kit-session`, `kit-xorg` | the X session (Xfce as `desk`, then `kit-smoke`) and Xorg's debug environment |
| `kit-smoke` | the smoke test, full (glxinfo, gears, glmark2, a browser on `test.html`, a dragged window) or short; a screenshot of each, checked for a blank screen |
| `kit-hangcap` | a stuck X's state, wchan and thread stacks, the DRM state, sysrq-w, gdb only outside D state |
| `kit-wdt` | the chipset watchdog (iTCO) around the GPU and desktop steps, petted from the run's heartbeat |
| `kit-display`, `kit-save` | the display side's dumps and register timeline; the results file, written and renamed |
| `99-kit-readonly.rules`, `initramfs-hook` | every non-USB disk read-only from the initramfs on |
| `qemu-test.sh` | the CI check: boots the image in QEMU next to a disk that must stay unchanged |
| `SERIES` | the kit's version for testers (`v5`), on the screen and in the results file name |

CI: `.github/workflows/cedarview-live.yml` takes the newest `powervr-sgx-deb-cedarview-kit`
(kernel), `powervr-sgx-cedarview` (static tests) and `mesa-sgx-trixie` (firmware and Mesa
packages), builds microkernel variant B from `ukernel/`, and keeps the image as the
`sgx545-cedarview-kit-<series>` artifact (`sgx545-cedarview-kit-v5` for v5, from `SERIES`) for 90 days. There are no GitHub releases.
On a `release/*` branch all three come from that branch's own runs, so the stick carries
the release's kernel and Mesa together.

v5 is v4's run with the 0.2 kernel and Mesa (uAPI revision 4) from `release/v0.2`. Only
the amd64 and `all` packages go on the stick; the i386 `mesa-sgx` stays off it.

## One boot

GRUB has one entry and boots it by itself (`drm.debug=0x6 log_buf_len=4M`). The run is a list
of steps (`kit-live-run`): system facts, the driver, the GPU tests, the blit set, then one X
attempt per line of `desktop-variants` (A: the kernel's defaults with the full smoke test,
B: `cdv_dsparb=0x3f01`, C: `cdv_sr=1`, the old behavior). The gma500 parameters are written to
`/sys/module/gma500_gfx/parameters/` and applied with a blank/unblank; a variant whose
parameters can't be set is skipped.

Every step has a time limit. For X: up within 40 s, the first desktop screenshot within 90 s,
the whole attempt within 240 s. When Xorg.0.log stands still for 5 s before the session starts
(and again 30 s later), or a limit is hit, `kit-hangcap` records the stacks; X is then stopped
and the next variant runs. A lost GPU skips the remaining GPU and desktop steps; the run still
saves and powers off.

`kit-progress.txt` on SGXKIT records each step before it starts. A boot that finds a step
started but not done records it as HANG and resumes after it, in the same results file
(the earlier boot's results under `boot-N/`). The chipset watchdog (iTCO, 120 s) is armed
from the GPU steps to the end and petted while the run's heartbeat is fresh, so a hard hang
restarts the computer by itself. Test switches: `kit.test=xhang` (X stops dead in variant A),
`kit.test=gpuhang` (a forced GPU hang after the GPU tests), `kit.nopoweroff`.

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
