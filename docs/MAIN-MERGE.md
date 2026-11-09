# Merging um-re into main

`um-re` holds the whole open stack. `main` still holds the earlier layout: a Buildroot package
for the DDK-derived `pvrsrvkm` (`Config.in`, `sgx545-ce.mk`, `Kbuild`, `src/`). This page is the
checklist for the merge. It is done as a merge commit (`git merge --no-ff`), at a time Joseph
picks.

State on 2026-11-09:

- `um-re` is 1258 commits ahead of `main`, and `main` has nothing `um-re` lacks. A trial merge
  (`git merge-tree origin/main origin/um-re`) is clean.
- `m2-drm` has 18 commits that are not in `um-re` yet, among them the runtime-PM stopgap
  (535b5dc), gma500's read-only display register dump (bd6c15f) and the released-context cache
  invalidate (8da962c).

## Before the merge

| | Item | State |
|---|---|---|
| 1 | Merge `m2-drm` into `um-re`, so the kernel side in the merge is the one the packages were built from. | open |
| 2 | Workflows: every one triggers on pushes to `um-re` or `m2-drm` only. Add `main` to the branches (or move the triggers there) in the same change as the merge. | open |
| 3 | Artifact lookups: `cedarview-live.yml` and `cedarview-kit.yml` take the newest artifact by `head_branch == "m2-drm"` / `"um-re"`. If development moves to `main`, these must follow. | open |
| 4 | Every workflow green at the merge commit (list below). | open (green on the branches today) |
| 5 | `main`'s old layout goes away: `Config.in`, `sgx545-ce.mk`, `Kbuild`, `Kconfig`, `Makefile`, `linux.fragment`, `sgx-init`, `src/`, `dev/`. Anything that builds from `main` as a Buildroot package breaks at the merge, so check downstream pins first and point them at a tag of the old `main` if they still need it. | open |
| 6 | Tag the current `main` (for example `pre-um-re`) before merging, so the old layout stays reachable. | open |
| 7 | Root clutter: `ce5300-regsample-ab.sh`, `ce5300-ro1-test2.sh` and `ce5300-stabilize.sh` sit at the top level. Move them under `tools/` or drop them. | done: moved to `tools/rig/` |
| 8 | `docs/log/data/2026-11-03-0700-ea3-ab-ddk-render.bb.bin` (1.4 MB) is named like a capture of a DDK render. Check that it holds no DDK-derived output; the repository is public. | done: removed from the tree |
| 9 | README current: the stack, the status, the hardware, how to install and test. | done (this change) |
| 10 | `docs/INSTALL.md` checked on hardware, with the CI packages (below). | done for userspace; kernel steps partly |
| 11 | No closed binaries: only the open microkernel is built and shipped; Intel's DDK image is never in the repository or a package. | done |
| 12 | No mention of the board project in code or docs (CI workflows may name the board repository). | done (a case-insensitive grep for the board project's name matches one workflow only) |
| 13 | American English in docs and kit text. British spellings (colour, behaviour, initialise, centre, ...) remain in 99 tracked `.md`/`.txt` files, mostly `docs/` and `docs/log/`. A careful pass is needed: leave code identifiers, register names and quoted text alone. The kit and the tester guide are clean. | open |

## Workflows

Latest run of each, 2026-11-09:

| Workflow | Branch it runs on | Latest |
|---|---|---|
| `mesa-debian` (the `mesa-sgx` and `sgx545-firmware` packages) | `um-re` | green (851c9c9) |
| `desktop-root` (the Debian 13 test root) | `um-re` | green (1120d5b) |
| `cedarview-live` (test stick v3.1) | `um-re` | green (6d9e49c) |
| `cedarview-kit` (test stick v2, superseded by v3) | `um-re` | green (34da9cc) |
| `sh-check` | `um-re`, pull requests | green (6d9e49c) |
| `powervr-sgx` (the driver for each board) | `m2-drm` | green |
| `powervr-sgx-deb` (the Cedarview kernel package) | `m2-drm` | green (42570447159) |
| `powervr-sgx-cedarview` | `m2-drm` | green |
| `powervr-sgx-upstream` (checks against drm-misc-next) | `m2-drm` | green |

Decide whether `cedarview-kit` (v2) stays now that v3 replaces it.

## INSTALL.md on hardware

On a CE5300 board (EA1) on 2026-11-09, in the Debian 13 test root, with the packages from
`mesa-debian` run 42557836753 (the ones stick v3.1 carries) and the kernel from
`powervr-sgx` 535b5dc. The run is `mesa/tests/desktop/install-e2e.sh`.

| Step | Result |
|---|---|
| `apt install ./sgx545-firmware_*.deb` | installs `/lib/firmware/powervr_sgx/sgx545_ukernel.bin`; the driver loads that file: "open microkernel ukernel-851c9c9, interface 1.0, features 0x2", "microkernel started", `renderD128` present |
| `apt install ./mesa-sgx_*.deb` | installs `/opt/sgx`, `/etc/ld.so.conf.d/00-sgx.conf` and `/etc/glvnd/egl_vendor.d/10_sgx.json`; `ldconfig` puts `/opt/sgx/lib` first |
| `glxinfo -B` on the Xfce desktop | "OpenGL renderer string: PowerVR SGX545", "2.1 Mesa 26.2.5"; es2gears 53 FPS |
| `apt remove mesa-sgx` | `/opt/sgx` and the settings files are gone; `glxinfo -B` reads "llvmpipe (LLVM 19.1.7)", "Mesa 25.0.7-2+deb13u1"; the desktop and es2gears keep working on Debian's Mesa |
| `apt remove sgx545-firmware` | the firmware file is gone |

Not covered by that run:

- The Cedarview kernel package (`linux-image-7.1.8-sgx`): it needs a Cedarview computer. CI
  installs it with `apt` into the test stick's root on every `cedarview-live` run, and testers
  boot that kernel from the stick.
- The CE5300 DKMS steps: the EA boards run their own system, not Debian, so the DKMS path has
  not been run on a Debian CE5300 host.

## Known issues to state with the merge

- **Parameter-buffer out-of-memory.** The open microkernel's out-of-memory handler (the
  partial render) is still in progress. Until it lands, the kernel gives the shared parameter
  buffer 16 MiB (0x1022 pages), which Minecraft does not fill: its largest scene took 0x529 pages
  (1,321) over a 15-minute run.
  With 4 MiB, Minecraft runs out of memory and the GPU hangs.
- **Runtime PM.** On a debug kernel (KASAN and lockdep), work submitted while the GPU was
  runtime-suspended could stall. A render context now keeps the GPU awake (535b5dc), the
  stopgap the packages carry. The root cause is not found yet; a release kernel never showed it,
  even with a 1 ms autosuspend delay.
- **Cedarview displays.** Some Cedarview panels stay dark (backlight on, no picture) with
  `gma500`, Debian's kernel as well as ours. Stick v3.1 records the display state and offers
  boot entries with fallbacks. This is a `gma500` issue, not the GPU driver's.
- **Transfers.** Mesa submits no transfer jobs. The transfer path (`powervr_sgx_blit`) works,
  but `-T` with a strided texture-heap source depends on what ran before it in the same boot.
- **Performance.** Correctness first. In the same-binary comparison on 2026-11-08 (the same
  gl4es and JVM, only the GLES2 library swapped), our Mesa ran Minecraft at 6.6 and 6.5 FPS
  against Intel's DDK at 10.7 and 13.2. Speed is the next phase.
