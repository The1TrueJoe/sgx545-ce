# piglit on the EA: desktop GL for M6

The desktop GL milestone (AGENTS.md, M6) is gated on piglit's `gpu` profile,
triaged the way dEQP-GLES2 was. This page is how piglit is built and run on an
EA, and what was learned setting it up.

## The stack

The EA has no X, so desktop GL goes through EGL on the surfaceless platform,
with libglvnd as the GL library:

| piece | what | built by |
|---|---|---|
| libglvnd 1.7.0 | `libOpenGL.so.0`, `libEGL.so.1` (the dispatcher), `libGLdispatch.so.0` | `mesa/tests/piglit/build-piglit.sh` |
| our Mesa, desktop GL | `libEGL_mesa.so.0` + `libgallium`, a second build: `mesa/build.sh x86_64-gl` (`-Dopengl=true -Dglvnd=enabled`). The shipping build (`x86_64`, GLES only, no glvnd) is unchanged | `mesa/build.sh` |
| waffle 1.8.1 | piglit's window-system layer; its `surfaceless_egl` platform | `build-piglit.sh` |
| piglit | Mesa CI's revision and patch (`.gitlab-ci/container/build-piglit.sh` of the release in `mesa/VERSION`), GL tests only (no X11, GBM, Wayland, Vulkan, EGL or GLES tests) | `build-piglit.sh` |
| piglit-runner | deqp-runner's sibling, from the same checkout | `build-piglit.sh` |

Everything runs from `/root/piglit` on the box; nothing is installed into its
root file system. glvnd finds our Mesa through
`__EGL_VENDOR_LIBRARY_FILENAMES` pointing at a vendor file with an absolute
path.

## The gate

`gpu` is a meta-profile (`tests/gpu.meta.xml`): `quick_gl` (7,909 tests) and
`quick_shader` (18,018), 25,927 in all. `mesa/tests/deqp/drive-deqp.py piglit`
runs it a chunk at a time, as it does dEQP: `piglit-runner` takes no caselist,
so each chunk goes to it as anchored `--include-tests` regexes.

On a box other sessions share, `mesa/tests/piglit/slices.sh NAME` runs it in
slices of about 30 minutes, each holding `/tmp/gpu.lock` throughout and starting
only on the box's normal boot with no SGX recoveries but its own; the payload
copy (no GPU work) goes first, in pieces marked done on the box, so another
session's reboot only costs the pieces in flight.

## Build and setup traps

- **waffle 1.8.1 doesn't build against glibc 2.34+** (its C11 `once_flag`
  clashes with glibc's): master's two threads fixes are cherry-picked onto it.
  Two more waffle bugs in a GLES- and X-less build are patched in the script: a
  disabled Wayland's `dependency('egl')` replaces the one the surfaceless
  platform found, and `sl_platform.h` includes `gbm.h` without using it.
- **waffle opens `libGL.so.1`** for desktop GL symbols. glvnd without GLX has
  no `libGL`; `libOpenGL.so.0` exports the same entry points, so the payload
  links `libGL.so.1` to it (and CMake's FindOpenGL is given the same link).
- **Unloading Mesa crashes the process at exit.** At teardown waffle
  `dlclose`s libEGL, which unloads `libEGL_mesa` and `libgallium`; the process
  then jumps into unmapped code during exit (`wflinfo` segfaults after doing
  its work, with its output still unflushed). Why is not established; Mesa's
  libraries are not built `-z nodelete`, and the toolchain is zig's. Keeping
  `libEGL_mesa` loaded with `LD_PRELOAD` avoids it, and the runs do that.
- **piglit needs `PIGLIT_NO_WINDOW=1`** on the surfaceless platform: without
  it a test with `-auto` waits in a window event loop that returns at once, and
  piglit calls `abort()` with nothing printed.
- **piglit's EGL support comes with its EGL tests** (`PIGLIT_BUILD_EGL_TESTS`
  defines `PIGLIT_HAS_EGL`, which the surfaceless platform needs), and
  `tests/egl`'s programs need X11: the build turns the EGL tests on and skips
  that one directory.
