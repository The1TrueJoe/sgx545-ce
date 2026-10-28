# mesa/: the SGX545 GL driver

An open EGL + OpenGL ES 2.0 driver for the PowerVR SGX545 of the Intel CE5300
(Control4 EA-1/EA-3). It is a Mesa gallium driver kept as an **overlay on a
pinned Mesa release**. The files here mirror Mesa's tree, so moving them into
Mesa later is a copy. It needs no Intel or Imagination userspace: frames
are built from `docs/STATE.md`, `docs/PDS.md` and `docs/ISA.md`, shaders by
our compiler, and the GPU is started by `srvinit/`.

Status (EA3, 2026-10-28; `docs/log/entries/2026-10-28-1020-mesa-driver.md`):

- `eglinfo` lists `PowerVR SGX545 (sgx545-ce)`, OpenGL ES 2.0, Mesa 26.2.5.
- Clear + triangle through GLES2 are byte-identical to the DDK.
- es2gears' shaders run on the GPU at ~400 FPS on fb0.

## Layout

| path | what |
|---|---|
| `VERSION` | the Mesa release overlaid: version, URL, sha256 |
| `build.sh` | fetch, check, patch, overlay, build (x86_64 and/or i386) |
| `patches/` | the hooks Mesa needs outside the overlay (meson options, loader, pipe-loader, EGL platform wiring); small, applied with `patch -p1` |
| `cross/` | the zig cc wrapper and the meson cross-file template |
| `src/gallium/drivers/sgx545/` | the gallium driver: screen, context, state, resources, draw/clear, shaders (`sgx545_program.c` is the USP finaliser) |
| `src/gallium/winsys/sgx545/drm/` | the winsys: `sgx545_frame.c` (`tools/gen`'s sgxgen as a multi-frame library: PB, render targets, PDS programs, TA state, VDM, kicks) over `libpvr/` (linked in by build.sh); `sgx545_frame_test.c` exercises it without Mesa |
| `src/sgx545/compiler/` | the NIR → USSE compiler (`sgx_compiler.h`); its tools and checks are in `tools/usc` |
| `src/egl/drivers/dri2/platform_fbdev.c` | EGL's `fbdev` platform: window = the framebuffer console |
| `tests/` | `sgx545_eglinfo`, `sgx545_es2gears` (es2gears' shaders), `sgx545_cpugears` (fixed programs only); `sgx545_texab.sh` and `sgx545_blendab.sh`, A/B against the DDK on the EA3 |

`build.sh` also links `kernel/src` into the tree as `include/sgx545-kernel` (the
bridge structures) and generates the fixed USSE programs from
`tools/gen/mkusse.py`.

## Building

```sh
mesa/build.sh x86_64 i386     # into $SGX545_MESA_BUILD (default build/mesa)/<arch>/install
```

You need zig (0.14+), meson, ninja, Python 3.10+ with mako and pyyaml, bison 3
and flex. The tarball and the libdrm wrap come from the network. Mesa is
configured with:

- `gallium-drivers=sgx545` and `platforms=fbdev` (EGL's native platform);
- GLES2 only (`opengl=false`, `gles1=disabled`, `glx=disabled`);
- no LLVM, no GBM, no xmlconfig/zlib/zstd/expat.

The targets are glibc 2.36 x86_64 (openHC's Buildroot userspace, glibc 2.42,
runs it) and glibc 2.36 i386 (the Debian bookworm root of openhc-minecraft).
Never build Buildroot locally; this build is Mesa only, and it fits in about
1 GB per target.

## Running

Copy `install/usr/lib` onto the box and point the loader at it:

```sh
LD_LIBRARY_PATH=/tmp/mesa/lib ./sgx545_eglinfo
LD_LIBRARY_PATH=/tmp/mesa/lib ./sgx545_es2gears -n 600
LD_LIBRARY_PATH=/tmp/mesa/lib PROBE_OUT=f.rgba ./probe       # tools/probe
```

The GPU must be up (`srvinit/`, or the DDK's sgxinit). On the shared EA3, wrap GPU runs in
`flock /tmp/gpu.lock`.

| variable | effect |
|---|---|
| `SGX545_DEBUG=frame,draw,nir,sync` | per-frame/draw/shader/allocation logging |
| `SGX545_TRACE=path` | a pvrtrace log of the client (every bridge call, memory before each kick); `tools/decode`, `tools/state` and `tools/pds` read it like a DDK capture |
| `SGX545_DUMP_USP=dir` | write each compiled USP (`tools/isa/usp.py`, `dis.py` read them) |
| `SGX545_NO_COMPILER=1\|vs\|fs` | use the fixed programs (gl_Position = attribute 0; gl_FragColor = a uniform or constant) for both or one stage |
| `SGX545_VERBOSE=1` | winsys logging (allocations, waits) |
| `FRAMEBUFFER=/dev/fbN` | the fbdev platform's framebuffer (default /dev/fb0) |

## How it works

- **Loading.** The loader maps PCI `8086:*` to `sgx545` when the kernel driver is
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
- **Shaders** (`sgx545_program.c`). Each shader is compiled on creation by
  `mesa/src/sgx545/compiler`. The driver reads the USP's lists and does what
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

This list is the next work, roughly in the order gl4es and Minecraft need it:

1. **Discard** (and alpha test through it): ISPA pass type 2/4 and the DDK's
   phase split around the feedback; refused today.
2. **gl_FragCoord and gl_PointCoord** as fragment inputs, and a saturated
   dot product: compiler gaps (reported to the compiler).
3. **Constant-colour blending** (`GL_CONSTANT_COLOR` and the rest). sop2 has
   two more selectors, k18/k19, that are not decoded yet; until then those
   draws come out opaque, with a message.
4. **Textures, the rest**: block 9, a GPU blit (the CPU one does mipmap
   levels and copies today), compressed formats, render-to-texture with a
   sampleable layout.
5. **Scissor and partial viewports**: pass-type-3 objects and region clips.
6. **Lines and points** (VDM primitive 1/2, point size output, set 0).
7. **Depth/stencil buffers in memory (ZLS).** Depth works within one render today,
   which covers a frame unless the rings fill mid-frame.
8. **Present without tearing.** Render into a back buffer and blit to fb0 with the
   transfer queue (`sgxtex_blit_*`), the way the DDK swaps, or flip a double-height
   fbdev.
9. **Performance**: zero-copy vertex buffers (the PDS fetching from device
   buffers), and pipelining frames instead of waiting for each render.
10. **F16 mediump** (`sgx_compile_options.f16`), for the DDK's exact pixels on
   mediump shaders.
11. Prune the extension list. st/mesa advertises some extensions from defaults
   (S3TC, depth textures) that the driver can't do yet.

## The gl4es ctest suite on the EA3 (milestone d)

gl4es' `tests/` are apitrace traces with reference images (`traces/`, `refs/`,
the list in its `CMakeLists.txt`). `tests/gl4es-ctest-ab.sh` replays each one
on the EA3, through the runtime's gl4es (unmodified), once on the DDK and once
on ours, and snapshots the call ctest checks; `tests/ctest_cmp.py` compares the
two snapshots pixel for pixel and each with gl4es' reference as `tests/test.sh`
does (`compare -metric AE -fuzz 20%` over its region, under its tolerance).

The rig (the EA1's copy of it is off limits):

- The openhc-minecraft runtime (`runtime-i386.tar.xz` from that repo's CI) in
  an ext2 image on the EA3's USB disk (`/media/DISKTEST/mcroot-ext4.img`,
  loop-mounted at `/opt/minecraft`; the root filesystem has 250 MB free). The
  DDK goes to `/usr/lib/sgx` as `mc-session` puts it there, our i386 build to
  `/usr/lib/sgx545`, apitrace's `glretrace` from Debian's `apitrace` package
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
  stream places it. Byte-identical to the DDK (`sgx545_texab.sh`).
- **The PDS middle word** declares the fragment program's primary
  attributes (b0-15, units of 4 dwords) and 12 / units in b25-28, as the DDK
  writes it: a fixed word for one unit overflowed and locked the GPU up as
  soon as a program iterated two F32 vec4 varyings.
- **Vertices per USSE task** are 128 / output dwords, at most 16 (the DDK's
  numbers for 4 to 24 output dwords).

## Plan: Minecraft on our driver (milestone e)

1. The i386 build drops into the chroot as `/usr/lib/sgx545/` (libEGL.so.1,
   libGLESv2.so.2, libgallium, libdrm). It is self-contained apart from glibc.
2. `runtime/bin/gl4es-run` switches with the same two variables:
   - `LIBGL_EGL` / `LIBGL_GLES` → `/usr/lib/sgx545`;
   - drop `LD_PRELOAD=sgx-busidshim.so`, which only the DDK needs;
   - `LIBGL_DEEPBIND` can return to gl4es' default.

   An A/B session runs the game on the EA3 once per stack, compares frames of
   the same saved world and camera, and measures FPS against the DDK's
   ~40-55.
3. The i386 process talks to the x86_64 kernel through the compat bridge, as
   the DDK did; `libpvr` builds for i386 (`MAP_32BIT` is x86_64 only).
4. Release images ship ours; the DDK stays installable for A/B only.

## openHC integration (sketch, not built)

openHC builds the kernel driver from the sgx545-ce submodule. Userspace would
come the same way:

- **A Buildroot package `sgx545-mesa`** in openHC's `package/`. It points at the
  sgx545-ce submodule and does what `build.sh` does with Buildroot's own
  toolchain (no zig):
  - extract Buildroot's `mesa3d` source, or fetch `mesa/VERSION`'s tarball
    itself, and check the sha256;
  - apply `mesa/patches`, copy the `mesa/src` overlay in, generate
    `sgx545_usse_progs.h` with `tools/gen/mkusse.py` (host python), and link
    `kernel/src` and `libpvr`;
  - configure with the options in `build.sh` through
    `$(eval $(meson-package))`.

  It installs libEGL, libGLESv2 and libgallium, provides `libegl` and
  `libgles`, and conflicts with `BR2_PACKAGE_MESA3D`. Pinning one Mesa per
  sgx545-ce commit keeps the overlay and the release matched. Buildroot's
  mesa3d package can't take a third-party gallium driver without patching
  its `Config.in` and meson option list, so a separate package is simpler
  than an overlay of mesa3d.
- **openHC features.**
  - EA images select `sgx545-mesa`.
  - The DDK moves behind an opt-in `ddk-ab` feature that installs the i386 blobs
    under `/opt/sgx-i386` for comparison runs, and no longer in release images.
  - `S95ohc-display` brings the GPU up with `srvinit` instead of the DDK's
    `sgxinit`.
  - The on-panel webview (today WPE on the DDK's EGL through libwpe) moves to
    EGL fbdev on our driver first. WPE WebKit's newer platforms need EGL 1.5,
    GBM and dma-buf (`EGL_EXT_image_dma_buf_import`, `EGL_MESA_platform_gbm`),
    which are next for the winsys: dma-buf export of render targets through
    the services driver, or a GBM backend over `resource_create`.
- **CI.** The package builds in the GitHub Actions EA image jobs, like everything
  else in openHC. The overlay adds one Mesa build (about 10 minutes) to the EA
  images.
