SGX
===

``sgx`` is a Gallium driver for Imagination Technologies PowerVR SGX
(Series 5) GPUs.

========= ============ ================ ===========
Product   SoC          Core ID, rev     Status
========= ============ ================ ===========
SGX545    Intel CE5300 0x0115, 1.0.14   Supported
========= ============ ================ ===========

The SGX5xx cores differ in their USSE instruction set and state words: the
SGX530, 531, 535 and 540 against the SGX543, 544 and 554 (Series 5XT). The
driver keeps what each core can do in a table keyed by the core ID the
kernel reports (``sgx_device.c``). A core without an entry is not driven:
screen creation fails with a message naming its ID.

Kernel driver
-------------

The driver runs on the ``powervr_sgx`` DRM driver. It reads the core's ID,
revision and render-target limits with ``DRM_IOCTL_POWERVR_SGX_DEV_QUERY``
and submits render jobs only. The SGX is a render-only device: scanout goes
through a separate display driver, paired with ``kmsro``.

Supported APIs
--------------

- **OpenGL ES 2.0.**
- **OpenGL 2.0** (desktop), with GLSL 1.20. ``EXT_texture_sRGB``, the
  rest of 2.1, waits on its piglit tests.
- **OpenGL ES 1.1** and **OpenGL 1.x**, through Mesa's fixed-function
  shaders.

Shaders compile through NIR to the USSE instruction set (``src/sgx/compiler``).
Mediump and lowp fragment arithmetic runs in 16 bits (F16, and C10 for
colour constants).

Known limitations
-----------------

- One render target; no multisampling; no 3D textures.
- Non-power-of-two cube maps of 16 texels or more, and mipmapped ones, are
  not drawn yet.
- Depth and stencil are kept per tile within one render and not stored, so
  their contents do not carry over from one render to the next.
- ``glPolygonOffset``'s units and factor are one pair per render: a draw
  with another pair ends the render, so depth does not carry over to it.

Environment variables
---------------------

``SGX_DEBUG``
   A comma-separated list of:

   ``nir``
      print every shader's NIR
   ``draw``
      print every draw
   ``frame``
      print every frame submitted
   ``sync``
      print allocations and waits
   ``stats``
      print, every 2 s, where the time goes: draws, flushes and their
      causes, waits, the vertex cache, and what the GPU was given
   ``serial``
      wait for every render after submitting it
   ``flushall``
      submit at every flush instead of deferring a plain ``glFlush``
   ``hangabort``
      abort at the first render that does not finish
   ``novcache``
      no vertex cache
   ``nof16``
      no 16-bit fragment arithmetic

``SGX_COMPILER_DEBUG``
   Flags that each turn off one compiler optimization, for bisecting a
   miscompile (``src/sgx/compiler/README.md``).

``SGX_DUMP_CODE``
   A directory: every compiled program's instruction words are written there.

Testing
-------

``src/gallium/drivers/sgx/ci`` holds the expectation files for
dEQP-GLES2 (``sgx-fails.txt``, ``sgx-flakes.txt``, ``sgx-skips.txt``) and for
piglit's ``gpu`` profile (``sgx-piglit-*.txt``), in deqp-runner's format.
``meson test --suite sgx`` runs the compiler, instruction-set and
command-stream unit tests.
