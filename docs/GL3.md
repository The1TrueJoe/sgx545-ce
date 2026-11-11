# OpenGL 3.0, 3.1 and 3.2 on the SGX545: what the hardware does

For each feature that GL 3.0, 3.1 and 3.2 add, this says whether the SGX545
does it in hardware, what the driver does if it doesn't, the evidence for
both, and the test that would settle what is open. The verdict and the order
of work are at the end. The compiler's side of the same features (integer
ALU, texelFetch, UBO loads, gl_InstanceID) is in `COMPILER-GL3.md`; this
document covers the hardware capability, the state and the resources.

## How each fact is established

The tags of `STATE.md` and `TEX.md`, plus two kinds of second-hand evidence:

- **hardware**: confirmed on a CE5300 board (replay, native run, or `hwt_*`
  black-box test), with the output checked.
- **hardware, open driver**: the open driver does it and runs on the board.
- **capture**: seen in the DDK's captured output.
- **listed**: an implementation of this core claims it, but nothing here
  shows how it is done. Two lists are used:
  - *ICD*: Intel's Windows GL 3.0 driver for this core (`ig4icd32.dll`
    8.14.10.2117), as reported by a public capability survey: GL 3.0, GLSL
    1.30, `MAX_DRAW_BUFFERS` 8, `MAX_SAMPLES` 4, `MAX_ARRAY_TEXTURE_LAYERS`
    256, transform feedback 4 separate attributes × 4 components or 64
    interleaved, 16 vertex texture units, 6 clip planes, `MAX_3D_TEXTURE_SIZE`
    256, texture size 8192, renderbuffer size 2048. Its extension string has
    `ARB_draw_buffers`, `ARB_texture_float`, `ARB_color_buffer_float`,
    `ARB_half_float_pixel`, `ARB_framebuffer_object`,
    `EXT_framebuffer_multisample`, `EXT_packed_depth_stencil`,
    `ARB_occlusion_query`, `EXT_texture3D`, `ARB_texture_rectangle`. It does
    **not** list `EXT_texture_integer`, `EXT_texture_array`,
    `EXT_transform_feedback`, `ARB_texture_rg`, `EXT_texture_sRGB`,
    `EXT_framebuffer_sRGB`, `NV_conditional_render`, `ARB_draw_instanced`,
    `ARB_depth_clamp` or `ARB_seamless_cube_map`. A GL 3.0 driver has the
    3.0 features in core whether or not it lists their extensions, but a
    driver can do any of them in software, so *ICD* says "possible", never
    "native".
  - *DDK GL*: the DDK's own desktop GL (`PIGLIT.md`, "The DDK's desktop GL"),
    a GL 2.0 set with `ARB_draw_buffers`, `ARB_texture_float`,
    `ARB_color_buffer_float`, `EXT_framebuffer_multisample`,
    `ARB_occlusion_query`.
- **unconfirmed**, **guess**: as in `STATE.md`. Shipping code must not rely
  on them.

**Confidence** in the "can we do it" column: **high** (hardware or a
driver-only path with no hardware unknown), **medium** (a hardware path
one test away from usable, with a fallback), **low** (needs facts nobody has
yet, and the fallback is expensive), **none** (no known path).

## What Mesa checks

Mesa computes the version from extensions and limits
(`src/mesa/main/version.c`, `compute_version`). These are the gates, so they
are the list below. Each one also appears in the GL spec's core features.

- **3.0:** GLSL 1.30; `MaxColorAttachments` ≥ 4 (the spec says 8; Mesa
  allows 4); `MaxSamples` ≥ 4, or Mesa's fake MSAA; `ARB_color_buffer_float`,
  `ARB_depth_buffer_float`, `ARB_half_float_vertex`, `ARB_map_buffer_range`,
  `ARB_shader_texture_lod`, `ARB_texture_float`, `ARB_texture_rg`,
  `ARB_texture_compression_rgtc`, `EXT_draw_buffers2`,
  `ARB_framebuffer_object`, `EXT_framebuffer_sRGB`, `EXT_packed_float`,
  `EXT_texture_array`, `EXT_texture_integer`, `EXT_texture_shared_exponent`,
  `EXT_transform_feedback`, `NV_conditional_render`.
