# mesa/: the SGX545 GL driver

An open EGL + OpenGL ES 2.0 driver for the PowerVR SGX545 of the Intel CE5300
(Control4 EA-1/EA-3). It is a Mesa gallium driver kept as an **overlay on a
pinned Mesa release**. The files here mirror Mesa's tree, so moving them into
Mesa later is a copy. It needs no Intel or Imagination userspace: frames
are built from `docs/STATE.md`, `docs/PDS.md` and `docs/ISA.md`, shaders by
our compiler, and the GPU is started by `srvinit/`.

Status (EA3, 2026-10-28; `docs/log/entries/2026-10-28-1020-mesa-driver.md`):

- `eglinfo` lists `PowerVR SGX545`, OpenGL ES 2.0, Mesa 26.2.5.
- Clear + triangle through GLES2 are byte-identical to the DDK.
- es2gears' shaders run on the GPU at ~400 FPS on fb0.

## Layout

| path | what |
|---|---|
| `VERSION` | the Mesa release overlaid: version, URL, sha256 |
| `build.sh` | fetch, check, patch, overlay, build (x86_64 and/or i386) |
| `patches/` | the hooks Mesa needs outside the overlay (meson options, loader, pipe-loader, EGL platform wiring); small, applied with `patch -p1` |
| `cross/` | the zig cc wrapper and the meson cross-file template |
| `src/gallium/drivers/sgx/` | the gallium driver: screen, context, state, resources, draw/clear, shaders (`sgx_program.c` is the USP finaliser) |
| `src/gallium/winsys/sgx/drm/` | the winsys: `sgxws.h` (all the driver sees of it); `sgx_frame.c` (`tools/gen`'s sgxgen as a multi-frame library: rings, PDS programs, TA state, VDM) over a kernel backend, `sgx_powervr_sgx.c` (the powervr_sgx DRM driver) or `sgx_pvr.c` (pvrsrvkm through `libpvr/`, linked in by build.sh); `sgx_frame_test.c` exercises it without Mesa. `docs/WINSYS.md` says which runs when |
| `src/sgx/compiler/` | the NIR → USSE compiler (`sgx_compiler.h`); its tools and checks are in `tools/usc` |
| `src/egl/drivers/dri2/platform_fbdev.c` | EGL's `fbdev` platform: window = the framebuffer console |
| `tests/` | `sgx_eglinfo`, `sgx_es2gears` (es2gears' shaders), `sgx_cpugears` (fixed programs only); `sgx_texab.sh` and `sgx_blendab.sh`, A/B against the DDK on the EA3 |

`build.sh` also links `kernel/src` into the tree as `include/sgx-kernel` (the
bridge structures). The fixed USSE programs are assembly text in
`src/sgx/compiler/sgx_fixed.c`, assembled when a context is created.

## Licensing boundary (AGENTS.md section 3.5)

Mesa is MIT: nothing copied or mechanically derived from `kernel/src` may
reach the code that goes upstream.  Where the line runs today:

| part | kernel/src or libpvr? |
|---|---|
| `src/sgx/compiler/` | **No.** Includes only Mesa and its own headers. |
| `src/gallium/drivers/sgx/` | **No.** It sees the winsys only through `sgxws.h` (GL-level state, the hardware's words as `docs/` describes them, opaque device/context/RT/buffer handles). Its meson dependency (`idep_sgx_ws`) carries no kernel include path and no kernel defines, so an include of anything services-side fails to build. |
| `src/gallium/winsys/sgx/drm/` (`sgx_frame.c`, `sgx_powervr_sgx.c`) | **No.** The frame recorder and the powervr_sgx backend include the dual-licensed uAPI header (`include/drm-uapi/powervr_sgx_drm.h`), nothing else. |
| `src/gallium/winsys/sgx/drm/` (`sgx_pvr.c`, `libpvr/`) | **Yes, by design, overlay only.** Built only where build.sh links libpvr in, against the kernel's bridge headers (`include/sgx-kernel`, the `-DSERVICES4`... defines), into objects of their own. It lays out structures the microkernel reads (the TA command, render details, RT data, sync lists) by their offsets. |
| `src/egl/drivers/dri2/platform_fbdev.c` | No kernel material; not for upstream (Mesa has no fbdev platform). |

## Building

```sh
mesa/build.sh x86_64 i386     # into $SGX_MESA_BUILD (default build/mesa)/<arch>/install
```

You need zig (0.14+), meson, ninja, Python 3.10+ with mako and pyyaml, bison 3
and flex. The tarball and the libdrm wrap come from the network. Mesa is
configured with:

- `gallium-drivers=sgx` and `platforms=fbdev` (EGL's native platform);
- GLES2 only (`opengl=false`, `gles1=disabled`, `glx=disabled`);
- no LLVM, no GBM, no xmlconfig/zlib/zstd/expat.

The targets are glibc 2.36 x86_64 (newer glibc userspaces, 2.42, run it) and
glibc 2.36 i386 (a Debian bookworm root).
Never build Buildroot locally; this build is Mesa only, and it fits in about
1 GB per target.

## Running

Copy `install/usr/lib` onto the box and point the loader at it:

```sh
LD_LIBRARY_PATH=/tmp/mesa/lib ./sgx_eglinfo
LD_LIBRARY_PATH=/tmp/mesa/lib ./sgx_es2gears -n 600
LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=f.rgba ./probe       # tools/probe
```

The GPU must be up (`srvinit/`, or the DDK's sgxinit). On the shared EA3, wrap GPU runs in
`flock /tmp/gpu.lock`.

Debugging options follow Mesa's convention: flags in `SGX_DEBUG`
(`SGX_DEBUG=help` lists them), read once at screen creation; nothing in
the default path depends on the environment.

| variable | effect |
|---|---|
| `SGX_DEBUG=frame,draw,nir,sync` | per-frame/draw/shader/allocation logging |
| `SGX_DEBUG=stats` | where the time goes, every 2 s: FPS, flushes by cause, vertex cache, the winsys's waits and render latency |
| `SGX_DEBUG=serial` | wait for every render after submitting it (GPU time per render in `stats`) |
| `SGX_DEBUG=hangabort,flushall,flushrt` | abort at a render that does not finish; no deferred flushes; SGX_FLUSH_HW_RENDER_TARGET at a target's destruction |
| `SGX_DEBUG=novcache,nof16,noffmediump,nof16samples,nophasesplit` | turn off the vertex cache, F16/C10 fragment arithmetic, mediump fixed-function programs, F16 texel returns, the split of discarding programs |
| `SGX_DEBUG=nozerocopy,nozls,nolazyscanout,nopdsmerge` | gather vertex inputs instead of DMAing them in place; no depth/stencil across renders; scanout buffers on the display device from the start; samples merged only into their own set's iteration |
| `SGX_DUMP_CODE=dir` | write each compiled program as `vs-N.code` / `fs-N.code`, one "hi lo" hex pair per instruction (`tools/isa/dis.py -x` reads the pairs). The compiler no longer writes USPs; sgxc does, for the DDK A/B harness |
| `SGX_TRACE=path`, `SGX_TRACE_KICKS=n` | a pvrtrace log of the client (every bridge call, memory before each kick, for the first n kicks); `tools/decode`, `tools/state` and `tools/pds` read it like a DDK capture |
| `SGX_SLOTS=n` | frames in flight, 2 to 4 (default 4); fewer lower other clients' latency behind a game |
| `SGX_DRY_RUN=n`, `SGX_FRAME_GAP_US=us` | frames from the n-th on built but never sent; idle time after each render (hang bisecting) |
| `FRAMEBUFFER=/dev/fbN` | the fbdev platform's framebuffer (default /dev/fb0) |

## How it works

- **Loading.** The loader maps PCI `8086:*` to `sgx` when the kernel driver is
  `pvrsrvkm` (a predicate like iris's). The DRM descriptor creates the screen on
  the DRM node; the screen owns the services connection (`libpvr`), one per
  process.
- **The fbdev platform.**
  - A window is fb0 itself, wrapped as device memory
    (`resource_from_user_memory`). The PBE renders into the scanout buffer, and
    `eglSwapBuffers` waits for the render: single-buffered, no copy.
  - Pbuffers are as on the surfaceless platform.
  - Any native window handle is accepted, NULL too.
  - The platform enumerates primary DRM nodes itself, because EGL's device list
    only takes devices with a render node.
- **Frames.**
  - A frame is recorded into per-context rings (PDS, USSE, vertices, VDM) and
    submitted with one TA kick, first and last in scene, at flush, swap,
    readback, or a ring filling up. The driver waits for every render, so
    fences are always signalled.
  - Per frame the client writes what the DDK writes: the RT data frame counter,
    render details +0xdc and the re-armed sync words. Without those the 4th
    frame hangs (the log entry tells how this was found).
  - Kernel-data and sync allocations follow the DDK's order exactly, because the
    microkernel reads the previous client's structures at the next TA
    (`docs/GEN.md`, "Layout").
- **Draws.** Vertices are gathered on the CPU: indices resolved, each attribute
  as four dwords with GL's defaults. They are copied into the frame and drawn
  through an identity index list. Triangle lists and strips are drawn as they
  are; fans become lists.
- **Shaders** (`sgx_program.c`). Each shader is compiled on creation by
  `mesa/src/sgx/compiler`. The driver reads the USP's lists and does what
  the DDK's GL driver did:
  - a vertex program becomes prologue phase, shader + `emit`, epilogue phase;
  - a fragment program becomes `phas`, shader, `mov o0, pa<colour> [end mask=1]`;
    with blending, the last instruction is instead a `sop2` that blends the
    colour into o0. In a translucent pass (ISPA pass type 1), o0 starts as the
    tile's colour. Every GL factor but the constant ones maps onto sop2's
    selectors. Reverse subtract trades the sources (`sop2 o0, o0, pa<c>`).
    A colour mask appends `sopwm o0, pa<c>, pa<c> [end mask=…]`, one bit per
    byte of the packed BGRA8. Each fragment shader keeps a program per blend
    and colour-mask state it has drawn with.
  - constants go into secondary attributes per list C, DMA'd in 16-dword chunks;
  - each varying is iterated with a DOUTI. A varying's set is its location -
    VARYING_SLOT_VAR0 on both sides, so stages compiled apart agree.

## What is missing

This list is the next work, roughly in the order dEQP-GLES2 needs it (the fails
lists in `src/gallium/drivers/sgx/ci/` say what each costs):

1. **Depth/stencil buffers in memory (ZLS).** Depth and stencil work within one
   render, which covers a frame unless the rings fill mid-frame or the target
   changes. Stores work (TEX.md section 7); nothing has made a render load one yet.
2. **Textures, the rest**: 4444, 5551 and 565 as native formats (st uses RGBA8
   today, and T1 0x05 hands the compiler's unpack R and B swapped), LOD limits,
   border colours. Compressed formats go through Mesa's CPU fallback.
3. **Present without tearing.** Render into a back buffer and blit to fb0 with the
   transfer queue, the way the DDK swaps, or flip a double-height fbdev.
4. **Performance**: zero-copy vertex buffers (the PDS fetching from device
   buffers), and pipelining frames instead of waiting for each render.
5. **Prune the extension list.** st/mesa advertises from defaults some that the
   driver can't do yet: GL_OES_depth_texture, GL_EXT_shadow_samplers,
   GL_EXT_frag_depth, GL_EXT_texture_border_clamp, GL_OES_EGL_image_external and
   the draw-buffers family. It takes a Mesa-side way to say no; there is no pipe
   cap for most of them.

## dEQP-GLES2 (milestone M0's exit test)

dEQP-GLES2 runs the way Mesa's CI runs it, so its results and expectation
files carry over upstream:
- `mesa/tests/deqp/build-deqp.sh [x86_64|i386]` builds `deqp-gles2` from the
  VK-GL-CTS release that the Mesa release in `mesa/VERSION` pins, with the
  same backports and patches (read from that release's
  `.gitlab-ci/container/build-deqp.sh`). It uses dEQP's `surfaceless` target
  and cross-compiles with zig against our Mesa. It also builds `deqp-runner`
  at the CI's version.
  - Sources and the build live in `~/.cache/sgx545/deqp` (a few GB).
  - The result is `out/`, which gets copied to the box.
  - x86_64 is ours; i386 is for the DDK.
- On the box, `run-deqp.sh sgx|ddk` (from that `out/`) runs the GLES2
  mustpass list under `deqp-runner`:
  - pbuffers, 256×256, config `rgba8888d24s0ms0` (no stencil yet);
  - one job (two render contexts on this GPU have locked it up);
  - the whole run holds `/tmp/gpu.lock`.
  - Ours runs on the surfaceless EGL platform (`EGL_PLATFORM=surfaceless`,
    which `mesa/patches/0003` lets find pvrsrvkm's primary node), with
    `SGX_DEBUG=hangabort`, so a hang is one failure, not a wedged run.
  - The DDK runs the i386 binary through its own loader and the bus-id shim:
    its default EGL display offers pbuffers too.
- Expectations are in `src/gallium/drivers/sgx/ci/`, in Mesa CI's
  format: `sgx-fails.txt`, `-flakes.txt` and `-skips.txt` for ours, and
  `ddk-*` for the DDK. Every entry gets a reason. Skips are only for tests
  that hang the GPU.

## The gl4es ctest suite on the EA3 (milestone d)

gl4es' `tests/` are apitrace traces with reference images (`traces/`, `refs/`,
the list in its `CMakeLists.txt`). `tests/gl4es-ctest-ab.sh` replays each one
on the EA3, through the runtime's gl4es (unmodified), once on the DDK and once
on ours, and snapshots the call ctest checks; `tests/ctest_cmp.py` compares the
two snapshots pixel for pixel and each with gl4es' reference as `tests/test.sh`
does (`compare -metric AE -fuzz 20%` over its region, under its tolerance).

The rig (the EA1's copy of it is off limits):

- An i386 Minecraft runtime (`runtime-i386.tar.xz`) in
  an ext2 image on the EA3's USB disk (`/media/DISKTEST/mcroot-ext4.img`,
  loop-mounted at `/opt/minecraft`; the root filesystem has 250 MB free). The
  DDK goes to `/usr/lib/sgx` as `mc-session` puts it there, our i386 build to
  `/usr/lib/sgx`, apitrace's `glretrace` from Debian's `apitrace` package
  (unpacked: the runtime's apt can't install it), and the traces to `/ctest`.
- Each replay: Xvfb `:1`, `glretrace --singlethread` with
  `LIBGL_FB=3` (pbuffer) and `LIBGL_EGL`/`LIBGL_GLES` naming one stack.
- `LIBGL_NOTEST=1` for both stacks. gl4es' hardware probe initialises EGL,
  terminates it and initialises it again; the DDK can't create a render target
  after that (a NULL dereference in `SGXAddRenderTarget`), so without it the DDK
  replays nothing.
- `tests/gpuwatch.sh` runs every replay under the GPU lock and kills it at its
  first SGX recovery. On this box one lockup, the DDK's or ours, often leaves
  the microkernel unable to re-initialise ("SGXInitialise failed (24)"), so a
  suite run checks the GPU before every replay and power-cycles the EA3 when
  the reference scene no longer renders.
- The GPU is brought up by the DDK's `sgxinit` (as `S95ohc-display` does at
  boot). On a GPU that srvinit started, a process's render-context cleanup
  never completes, and the next client's TA reads the dead client's render
  target data.

What the first runs found, all fixed in the driver:

- **One render context per process.** Mesa creates two pipe contexts under
  glretrace, and gl4es initialises EGL twice. A second render context with its
  own parameter buffer made renders take 100-900 ms in a period-4 pattern and
  then locked the 3D; the screen now owns the one render context, and keeps it
  (with the services connection) across `eglTerminate`, as the DDK keeps its
  per-process parameter buffer.
- **Discard.** A fragment shader that discards needs its own ISP pass type
  and phase split; drawn opaque, its first discarded pixel locks the GPU up
  (gl4es' clip planes in neverball). Such shaders are refused until that is
  done.
- **Varyings.** A draw whose fragment program iterates a set the vertex
  program doesn't write is refused (it locks the GPU up).
- **Dependent texture samples** (gl4es' projected lookups, swizzled or
  biased coordinates): the USP's sample block carries an encoded `smp`
  template; the driver puts it, with the sampler's four DOUTT words in SA
  after the program's own, and a `wdf` into the flat program where the block
  stream places it. Byte-identical to the DDK (`sgx_texab.sh`).
- **The PDS middle word** declares the fragment program's primary
  attributes (b0-15, units of 4 dwords) and 12 / units in b25-28, as the DDK
  writes it: a fixed word for one unit overflowed and locked the GPU up as
  soon as a program iterated two F32 vec4 varyings.
- **Vertices per USSE task** are 128 / output dwords, at most 16 (the DDK's
  numbers for 4 to 24 output dwords).

## Plan: Minecraft on our driver (milestone e)

1. The i386 build drops into the chroot as `/usr/lib/sgx/` (libEGL.so.1,
   libGLESv2.so.2, libgallium, libdrm). It is self-contained apart from glibc.
2. `runtime/bin/gl4es-run` switches with the same two variables:
   - `LIBGL_EGL` / `LIBGL_GLES` → `/usr/lib/sgx`;
   - drop `LD_PRELOAD=sgx-busidshim.so`, which only the DDK needs;
   - `LIBGL_DEEPBIND` can return to gl4es' default.

   An A/B session runs the game on the EA3 once per stack, compares frames of
   the same saved world and camera, and measures FPS against the DDK's
   ~40-55.
3. The i386 process talks to the x86_64 kernel through the compat bridge, as
   the DDK did; `libpvr` builds for i386 (`MAP_32BIT` is x86_64 only).
4. Release images ship ours; the DDK stays installable for A/B only.
