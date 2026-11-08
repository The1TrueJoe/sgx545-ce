# Upstream readiness

A living checklist of what each component needs before any of it is proposed
upstream. **Nothing here has been sent anywhere.** Naming in upstream-facing
text is neutral: the platform is the **Intel Atom CE5300**, the GPU is
**PowerVR SGX545** (the SGX5xx family generally); dev-box specifics and the
porting history stay in `docs/log/`.

Other components track their own readiness in their own docs
(`docs/DRIVER-UPSTREAM.md`, `docs/COMPILER-UPSTREAM.md`); this file collects the
cross-cutting status and the firmware section.

## Firmware (the open SGX microkernel, `ukernel/`)

The microkernel is MIT-licensed source (`ukernel/ukernel.py`) assembled to a
small binary a loader runs on the SGX core. Upstreaming has two destinations:
the **source** (its own repository) and the **built binary** (linux-firmware,
loaded by the DRM driver).

| item | state |
|---|---|
| Reproducible build | **Done.** Pure-CPython assembler, no timestamps/paths/randomness; byte-identical `.bin`+`.json` on any machine, any CPython ≥ 3.7. One command: `ukernel/build.sh`. |
| Pinned toolchain | **Done (documented).** Only CPython ≥ 3.7; no external packages. Recorded in `ukernel/README.md` → Building. |
| MIT licence | **Done.** `ukernel/LICENSE` + an `SPDX-License-Identifier: MIT` header on every source file. |
| Release manifest | **Done.** `core`, `version` (major.minor), `features`, `build_id`, sizes, entry offsets, patch sites. |
| Per-core identity | **Done.** `ukernel.py:CORE_NAME` names the target SGX core; build.py derives the binary name and manifest `core` from it, so SGX540/SGX531/… images never collide. |
| Feature-bit documentation | **Done.** `ukernel/README.md` → Feature bits (bit 0 CLEANUP_V2, bit 1 SECURE). |
| Build/feature README | **Done.** `ukernel/README.md`. |
| linux-firmware WHENCE entry | **Draft below** (not submitted). |
| Neutral docs pass | **Pending ta3d-state** — `docs/` and any Documentation text use CE5300/SGX5xx neutrally; dev-box history → `docs/log/`. |
| DPM out-of-memory handler | **Pending** — the one open functional gap before the firmware is feature-complete for general workloads (`side-session-notes/.../dpm-oom-handler-design.md`). Not a packaging item, but gates "ready". |

### Draft linux-firmware WHENCE entry (not submitted)

The binary linux-firmware would carry is the **packaged** image the DRM driver
requests (header + feature bits applied by the driver's packaging tool over the
`ukernel/build.py` output), named as the driver requests it:

```
Driver: powervr_sgx — PowerVR SGX5xx (e.g. SGX545 on the Intel Atom CE5300)

File: powervr/sgx545-openhc-ukernel.bin
Version: 1.0
Source: <source-repository-url>/ukernel   (ukernel.py + build.py; the DRM
        driver's packaging tool applies the header/feature bits)
License: MIT.  See LICENSE.<id> for details.
```

Open before this is real: the `powervr/` subdirectory name and the exact file
name are the DRM driver's to settle (it owns the `firmware=` request path); the
MIT text must be added to linux-firmware's licence set; `Source` needs the
public repository URL. The source and the built binary are versioned together
by the manifest `version` + `build_id`.

### Not a blocker, noted
- The microkernel source carries inline hardware-provenance comments (which
  behaviours were confirmed on which bring-up box). These are useful engineering
  history; a final upstream-source pass can thin them or move the box-specific
  ones to `docs/log/`, but they do not affect the binary and are low priority.
