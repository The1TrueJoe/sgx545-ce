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
- **A surfaceless display has no window configs**: a client must ask for
  `EGL_PBUFFER_BIT` (or `EGL_SURFACE_TYPE` 0), as dEQP and waffle do.

## First light (EA1, 2026-11-02)

With the shipping caps, a desktop context reports `OpenGL version string: 2.0
Mesa 26.2.5` on `PowerVR SGX545 (sgx545-ce)`. Mesa grants 2.1 for
`EXT_texture_sRGB` on top of 2.0, which needs an sRGB format the driver can
sample.