- **A surfaceless display has no window configs**: a client must ask for
  `EGL_PBUFFER_BIT` (or `EGL_SURFACE_TYPE` 0), as dEQP and waffle do.

## First light (EA1, 2026-11-02)

With the shipping caps, a desktop context reports `OpenGL version string: 2.0
Mesa 26.2.5` on `PowerVR SGX545 (sgx545-ce)`. Mesa grants 2.1 for
`EXT_texture_sRGB` on top of 2.0, which needs an sRGB format the driver can
sample.

## Uniforms past sa127 (EA1, 2026-11-03)

`glsl-uniform-interstage-limits` (118 vertex vec4, 126 fragment vec4, each
array read as `u[i]` in a loop over a uniform count) first locked the 3D
because the constants ran past sa127. With constants in memory (the compiler
keeps the most-used in SA and loads the rest with `ld.a.d` from a per-draw block
in the general ring, `sgxws_const_mem`), the spilled programs are right:

| shader_test | result |
|---|---|
| VS sums 40 / 118 vec4, FS sums 40 / 120 vec4 (static indices) | right, no recoveries |
| one uniform at a time through VS 118 vec4, and combinations | right |
| VS `u[i]` loop over 118, FS `u[i]` loop over 126 (small rect) | right |

Without `indirect_const_addr`, the test still recovered, and that was run time,
not a wrong program. GLSL lowers `u[i]` to a select over the whole array, which
leaves every element as a static use. The vertex program is then 1,321
instructions with 360 `ld.a.d`+`wdf` per iteration. At 126 iterations per
pixel, a 25x25 px rect sometimes takes one recovery (TA lockup, no BIF fault),
and a 62x62 rect takes two and draws nothing.

The fix is indexed loads, and it passes on the EA1 with no recoveries. An
indexed array that doesn't fit SA is read from the block: the compiler clamps
the index, adds it to the base with `imae` and loads each component with an
immediate offset (36ad6fc). The driver sets `indirect_const_addr` (1836bfc).
The test passes in all three runs (default, `--subdivide 5`, `--subdivide 5
--statechanges`, 25 cases each), and a full-screen 126-iteration loop takes
under a second.

## Results (EA3, 2026-11-03)

