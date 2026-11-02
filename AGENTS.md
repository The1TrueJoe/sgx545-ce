# Agent brief: sgx545-ce → a GPU that works on stock desktop distros

**Read this first if you are a session working on `The1TrueJoe/sgx545-ce` (branch `um-re`).**
It explains the end goal, the standards the work has to meet to get there, and the
work that remains, in order. It does not replace `docs/`. `docs/` says how the
hardware works; this says what we are building with that knowledge and what rules
that build has to follow.

---

## 1. The goal, in one paragraph

Old netbooks and nettops with **Intel Cedarview (GMA 3600/3650: Atom D2500/D2550/D2700/N2600/N2800)**
should get GPU acceleration on a **stock** modern distro (Debian, Ubuntu, Fedora,
Arch) with no custom image and no setup. The only way to get that is
**upstream**: a kernel DRM driver in Linux, the microkernel in `linux-firmware`,
and the GL driver in Mesa. Distros then pick it up on their own. The Control4
EA (CE5300/CE5310) stays the development and openHC target, but the
deliverable is upstream code that runs on Cedarview. We are **not** building a
distro. A DKMS module plus out-of-tree Mesa packages (apt/PPA/COPR/AUR) is an
acceptable *bridge* until things are merged, never the destination.

Secondary goal: climb from GLES2 to **GLES 3.0 / OpenGL 3.x**. The hardware was
marketed as DX10.1 / OpenGL 3.2, and Intel's Windows ICD shipped GL 3.0 on this
core.

---

## 2. Where the project is now (as of `um-re` 7d02f89, 2026-11-01)

| Piece | State | Upstreamable as-is? |
|---|---|---|
| `kernel/` (`pvrsrvkm`) | Intel's GPL DDK 1.7 ported to Linux 7.1, with an i386↔x86_64 compat bridge. Runs on the EA. | **No.** The services/bridge-ioctl uAPI will not be accepted (§3.1). Keep it as openHC's driver and the A/B rig. |
| `srvinit/` | Userspace GPU bring-up (replaces `libsrv_init`); `-o` loads our microkernel | **No.** Upstream, the *kernel* loads firmware and initialises the GPU. |
| `ukernel/` | Open microkernel in USSE assembly, validated offline: init, host-kick dispatch, TA kick, TA finished, 3D start, render done; register programming matches the DDK per phase. 3ce8f51 fixed the init path offline (bare-DOUTU pds0 + loader-patched limms). | The **key to the firmware problem** (§3.3). Not complete: render teardown, transfers, cleanup, power, multi-render bookkeeping, hardware validation. |
| `mesa/` | Gallium driver + NIR→USSE compiler, overlay on Mesa 26.2.5. EGL fbdev only, GLES2, no GBM. Pixel-exact vs the DDK in the A/B cases. Minecraft through gl4es at ~33.5 FPS vs the DDK's ~41. | **Compiler and most of the driver: yes, in spirit.** The winsys (`libpvr` over the services bridge) must be rewritten for a new uAPI. |
| `libpvr/` | Services client (bridge ioctls) shared by srvinit, winsys and tools. `abi/` is generated from the GPL kernel headers. | **No.** It disappears once the new uAPI exists. Nothing derived from `kernel/src` headers may go into Mesa (§3.5). |
| `docs/` | Hardware reference written from the RE: ISA, PDS, STATE, TEX, UKERNEL, SRVINIT, GEN | This is the *clean-room interface* (§3.6). Keep it authoritative. |

### Hardware facts that drive the design

- **CE5300/CE5310 (EA-1/EA-3):** the SGX545 is its own PCI function, `01:02.0 8086:089b`. The display (VDC, `01:08.0 8086:2e61`) is a separate function with **no upstream KMS driver**. Core rev **1014**.
- **Cedarview:** the SGX sits **behind the same PCI function/BAR as the display engine**, which upstream `gma500` already owns. gma500 keeps Cedar Trail support (Medfield and Moorestown were removed in 2021; Poulsbo, Oaktrail and Cedar Trail stay). Core rev **10131** (same errata as 1014, but the microkernel build may differ, so verify).
- **Clover Trail (Atom Z2760)** also has SGX545, but its display has no upstream support. Out of scope.

