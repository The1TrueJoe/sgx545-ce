SGX
===

``sgx`` is a Gallium driver for Imagination Technologies PowerVR SGX
(Series 5) GPUs.

========= ============ ================ ===========
Product   SoC          Core ID, rev     Status
========= ============ ================ ===========
SGX545    Intel CE5300 0x0115, 1.0.14   Supported
SGX545    Cedarview    0x0115, 1.0.13   In progress
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
through a separate display driver, paired with ``kmsro``: ``simpledrm`` on
the CE5300, ``gma500`` on Cedarview, where ``powervr_sgx`` binds to an
auxiliary device of the display's PCI function and the loader takes the
render node of that child device.

Supported APIs
--------------

- **OpenGL ES 2.0.**
- **OpenGL 2.1** (desktop), with GLSL 1.20 and ``EXT_texture_sRGB``.
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
- Depth and stencil carry over from one render to the next (the 3D stores
  them at the end of a render and the next one loads them), but they are not
  read back: ``glReadPixels`` of depth or stencil returns no rendered values.
- ``glPolygonOffset``'s units and factor are one pair per render: a draw
  with another pair ends the render.
- sRGB textures sample right, but mipmap generation, alpha test against an
  sRGB framebuffer, and mirrored-repeat and border-colour filtering of them
  are wrong.

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
   ``noffmediump``
      fixed-function fragment programs at highp, not mediump
   ``nof16samples``
      texels a mediump program reads come back as RGBA8, not F16
   ``nodepf16samples``
      the same for texels of dependent samples
   ``nophasesplit``
      a fragment program that discards runs whole before the visibility
      test, not only up to its discard
   ``nopdsmerge``
      a texture sample is merged only into the previous iteration of its own
      coordinate set, not of another
   ``nozerocopy``
      vertex inputs gathered into f32 x 4, not DMAed where they are
   ``nozls``
      depth and stencil not kept across renders
   ``nolazyscanout``
      scanout buffers on a display-only device are the display's from the
      start, not from the first request for a KMS handle

``SGX_SLOTS``
   Frames in flight, 2 to 4 (4 by default). Fewer frames lower the latency of
   other clients, such as an X desktop behind a game, at some cost in the
   game's frame rate.

``SGX_DRY_RUN``, ``SGX_FRAME_GAP_US``
   For bisecting a GPU hang: frames from the n-th on are built but not sent;
   the GPU is left idle this long after each render.

``SGX_COMPILER_DEBUG``
   Flags that each turn off one compiler optimization, for bisecting a
   miscompile (``src/sgx/compiler/README.md``).

``SGX_DUMP_CODE``
   A directory: every compiled program's instruction words are written there.

Testing
-------

``src/gallium/drivers/sgx/ci`` holds the deqp-runner suites the expectations
come from, ``deqp-sgx-sgx545.toml`` (dEQP-GLES2 mustpass, 256x256 pbuffers)
and ``deqp-sgx-sgx545-piglit.toml`` (piglit's ``gpu`` profile, surfaceless),
and the expectation files for them: ``sgx-fails.txt``, ``sgx-flakes.txt``,
``sgx-skips.txt`` and ``sgx-piglit-*.txt``.  For example::

   deqp-runner suite --suite deqp-sgx-sgx545.toml \
      --baseline sgx-fails.txt --flakes sgx-flakes.txt --skips sgx-skips.txt \
      --output results
``meson test --suite sgx`` runs the compiler, instruction-set and
command-stream unit tests.