The whole `gpu` profile ran on the EA3 (a RAM boot of the #124 image), in
slices, with 0 wedges. Every test that didn't skip (3,700) was then rerun on
one build: um-re c111b12 with the varyings driver below (lib-pg17), 0
recoveries. Of the 25,805 top-level tests:

| result | tests |
|---|---|
| Pass | 3,372 |
| Skip | 21,668 |
| Fail | 237 |
| Crash | 87 |
| Missing | 437 |
| Timeout | 2 |
| Warn | 2 |

"Missing" tests are the GLES, EGL and window-system programs, which aren't
built.

The varyings work moved 153 tests from fail to pass. It gives the desktop
built-ins a set each: the back colors, which st's two-sided color reads in
the fragment shader, and gl_TexCoord[7]. Sets are handed out in slot order and
packed past ten. A vertex shader that writes slots its fragment shader doesn't
read is compiled again to that shader's sets. The compiler's side
(varyings sharing sets, derivatives, fragment depth) came in the same build.
One test went the other way: `gl-1.4-polygon-offset`, which reads depth back,
had passed by chance.

**NPOT 2D textures sampled through their mipmaps lock the 3D.** This showed
once coordinates were right: `arb_texture_compression@fbo-generatemipmap-formats`
NPOT took 12 recoveries (lockup_3d), each NPOT subtest a few. An NPOT
texture is linear (layout 3), and the DDK's GLES2 has no NPOT mipmaps, so no
linear mip chain is known. Such a texture is now sampled from its base level
only, and the test passes with no recoveries.

The expectations are `ci/sgx-piglit-fails.txt`, written by `mkexpect.py` from
the runs, with the reasons in `mesa/tests/piglit/sgx-reasons.tsv`. Every
failure is grouped under a reason. Where the cause is known it is stated;
otherwise the reason gives the symptom and says "not diagnosed".

Most failures come from these:

- **Flat shading is not implemented** (1,311 entries, nearly all `clipflat`
  subtests). The driver ignores the rasterizer's `flatshade` and flat
  varyings. The hardware's shade-mode state is not known; a capture of the
  DDK's GLES1 `glShadeModel(GL_FLAT)` against `GL_SMOOTH` would give it.
- **gl_FragDepth isn't written** (19). The compiler leaves the value in a PA
  register at the feedback nop for a `depthf`, but that instruction's form for
  a written depth isn't traced. The driver warns once and the triangle's own
  depth is used.
- **Derivatives compile but give wrong values** (8).
- **No depth format can be sampled** (96). `sgx_tex_format` has only the
  five 8-bit T1 codes, and Mesa always exposes `ARB_depth_texture`, so a
  `GL_DEPTH_COMPONENT` texture gets no format and the test crashes later. The
  DDK's GLES2 lists `OES_depth_texture`, `OES_packed_depth_stencil`,
  `OES_texture_float` and `OES_texture_half_float`, so a DDK capture gives
  the T1 codes. `tools/probe` uploads those formats (`PROBE_TEXFMT` depth16,
  depth32, d24s8, rgbaf32, lf32, rgbaf16, lf16).
- **No depth/stencil load or store (no ZLS).** Depth and stencil exist only
  within one render, so reading, copying or blitting them fails (41).
- **Texture formats, wrap modes and mipmaps** (49, 74, 13):
  - Only five sampler formats.
  - CLAMP and CLAMP_TO_BORDER are drawn as CLAMP_TO_EDGE.
  - No LOD clamps, base level or LOD bias.
  - No 3D textures (35).
- **M6 features not there yet:** user clip planes, polygon modes and edge
  flags, line stipple, logic ops, fixed-function point sprites.
- **Window-buffer tests on the surfaceless platform**: tests that read, draw
  or blit the front buffer or the window's depth/stencil abort or read nothing.

## Open bug: NPOT cube maps lock the 3D (EA3, 2026-11-03)

`arb_texture_cube_map@cubemap npot` and `arb_pixel_buffer_object@cubemap npot
pbo` draw cube maps of NPOT faces (50, 25, 12, 6 and 3 texels), first with one
level, then with mipmaps. Each draw that samples a 50 or 25 face takes an SGX
recovery (lockup_ta and lockup_3d, no BIF fault): 69 in the 56 seconds the test
runs. Skipping only the mipmapped draws still left 48 of them.

The cause is that no layout for NPOT cube faces is known: `face_stride_levels`
was measured on POT faces only, and dEQP's NPOT cube tests fail too (the DDK's
as well). As a **stopgap**, not a fix, a draw is skipped when it samples an NPOT
cube map of 16 texels or more, or a mipmapped one, with one `sgx: NPOT cube
maps ... not drawn yet` per context. With it the two tests take 0.6 s and no
recoveries. The fix is the DDK's layout for these, from a capture, after which
the guard goes.

## The DDK's desktop GL, as an oracle

The DDK ships a desktop GL: `libPVROGL_MESA.so` (IMG's GL built on Mesa, with
its own GLSL compiler) and its helper `libegl4ogl.so`. It is a GLX driver:
`libegl4ogl.so` exports no EGL entry points, and the DDK's `libEGL` rejects
`EGL_OPENGL_API` (`EGL_BAD_PARAMETER`), so without X it can't be run on an EA,
only read. Its extension strings (EA1, 2026-11-02) are a GL 2.0 set:

- **there:** `ARB_occlusion_query`, `EXT_texture3D`, `ARB_shadow` (+
  `_ambient`, `EXT_shadow_funcs`), `ARB_depth_texture`, `ARB_point_sprite`,
  `ARB_pixel_buffer_object`, `ARB_draw_buffers`, `ARB_texture_float`,
  `ARB_color_buffer_float`, `ARB_half_float_pixel`,
  `EXT_framebuffer_multisample`, `EXT_framebuffer_blit`,
  `ARB_texture_border_clamp`, `EXT_stencil_two_side`, GLSL 1.10;
- **not there:** `EXT_texture_sRGB`, which GL 2.1 needs.

So the hardware path for occlusion queries and 3D textures exists (the
driver for this core uses it), while sRGB has no oracle here: Intel's
Windows GL 3.0 ICD for this core is the only one (AGENTS.md M6).