---

## 3. The standards the work must meet

### 3.1 Kernel DRM uAPI rules (non-negotiable)

From `Documentation/gpu/drm-uapi.rst`, "Open-Source Userspace Requirements":

- Any new DRM uAPI needs **open-source userspace that is reviewed and ready to merge into its canonical upstream project** (here, Mesa). It must be the real driver, handling errors and corner cases, not a test toy.
- A userspace-side reviewer should **Ack the kernel uAPI patch**.
- The kernel patch **must land in drm-next or drm-misc-next before** the userspace patches land. uAPI flows from the kernel.
- Mesa's `include/drm-uapi/` is only updated from drm-next (or a tagged release), with the source commit in the message. **We cannot ship a Mesa driver against an out-of-tree uAPI.**

uAPI design conventions (follow `include/uapi/drm/pvr_drm.h` as the house model):

- No pointers in structs. User pointers are `__u64`. All members type-aligned, structs padded to 64 bits, explicit padding, **identical layout on i386 and x86_64**. That makes the whole compat layer unnecessary.
- Every `flags`/`pad`/reserved field is checked to be zero, so it can be extended later.
- Arrays are passed as `{stride, count, __u64 ptr}` objects (pvr's `drm_pvr_obj_array`), so structs can grow.
- Render-node ioctls are marked `DRM_RENDER_ALLOW`. Nothing global or modeset-related goes on the render node.
- Header license: `(GPL-2.0-only WITH Linux-syscall-note) OR MIT`, so Mesa can carry it.
- Full kerneldoc on every struct and ioctl, plus `Documentation/gpu/<driver>/uapi.rst`.

### 3.2 What a modern DRM render driver is expected to use

Reviewers will expect the standard helpers, not hand-rolled equivalents:

| Need | Expected mechanism |
|---|---|
| Buffers | GEM, almost certainly `drm_gem_shmem`; dma-buf export/import (PRIME) |
| GPU virtual memory | Per-file VM contexts on **`drm_gpuvm`** (VM_MAP/UNMAP), `drm_exec`/`dma_resv` locking. SGX MMU: per-context page directory via the BIF (see UKERNEL.md, TA kick). |
| Scheduling | **`drm_sched`**, with job timeouts feeding hardware recovery (UKERNEL.md §5a) |
| Sync | `dma_fence` signalled from the completion IRQ; **syncobj, including timeline**, for in/out fences; implicit sync through `dma_resv` on shared BOs |
| Power | Runtime PM, mapped onto the microkernel power command (type 3) and APM |
| Debug | debugfs; **devcoredump** on hang |
| Firmware | `request_firmware()` from linux-firmware, with a version and ABI check |
| Tests | IGT tests for the uAPI (`tests/<driver>/`) are strongly expected |

`dma_fence` rules matter. Fences must signal in bounded time, with no memory allocation or blocking on other fences in the signalling path, and with `dma_fence_begin/end_signalling` annotations so lockdep checks it. A GPU that can hang must have a timeout and recovery path, or fences never signal.

**The model to copy is `drivers/gpu/drm/imagination` (the upstream Rogue driver).** It is firmware-mediated like ours. It uses drm_sched, the GPU VA manager, runtime PM, drm_gem_shmem and watchdog/device-loss handling. Userspace-supplied parts of firmware commands are passed as **"streams" whose layout the kernel validates field by field** (`pvr_stream.c`; formats in Mesa's `src/imagination/csbgen/rogue_kmd_stream.xml`). Its uAPI has DEV_QUERY (GPU info, quirks, heaps), CREATE_BO, VM contexts, VM_MAP/UNMAP, contexts, free lists, HWRT datasets and SUBMIT_JOBS. Ours should look much like a small SGX-flavoured version of it. It does **not** cover SGX, and its ISA, firmware and MMU are all different, so reuse its *shape*, not its code paths.

### 3.3 Isolation and security: the biggest design change from the DDK

In the DDK, userspace (`libsrv_um`) builds structures the microkernel trusts: render contexts, RT data, PB descriptors and sync objects, in shared heaps. `docs/GEN.md` ("Layout") records that **the microkernel reads the previous client's structures at the next TA**. Upstream will reject anything like that. The new kernel driver must:

- **Own every firmware-visible structure:** host control block, kernel CCB, HW render/transfer contexts, RT data, parameter buffers and free lists, and the sync/status words the microkernel writes.
- Let userspace submit **only** the client part of a command (e.g. the `SGXMKIF_CMDTA` client fields documented in STATE.md §1), as a validated stream, plus GPU VAs **inside its own VM**. The kernel patches in the addresses of kernel-owned structures.
- Make sure one client's commands can't make the microkernel read or write another client's memory. Both the per-context page directory and the kernel-owned structures enforce this.
- Treat every userspace-supplied address the microkernel will dereference as hostile. Range-check it against the client's VM and heaps.

Write a short **threat-model section** in the uAPI design doc. Reviewers will ask for it.

### 3.4 Firmware: linux-firmware rules

- linux-firmware only takes **redistributable** firmware with a license file and a `WHENCE` entry. Imagination's 53,096-byte microkernel can't go in, and can't go on any ISO, unless Imagination licenses it. (Joseph is asking them separately. Don't block on it.)
- **Our open microkernel is the primary plan.** To be submittable it needs:
  1. Hardware validation on **both core revs** (1014 EA-3 and 10131 Cedarview).
  2. The full command set the kernel driver uses: TA, 3D, render teardown with multi-render bookkeeping, transfer, cleanup, power, HWR re-init, get-misc-info.
  3. A permissive license (MIT/BSD), source in-tree, and a **reproducible build** from `ukernel/build.py` with no DDK inputs.
  4. A versioned host interface. The kernel checks the version at load.
- **Strategic decision (record it in `docs/` once made):** keep the open microkernel **SGXMKIF-compatible** until it's complete, so the DDK microkernel stays a drop-in for A/B testing. After that, decide whether to simplify the host interface for the new kernel driver. Changing it later means a firmware ABI bump.

### 3.5 Licensing boundaries

- `kernel/src/` is **GPL-2.0** (Imagination via Intel). Register and struct knowledge from it can be used in a **GPL kernel driver**.
- **Mesa is MIT.** Nothing copied or mechanically derived from `kernel/src/` goes into Mesa: no headers, tables or struct layouts. `libpvr/abi/` is in that category. Mesa-side hardware definitions must be our own, preferably as XML with generated packers, the way `src/imagination/csbgen` and genxml do it. `docs/` is the source for writing them.
- The uAPI header is dual-licensed (§3.1), so both sides can include it.
- **Never commit** DDK binaries, the Imagination microkernel, the Windows ICD, or decompiler output.

### 3.6 Clean-room provenance

Reviewers for reverse-engineered drivers (lima, etnaviv, asahi, panfrost) ask how the knowledge was obtained. Keep the existing discipline, which `ukernel/README.md` already states: implementation is written **from `docs/`** (behaviour, encodings, register meanings found by tracing and hardware tests like `tools/isa/hwt_*`), and is **never translated from proprietary bytes or decompiled code**. Proprietary drivers are *oracles* for A/B comparison and behaviour, not sources. When a doc fact came from static analysis of a blob, record it in the doc's provenance markers. Then confirm it by hardware experiment before code depends on it.

### 3.7 Mesa expectations

- **Location and name:** new gallium driver plus compiler. Talk to the `src/imagination` (Rogue) maintainers early about naming and placement. `src/imagination/sgx/…` versus `src/gallium/drivers/sgx545` is their call, and the ISA is unrelated to Rogue's. The DRM driver name (what `drmGetVersion` returns) decides the loader mapping. **Don't pick `pvr`**, which is taken.
- **Platforms:** with a proper render node and dma-buf, gallium gives you GBM, EGL Wayland/X11/surfaceless and DRI3 almost for free. The fbdev EGL platform in the overlay is openHC-only and won't go upstream (Mesa has no fbdev platform).
- **Render-only pairing (`kmsro`/`renderonly`):** on Cedarview, the display device is gma500 and the render device is ours. Mesa needs a kmsro entry so opening gma500 creates our screen. Scanout buffers are linear (verify gma500's stride and alignment rules).
- **Modifiers:** support `DRM_FORMAT_MOD_LINEAR` everywhere first. Twiddled/tiled layouts (TEX.md §1) would need modifiers in `drm_fourcc.h`. Check whether an Imagination vendor namespace exists before inventing one.
- **Sync:** implicit sync through dma_resv for X/DRI3 and old Wayland; explicit timeline syncobj for Wayland's `linux-drm-syncobj-v1`.
- **Testing culture:** dEQP via `deqp-runner`, with `*-fails.txt`/`*-flakes.txt`/`*-skips.txt` expectation files (Mesa CI convention), and piglit for desktop GL. A hardware CI runner (LAVA/ci-tron) is welcome but not required to merge. Khronos conformance submission is optional, but a clean dEQP-GLES2 run is the credibility bar.
- **Code standards:** Mesa style, meson options, no environment-variable-only features in default paths, and the `SGX545_*` debug variables moved to the driver's debug-option table.

---

## 4. What desktops need from the driver (so priorities are right)

| Consumer | Needs | Our status |
|---|---|---|
| Any Wayland compositor / Xorg modesetting | KMS device + GBM + dma-buf + render node + fences | KMS: gma500 (Cedarview). The rest is the new driver + winsys. |
| GNOME Mutter (Ubuntu 26.04 is **Wayland-only**) | GL ≥ 3.1 or **GLES ≥ 2.0** | GLES2 is enough |
| wlroots (sway, labwc), Weston | GLES2 renderer | GLES2 is enough |
| Xorg glamor | GLES2 + some extensions (verify the current list) | Likely GLES2 + extensions |
| GTK4 GPU renderers (most GNOME apps) | **GL 3.3 / GLES 3.0** or Vulkan; otherwise Cairo software | Needs GLES3 |
| Firefox WebRender (hardware) | GLES3-class | Needs GLES3; falls back to software WebRender |
| Qt 6 / LXQt | GLES 2.0 works | GLES2 is enough |
| Minecraft Java 1.17+ | **GL 3.2 core** | Needs 3.2 |
| Video decode | Cedarview VXD decoder: no open driver | Out of scope (note for users) |

---

## 5. The work, in order

Each milestone has an exit criterion. Don't start the next one's upstream-facing work until the previous criterion holds. Exploration ahead of that is fine.

### M0: finish GLES2 on the current stack (keeps openHC moving)
The `mesa/README.md` "What is missing" list: discard/alpha test (ISPA pass types 2/4), gl_FragCoord/gl_PointCoord, constant-colour blend (sop2 k18/k19), the rest of textures, scissor/partial viewports, lines/points, ZLS, tear-free present, zero-copy vertices and pipelining, extension pruning.
**Exit:** dEQP-GLES2 runs to completion on the EA3 with a fails list we understand. Run it on the DDK too for comparison.

### M1: open microkernel complete and on hardware
Finish what `ukernel/README.md` lists as not yet reproduced. Bring it up on the EA3 with `srvinit -o`, then run frames, multi-render, transfers, cleanup, power and HWR. Validate against the DDK with `run_ours.py` per phase.
**Exit:** the full M0 dEQP run passes as well with our microkernel as with the DDK's, with HWR recovery demonstrated. Reproducible build, license chosen.

### M2: uAPI design doc, then the new kernel driver (on CE5300 first)
CE5300 first, because the SGX is its own PCI device there and nothing needs untangling.
1. Write `docs/UAPI.md`: ioctls (DEV_QUERY with core rev/heaps/quirks; CREATE_BO; MMAP_OFFSET; VM context + VM_MAP/UNMAP; CREATE_CONTEXT render/transfer; parameter buffer/RT dataset objects if the kernel doesn't hide them; SUBMIT_JOBS with syncobj in/out and BO lists), stream formats for the client parts of TA/3D/transfer commands (from STATE.md), heap layout constraints (PDS/USSE code heaps and their addressing limits), the threat model (§3.3), and how HWR shows up to userspace (context guilty/innocent, device-lost).
2. A new driver under `drivers/gpu/drm/`: kernel-side init that replaces `srvinit` (firmware load, host control block, kernel CCB, event programs), MMU/BIF, GEM shmem, drm_gpuvm, drm_sched + IRQ completion, syncobj, runtime PM, devcoredump, debugfs.
3. IGT tests for each ioctl, including the negative cases (bad flags, VAs outside the VM, malformed streams).

Leave `pvrsrvkm` alone apart from openHC's needs. **Don't add more compat-layer work to it.**
**Exit:** the driver boots the GPU from the kernel on the EA3 and runs submitted jobs, IGT passes, and one client can't corrupt another (test it).

### M3: Mesa winsys on the new uAPI
Replace `libpvr` in `src/gallium/winsys/sgx545/` with a DRM winsys: BO allocation, VM_BIND, submit, syncobj, dma-buf import/export. Drop the fbdev platform from the upstream branch (keep it in the overlay for openHC). Enable surfaceless, GBM, Wayland and X11.
**Exit:** dEQP-GLES2 on the new kernel driver matches the M0 baseline. Weston or labwc runs on CE5300 through a KMS device. A minimal VDC KMS driver or simpledrm on the CEFDK framebuffer is fine for the EA. Lower priority than Cedarview.

### M4: Cedarview
Needs a Cedarview machine (open question §7).
1. **gma500 hosts the SGX:** gma500 creates an **auxiliary bus device** for the SGX, shares the BAR mapping, and dispatches the SGX interrupt to it. Work out on hardware how gma500's IRQ handler and the SGX status bits share the line. Our driver binds to the auxiliary device.
2. **Scanout path:** gma500 either imports our dma-bufs for scanout, or exports its dumb buffers for us to import (kmsro allocates on the KMS device). Check gma500's current PRIME support and add what's missing. Linear only.
3. **Mesa kmsro mapping** for gma500 → our driver.
4. Confirm the microkernel and errata for core rev 10131.

**Exit:** a stock Debian or Ubuntu install with our kernel driver (DKMS) and our Mesa build runs Weston, labwc, sway, Mutter and Xorg (modesetting+glamor) with GPU compositing on a Cedarview netbook. This is also the bridge release.

### M5: upstream submission
1. RFC series to **dri-devel** (driver + uAPI + docs + MAINTAINERS), with the Mesa MR open as a draft that points at it, and the gma500 auxiliary-device patches CC'd to gma500's maintainers. The target tree is probably drm-misc.
2. Firmware to linux-firmware (open microkernel binary, license, WHENCE entry, and a source pointer).
3. Iterate. Once the kernel lands in drm-misc-next/drm-next, sync `include/drm-uapi/` in Mesa and merge the Mesa driver.

**Exit:** merged in all three. Distros pick it up from there.

### M6–M8: the GL ladder (can overlap M3–M5 once the compiler work is separable)
- **M6, desktop GL 2.1 (compat):** occlusion queries (needed since GL 1.5), 3D textures (Windows exposed `EXT_texture3D`), shadow compare (or `nir_lower_tex` shadow lowering), clip planes, point sprites, polygon modes via st/mesa fallbacks. Gate: piglit `gpu` profile triaged.
- **M7, GLES 3.0 / GL 3.0:** integer ALU and bitwise ops, texelFetch/textureSize, integer and float textures, float render targets, MRT (Windows ICD: 8 colour attachments), sRGB, packed depth/stencil, transform feedback (Windows: 4 separate attribs × 4 components), texture arrays, MSAA, UBOs, instancing (PDS.md already shows an instance divisor), 8 clip distances (lower beyond the hardware's 6 planes), seamless cube maps (verify the hardware does them), ETC2 via Mesa's decompress-on-upload. Gate: dEQP-GLES3 triaged.
- **M8, GL 3.1 / 3.2 core:** TBOs, primitive restart (CPU fallback is fine), rect textures; **geometry shaders** (ISA.md notes libusc's geometry "emit and cut" bit), depth clamp, sync objects, multisample textures. Gate: piglit + GL CTS subsets.

**Oracles for M6–M8:**
- **Intel's Windows GL 3.0 ICD for this core** (`ig4icd32.dll` 8.14.10.2117; Wildfire Games' capability report lists its extensions and limits). Use it to learn what the hardware can do and what the encodings must achieve. Confirm everything with `hwt_*` hardware tests before code depends on it (§3.6).
- The DDK 1.7 libusc may already contain integer and geometry paths, since it's shared across APIs. Use it as an A/B oracle the way the GLES2 work does.
- Imagination's own claims: DX10.1, OpenGL 3.2, geometry shaders, a DX10 data assembler (vertex/primitive/instance IDs), render-target arrays.

Expectation to set in docs: the point of GL3 on this GPU is that apps start and run correctly. It won't be fast.

---

## 6. Rules of engagement for sessions on this repo

- Follow the repo's existing rules: wrap GPU runs on the shared EA3 in `flock /tmp/gpu.lock`, **the EA-1 is off limits**, never build Buildroot locally, and log hardware work in `docs/log/entries/`.
- Don't break openHC. `pvrsrvkm` + the overlay stay its shipping stack until the new driver replaces them.
- Don't put uAPI in Mesa before the kernel side has merged. Don't write 32-bit compat structs in new uAPI. Don't copy `kernel/src` material into Mesa. Don't commit proprietary binaries.
- Before code relies on any reverse-engineered fact, it goes into `docs/` with how it was established.
- When a decision closes off options (driver name, firmware host-interface changes, the uAPI shape), write it into `docs/` with the reasoning and check it with Joseph first.

---

## 7. Open questions (for Joseph, or for the session to resolve)

1. **Cedarview hardware:** which machine(s) do we have, with which CPU, panel and RAM? M4 can't start without one.
2. **Driver name** for the kernel module, DRM name and Mesa directory. Coordinate with Imagination's Rogue maintainers.
3. **Imagination:** will they license the SGX545 microkernel for linux-firmware, or bless the open one? Joseph is asking. Either answer is useful.
4. Does the open microkernel stay SGXMKIF-compatible forever, or get a simpler host interface for the new driver after M1?
5. The CE5300 display: a minimal VDC KMS driver, or simpledrm only? This only matters for openHC wanting Wayland.
6. The Cedarview IRQ topology and the BAR layout split between gma500 and SGX, for the auxiliary-device design.

---

## 8. References

- Linux DRM uAPI docs, "Open-Source Userspace Requirements" and render nodes: `Documentation/gpu/drm-uapi.rst` (https://docs.kernel.org/gpu/drm-uapi.html)
- Mesa `include/drm-uapi/README` (headers only from drm-next or tagged releases)
- Upstream Rogue driver series, the design model: https://lkml.indiana.edu/2311.2/07062.html ; uAPI `include/uapi/drm/pvr_drm.h` ; stream formats in Mesa `src/imagination/csbgen/rogue_kmd_stream.xml`
- gma500 keeps Cedar Trail (Medfield removal thread): https://lists.freedesktop.org/archives/dri-devel/2021-January/295823.html
- Imagination's SGX545 announcement (DX10.1, OpenGL 3.2, OpenCL 1.0): https://us.design-reuse.com/news/22405/graphics-ip-core-directx-opengl-opencl.html
- SGX545 Windows GL 3.0 capability report: https://feedback.wildfiregames.com/report/opengl/device/PowerVR%20SGX545
- In-repo: `README.md`, `kernel/README.md`, `mesa/README.md` (status, missing list, openHC plan), `ukernel/README.md`, `docs/UKERNEL.md`, `docs/SRVINIT.md`, `docs/GEN.md`, `docs/STATE.md`, `docs/TEX.md`, `docs/ISA.md`, `docs/PDS.md`, `docs/log/`