- **3.1:** GLSL 1.40; `ARB_draw_instanced`, `ARB_uniform_buffer_object`,
  `EXT_texture_snorm`, `NV_primitive_restart`, `NV_texture_rectangle`; 16
  vertex texture units.
- **3.2:** GLSL 1.50 (geometry shaders); `ARB_depth_clamp`,
  `ARB_draw_elements_base_vertex`, `ARB_fragment_coord_conventions`,
  `EXT_provoking_vertex`, `ARB_seamless_cube_map`, `ARB_sync`,
  `ARB_texture_multisample`, `EXT_vertex_array_bgra`.

Occlusion queries are GL 1.5 and Mesa doesn't gate on them, but GL 3.0's
conditional render is built on them. The driver has no queries today
(`sgx_context.c` creates none), so they're listed under 3.0.

## The pixel path is the main unknown

Most of 3.0's render-target features (MRT, float and integer color buffers,
64- and 128-bit formats) come down to one question: **can a pixel carry more
than one 32-bit output to the PBE?**

What is known:

- The fragment program ends with its result in `o0`, and the PBE stores o0
  unchanged (ISA.md; **hardware, open driver**). Blending is shader code that
  reads the tile's o0 (STATE.md §3, "Blending and the color mask are shader
  code"; **hardware, open driver**).
- The end-of-tile program is four LIMMs (the PBE words) and two `emit.t0`
  (STATE.md §4; **capture**, **hardware, open driver**). Every capture has one
  surface and one emit pair.
- The PBE sideband has a field, b6-11, "a per-surface value added for format
  codes 3-14", 0 in every capture (STATE.md §4, PBE words; **unconfirmed**).
  It is the candidate for "which output register this surface takes"
  (**guess**).
- *ICD* and *DDK GL* both list `ARB_draw_buffers` (8 on the ICD), so the
  core can write several targets somehow (**listed**).

Because the PBE writes o0 raw, **any 32-bit-per-pixel format can already be
rendered by packing in the fragment shader**: R32F, RG16F, R11G11B10F,
RGB10A2, RGBA8UI, RG16UI, R32UI and so on, with blending done in the shader
(it already is) after unpacking o0. That needs no new hardware fact. It's how
the driver would do float and integer render targets of 32 bits or less.
Formats wider than 32 bits, and more than one draw buffer, need either:

1. **More than one output per pixel** (o1, o2, …) and one PBE emit per
   surface. Test **H1** below. This is the real path.
2. **A 3D pass per output**, all from one TA pass: the parameter buffer is
   reused, and each pass's end-of-tile program stores a different output.
   The fragment program's choice of output would come from a secondary
   constant the driver changes between passes. This depends on rendering the
   same parameter buffer more than once, which the microkernel doesn't do
   today (`UKERNEL.md`). It's the fallback, and costs one 3D pass per extra
   32-bit target word.

## GL 3.0

| feature | hardware | the driver's path | evidence | confidence |
|---|---|---|---|---|
| GLSL 1.30 | integer ALU, bitwise, int↔float conversion exist | gl3-compiler's work | ISA.md "What a backend would use beyond GLSL ES 1.0" (encoder; hardware for the bitwise ops, hwt_p7) | medium (compiler) |
| 8 draw buffers (MRT), `EXT_draw_buffers2` | unknown how; **listed** (ICD: 8) | H1; fallback a 3D pass per target. Mesa accepts 4. `EXT_draw_buffers2` (per-target blend and mask) is free once MRT works: blend and mask are shader code per output | see "The pixel path" | medium |
| float textures, RGBA32F / RGBA16F / R32F … (`ARB_texture_float`) | one F32 one-channel sampler format, 0x12, unfiltered | 0x12 for R32F and depth; other float formats need their T1 codes (**H2**, or the DDK's `OES_texture_float` captures, `PROBE_TEXFMT=rgbaf32`, `rgbaf16`, `lf32`, `lf16`). Filtering of unfiltered formats: four taps in the shader, as for sRGB (`lower_srgb_filter`) | TEX.md §15 (capture: 0x12; hardware: unfiltered); ICD and DDK GL list it | medium |
| float render targets (`ARB_color_buffer_float`) | unknown PBE codes | 32-bit formats by shader packing (above); wider ones need H1. Clamping control is shader work | STATE.md §4 PBE words (o0 raw); listed | medium (≤ 32 bit: high) |
| half-float vertex attributes (`ARB_half_float_vertex`) | — | the driver gathers vertices on the CPU (`sgx_draw.c`) and converts | hardware, open driver (the gather) | high |
| integer textures (`EXT_texture_integer`) | no integer sampler format known | 8-bit: sample the unorm format (0x0d, 0x1a, 0x1b) NEAREST and scale by 255 in the shader; 32-bit one-channel: 0x12 if it returns the stored bits unchanged (**H2**); wider, or as a general path: `texelFetch` through `ld` from the texel's address. Integer textures are never filtered, so NEAREST is all GL asks | TEX.md §1 (formats), §15 (0x12); ISA.md `ld` (capture); the ICD doesn't list the extension | medium |
| integer render targets | — | ≤ 32 bits per pixel by shader packing; no blending, no dither | STATE.md §4 | high (≤ 32 bit) |
| `ARB_texture_rg` (R8, RG8, R16F, RG16F, R32F, RG32F) | R8 and RG8 as L8 (0x1a) and LA88 (0x1b) with a swizzle | sampler-view swizzle (already done in the shader, `sgx_fs_for`); render R8/RG8 by packing; the float ones as above | TEX.md §1 (hardware, open driver: 0x1a, 0x1b) | high (R8, RG8) |
| RGTC (`ARB_texture_compression_rgtc`) | none known | st decompresses to R8/RG8 on upload when the screen refuses RGTC (`st_format.c`); needs only R8 and RG8 | Mesa | high |
| sRGB textures and framebuffers (`EXT_framebuffer_sRGB`) | sRGB decode in the shader today; no TPU sRGB format known; the PBE writes o0 raw | sampling: as today (M10); rendering: encode in the shader before the PBE, blend in linear after decoding o0 (`dest_surface_srgb_control`) | UPSTREAM-READINESS M10 (hardware, open driver); neither list has it | high |
| `EXT_packed_float` (R11G11B10F), `EXT_texture_shared_exponent` (RGB9E5) | no sampler format known | sample: upload converted to RGBA16F (needs its T1 code, H2) or unpack the 32-bit word in the shader from an RGBA8 view (`nir_format_unpack_11f11f10f`, `nir_format_unpack_rgb9e5`) under NEAREST + four-tap filtering; render R11G11B10F by packing | Mesa's NIR format helpers | medium |
| `ARB_depth_buffer_float` (D32F, D32F_S8) | depth samples as F32 (0x12) already; the ZLS buffer's own format unknown beyond the DDK's | sampling: 0x12; as a depth buffer: the ISP's depth precision is F32 internally (**guess**), and storing/loading it goes through ZLS. Needs a test that a D32F depth survives a ZLS store and load bit-exact | TEX.md §7, §15 | medium |
| texture arrays (`EXT_texture_array`, 256 layers listed) | no layer field known in T0-T3 | one texture per layer at a layer stride; the shader adds `layer × stride` to the base address and samples with the dependent form, whose texture state comes from registers (ISA.md, "dependent: smp template"; hardware, open stack hwt_smp). Needs a compiler path (gl3-compiler). 1D arrays as 2D arrays of height 1 | TEX.md §5 (T2 = base); ISA.md (dependent smp with state in SA: hardware, A/B) | medium |
| transform feedback (`EXT_transform_feedback`; ICD 4 × 4 separate, 64 interleaved) | no stream-out unit known | the vertex program stores its outputs with `st` at `vertex index × stride`; the driver unrolls strips, fans and indices on the CPU (it gathers them already) so the index is the output slot; counts and `PRIMITIVES_WRITTEN` are known on the CPU. Needs `st` to a driver address from a vertex program (**H4**). Fallback: gallium's `draw` module runs the vertex shader on the CPU and writes the buffers | ISA.md `ld`/`st` (corpus `st.l.d`); the ICD's small limits fit a store-based path | medium |
| `NV_conditional_render` | — | `render_condition` waits on the query on the CPU and skips draws (the spec's NO_WAIT modes may draw) | needs occlusion queries | depends on queries |
| occlusion queries (GL 1.5, `SAMPLES_PASSED`) | **listed** (ICD, DDK GL: `ARB_occlusion_query`); no capture, no register known | **H5**. No good fallback: a fragment program can't count without atomics; `ANY_SAMPLES_PASSED` could be a store of 1 under a translucent pass, which shades in order | listed only | low |
| `ARB_shader_texture_lod` | `smp .lod` and `.grad` forms exist | compiler | ISA.md smp (encoder; `tex_lod` corpus) | medium (compiler) |
| `ARB_map_buffer_range`, `ARB_framebuffer_object` | — | gallium generic; FBO needs mixed attachment sizes, which the render target handles by its own size | — | high |
| `MAX_SAMPLES` ≥ 4 (`EXT_framebuffer_multisample`) | TE AA b30-31 = 2× in X / Y; PBE word 2 b30-31 = downscale; **listed** (ICD: 4) | 4× by rendering at 2×2 and downsampling in the PBE (**H6**, or a DDK ES2 capture with a 4-sample EGL config). Mesa's fake MSAA (`caps->fake_sw_msaa`) passes the gate with one sample; whether that is honest enough is Joseph's call | STATE.md §1 tail (+0x238: unconfirmed), §4 (unconfirmed) | medium |
| 8 clip distances | 6 planes in the TA (`fdpc` → cp0-5, output select b0-5) | 7 and 8 lowered to a discard (`nir_lower_clip_fs`) | STATE.md §3 "Clip planes" (capture) | high |

## GL 3.1

| feature | hardware | the driver's path | evidence | confidence |
|---|---|---|---|---|
| GLSL 1.40 | — | compiler | — | compiler |
| instancing, `gl_InstanceID` (`ARB_draw_instanced`) | the PDS vertex program's `in1` is the instance index (**unconfirmed**); no instance count is known in the VDM draw words | one VDM draw per instance, each with its own small PDS vertex program that writes the instance number with DOUTA into the primary attribute the compiler reads `gl_InstanceID` from. Native instancing, if H7 finds the count, replaces it | PDS.md (DOUTA: hardware; `in1`: unconfirmed); STATE.md §2 VDM draw | high (per-instance draws) |
| instance divisors (`ARB_instanced_arrays`, 3.3, but cheap here) | PDS has the divisor's reciprocal sequence (capture) | per-instance draws: the per-instance stream's base moves on the CPU | PDS.md | high |
| uniform buffers (`ARB_uniform_buffer_object`, 16 KiB, 12 per stage) | `ld.a.d` from a driver address: the state-emit programs the driver runs already use it | the driver puts each UBO's address in an SA register; the compiler loads with `ld.a.d`. Small UBOs could be copied into SA, but 16 KiB doesn't fit | STATE.md §2 (the state-emit program's `ld.a.d`: hardware, open driver); `sgxws_const_mem` already loads constants that don't fit in SA | high |
| `EXT_texture_snorm` | no signed sampler format known | sample the unorm format and convert in the shader (`2x - 1` from the byte, with the -1 clamp GL asks); render by packing | TEX.md §1 | high |
| primitive restart (`NV_primitive_restart`) | none known | split on the CPU (`util_draw_vbo_without_prim_restart`); the driver already reads indices on the CPU | `sgx_draw.c` | high |
| rectangle textures (`NV_texture_rectangle`) | NPOT linear layout (T1 layout 3) | unnormalized coordinates scaled in NIR (`nir_lower_tex` `lower_rect`) | TEX.md §1 (hardware) | high |
| texture buffer objects (3.1 core; Mesa doesn't gate on them) | — | a linear 1D texture of the buffer, `texelFetch` through `ld` | ISA.md `ld` | medium |
| 16 vertex texture units | the vertex program samples with `smp` (h8-9 = 2) | today the driver has fragment samplers only (`sgx_screen.c`); needs vertex sampler state in the vertex program's SA | ISA.md "Vertex texture fetch" (corpus); listed (ICD: 16) | medium |

## GL 3.2

| feature | hardware | the driver's path | evidence | confidence |
|---|---|---|---|---|
| geometry shaders (GLSL 1.50) | an `emit.t1` "geometry vertex emit" and a USP program type 2 exist in the encoder; nothing shows where a geometry program runs or how vertices reach it. Not in the ICD (GL 3.0) | gallium's `draw` module on the CPU: a draw with a geometry shader runs its vertex and geometry shaders on the CPU, then the hardware draws the result with a pass-through vertex program. Slow, correct, and how softpipe and i915g run them | ISA.md hw 1f (encoder); program type 2 unconfirmed | low (native: none) |
| depth clamp (`ARB_depth_clamp`) | no z-clip control known | the vertex program keeps z inside the clip volume and passes the real depth as a varying; the fragment program writes the clamped depth. Needs gl_FragDepth, which isn't written yet (PIGLIT.md) | STATE.md §3 | low |
| seamless cube maps (`ARB_seamless_cube_map`) | unknown | **H8**: look for a T-word bit that filters across faces. Fallback: LINEAR cube sampling in the shader, four taps that cross faces | TEX.md §8 | low |
| multisample textures (`ARB_texture_multisample`) | depends on MSAA (above) | with 2×2 supersampling, an unresolved surface is a texture at twice the size and `texelFetch(s, p, i)` reads texel `2p + offset(i)` | — | follows MSAA |
| `EXT_provoking_vertex` (first vertex) | the last vertex provokes; no word selects it | rotate each triangle's vertices on the CPU (a, b, c → b, c, a keeps the winding); lines swap | STATE.md §3 (the MTE and VDM words don't select it: hardware) | high |
| `ARB_draw_elements_base_vertex` | — | already: the draw adds `index_bias` on the CPU (`sgx_draw.c`) | hardware, open driver | high |
| `ARB_fragment_coord_conventions` | upper-left, half-integer origin | st lowers the other conventions | `sgx_screen.c` | high |
| `ARB_sync` | fences on the completion of a kick | already (fence objects in the winsys) | hardware, open driver | high |
| `EXT_vertex_array_bgra` | — | the CPU gather swizzles | — | high |

## Tests that settle the open hardware questions

Each is a black-box test with the open stack on a CE5300 board (`tools/isa/hwt_*`
style), unless it says capture. In order of how much each unblocks:

- **H1, two outputs per pixel.** A fragment program writes o0 = A, o1 = B. The
  end-of-tile program emits two PBE word sets, to two buffers, the second with
  sideband b6-11 = 1 (then 2, 4, …). Read both buffers. If buffer 2 holds B,
  MRT and wide formats are native. Then: how many outputs (8 for 3.0; 4 for
  RGBA32F), and whatever sets the output buffer's size (DOUTU or render
  details). Unblocks MRT, 64/128-bit formats.
- **H2, sampler format codes.** Sample a pattern texture with every T1 format
  code from 0x00 to 0x1f and record what each returns; and check that 0x12
  returns stored bits unchanged (NaN payloads, denormals, 0x80000000). Codes
  that fault are noted and skipped. A shorter way to the float ones: the DDK's
  ES2 captures with `PROBE_TEXFMT=rgbaf32`, `rgbaf16`, `lf32`, `lf16`, which
  `tools/probe` can already make.
- **H3, PBE format codes.** Render a known o0 with sideband codes 2 and 6-0x1f;
  read back the bytes each writes. Some may be wider formats, which makes the
  "raw o0" path unnecessary for them.
- **H4, a store from a vertex program.** A vertex program `st`s its outputs to
  a driver buffer at `index × stride` (index from in0 via DOUTA). Read the
  buffer. Unblocks transform feedback.
- **H5, occlusion queries.** No path is known. A DDK desktop-GL capture of
  `glBeginQuery(GL_SAMPLES_PASSED)` would show it, but that DDK needs X (it's
  GLX only). Otherwise, look for a counter in the render details' unknown
  words after a render with a visibility test.
- **H6, multisampling.** Capture the DDK's ES2 with a 4-sample EGL config if it
  offers one; or set TE AA b30-31 and PBE word 2 b30-31 on a replay of the base
  frame and look at the edges.
- **H7, an instance count in the VDM draw.** Lower priority: the per-instance
  path needs nothing.
- **H8, seamless cube filtering.** A LINEAR sample on a cube map's face edge,
  with the T0 and T3 bits that no capture sets swept one at a time.

## Verdict

- **GL 3.0 can be exposed honestly** if H1 (or the pass-per-target fallback)
  gives four or more color outputs, and multisampling is settled (H6, or
  Joseph accepts Mesa's fake MSAA). Everything else in 3.0 has a path that
  needs no fact we lack: shader packing for ≤ 32-bit float and integer
  targets, the 0x12 and unorm sampler formats plus shader conversion for
  sampling, st's RGTC decompression, sRGB in the shader, CPU-side vertex
  formats. Transform feedback has a `st` path (H4) and the CPU `draw`
  module as its fallback. Occlusion queries are the weakest link: no path is
  known, and conditional render depends on them.
- **GL 3.1 adds nothing the hardware has to do** beyond 3.0: instancing by
  per-instance draws, UBOs by `ld.a.d` (which runs today), snorm by shader
  conversion, primitive restart on the CPU. It's driver and compiler work.
- **GL 3.2 is possible only with geometry shaders on the CPU** (the `draw`
  module), depth clamp through gl_FragDepth, and seamless cube maps in the
  shader unless H8 finds a bit. That's honest (everything passes, slowly), but
  a geometry shader draw would run at CPU speed on an Atom. Exposing 3.2 is a
  decision for Joseph once 3.1 passes.

**Target: GL 3.1 (GLSL 1.40), with 3.2 as a follow-up decision.** Until MRT
works, Mesa's version stays at 2.1, but each extension below can be exposed on
its own as it passes.

What's emulated, in one list: RGTC (st), R11G11B10F / RGB9E5 sampling (shader
unpack), snorm (shader), integer sampling (shader scale or `ld`), sRGB
(shader), float filtering beyond what the TPU filters (four taps), primitive
restart and provoking vertex (CPU), instancing (one draw per instance),
texture arrays (dependent samples at a layer address), clip distances 7-8
(discard); and for 3.2, geometry shaders (CPU), depth clamp (gl_FragDepth),
seamless cubes (shader).

## Order of work

Driver-side pieces that need no new hardware fact come first; each goes in
behind its cap only when piglit and dEQP pass it.

1. CPU-side vertex and draw features: `NV_primitive_restart`,
   `ARB_draw_elements_base_vertex` (exposed), half-float and BGRA vertex
   formats, provoking vertex first.
2. Formats by swizzle and conversion: R8 and RG8 (`ARB_texture_rg`'s 8-bit
   part, and RGTC through st), rectangle textures.
3. Instancing by per-instance draws (needs the compiler's `gl_InstanceID`
   input: agreed with gl3-compiler).
4. UBOs (the driver's half: addresses in SA; the compiler's half: `ld.a.d`).
5. The box tests, in the order H1, H2, H4, H6, H5 (one slot covers H1-H3).
6. Render targets: shader packing for 32-bit float and integer formats, then
   MRT once H1 says how.
7. Float and integer sampling formats (H2), texture arrays, transform feedback
   (H4), occlusion queries (H5), conditional render.
8. Then 3.2's pieces, if Joseph wants 3.2.

## Crossing the driver/compiler line

These need the gl3-compiler session's agreement before code:

- the primary attribute that `gl_InstanceID` and `gl_VertexID` come in (the
  PDS vertex program's DOUTA target);
- how a fragment program writes a packed render-target format (a lowering
  the driver keys on the bound surfaces' formats, as sRGB decoding is keyed
  on the sampler views today), and later its outputs o1-o7;
- the UBO base addresses' SA registers;
- transform feedback turns Mesa's varying packing back on: st disables
  packing only while `EXT_transform_feedback` is off (`st_extensions.c`, next
  to `max_tex_indirections`). The compiler's varying map has to handle packed
  slots first.
