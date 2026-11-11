# OpenGL 3.0, 3.1 and 3.2 on the SGX545: what the hardware does

The target is GL 3.1; 3.2 is not planned, since no hardware geometry-shader
path is known (see "Verdict").

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
| `MAX_SAMPLES` ≥ 4 (`EXT_framebuffer_multisample`) | **native 4×**: 2×2 samples a pixel, resolved by the PBE at the end of the tile (box filter). TE AA b31 + b30 (`e0000000`), PBE word 2 b30-31 = 1 (downscale), X macrotile boundaries 6/12/18 and 0x30 tiles a macrotile, TA flags 2 b25, region headers and tail pointers 3× and 4×, per-sample depth/stencil (4× the 1-sample buffer) stored through ZLS. The colour target, its pitch and the tile grid stay the pixels' | the TA tail, RT data, region arrays and ZLS words as STATE.md "Multisampling (4x)" lists them. Under powervr_sgx the kernel computes TE AA, MTILE and the region arrays: uAPI rev 3 adds a samples field (1 or 4, 0 = 1, behind a per-core cap bit) and the kernel's K-calc follows it; the driver's MSAA surfaces, FBOs and resolve blit come after that. 2 and 8 samples, MSAA FBOs and the per-sample ZLS layout are not captured | capture (`ms1`/`ms4`, `ms1d`/`ms4d`: 480 edge pixels at exactly 1/4, 2/4 and 3/4 coverage); STATE.md "Multisampling (4x)" (dev ba9c83a4) | high (window surfaces); medium (FBOs, resolve blits) |
| 8 clip distances | 6 planes in the TA (`fdpc` → cp0-5, output select b0-5) | 7 and 8 lowered to a discard (`nir_lower_clip_fs`) | STATE.md §3 "Clip planes" (capture) | high |
| flat varyings (GLSL 1.30 `flat`; every integer varying is flat) | flat iteration works only on the TA's vertex-colour source, one vec4: DOUTI b25-26 on a texcoord set's iteration does nothing | one flat vec4 through the colour source; the rest by making the provoking vertex's values reach all three vertices (see "Flat varyings" below) | hardware (2026-11-11: the colour source flat, a texcoord set's flat bit no effect); STATE.md §3 "Clip planes, shading…" (DOUTI b25-26) | medium |

### Flat varyings

The hardware keeps one vec4 flat, the vertex-colour source, so the driver has
to make other flat varyings come out equal at a triangle's three vertices.
Then a smooth iteration returns the provoking vertex's value: the plane
through three equal values is constant (exact in F32 for whole numbers up to
2^24 and the 16-bit halves the compiler passes integers as; **unconfirmed**
until a test reads back such a varying across a triangle).

1. **The colour source** takes the first flat vec4, or up to four flat
   scalars packed into it. What precision the colour source iterates at (F32,
   or clamped or narrowed the way a vertex colour may be) is **unknown**, and
   decides whether integers and arbitrary floats can use it: a test that
   passes 0x3f800001, -3.5, 1e20 and 65535.0 through it flat and reads them
   back exactly.
2. **The rest, by replicating the provoking vertex.** The driver already
   unindexes draws on the CPU, so a draw whose program has more flat outputs
   than the colour source holds becomes a triangle list in which each vertex
   also carries the provoking vertex's inputs. A vertex-program variant
   computes the flat outputs from those copies (a second copy of the shader's
   NIR, reading the copied inputs and writing only the flat outputs). Only
   the inputs the flat outputs depend on are copied, within the 16 attributes.
   Lines take the second vertex, points need nothing.
3. **When that doesn't fit** (too many inputs to copy), the draw is refused
   with a message, not drawn wrong.

The provoking vertex is the last one (STATE.md §3); `flatshade_first` is a
rotation of each triangle's vertices on the CPU before step 2.

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
| 16 vertex texture units | the vertex program samples with `smp` (h8-9 = 2) | the compiler emits vertex samples (dependent `smp`); the driver has fragment samplers only (`sgx_screen.c`) and needs vertex sampler views and states, their DOUTT words in the vertex program's SA, and the size constants (lower_txs_int) for vertex units: until then a vertex program that samples is refused | ISA.md "Vertex texture fetch" (corpus); listed (ICD: 16) | medium |

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
- **H2, sampler format codes** (`tools/isa/hwt_fmt.py`). Sample a pattern texture with every T1 format
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
- **H6, multisampling.** Done as a capture: the DDK's ES2 renders 4× (see the
  table). Still open: an MSAA FBO and a resolve blit (`glBlitFramebuffer`),
  the per-sample ZLS layout (for multisample textures, which read samples
  one by one), and a replay of `ms4` on the open stack.
- **H7, an instance count in the VDM draw.** Lower priority: the per-instance
  path needs nothing.
- **H8, seamless cube filtering.** A LINEAR sample on a cube map's face edge,
  with the T0 and T3 bits that no capture sets swept one at a time.

## Verdict

- **GL 3.0 can be exposed honestly** if H1 (or the pass-per-target fallback)
  gives four or more color outputs. Real 4× multisampling is there (H6, a
  capture); it needs a uAPI samples field and the driver's half. Mesa's fake
  MSAA (`fake_sw_msaa`) is not an option. Everything else in 3.0 has a path that
  needs no fact we lack: shader packing for ≤ 32-bit float and integer
  targets, the 0x12 and unorm sampler formats plus shader conversion for
  sampling, st's RGTC decompression, sRGB in the shader, CPU-side vertex
  formats. Transform feedback has a `st` path (H4) and the CPU `draw`
  module as its fallback. Occlusion queries are the weakest link: no path is
  known, and conditional render depends on them.
- **GL 3.1 adds nothing the hardware has to do** beyond 3.0: instancing by
  per-instance draws, UBOs by `ld.a.d` (which runs today), snorm by shader
  conversion, primitive restart on the CPU. It's driver and compiler work.
- **GL 3.2 is not planned.** No hardware geometry-shader path is known, and
  running geometry shaders on the CPU (the `draw` module) was ruled out
  (decision, 2026-11-11). Depth clamp, seamless cube maps and the other 3.2
  pieces in the table above stay recorded, but nothing is built for them.

**Target: GL 3.1 (GLSL 1.40).** Until MRT and MSAA work, Mesa's version stays
at 2.1, but each extension below can be exposed on its own as it passes. The
pieces a clean 3.1 needs: real 4× MSAA (H6), occlusion queries (H5), MRT and
the render and sampler formats (H1-H3), transform feedback (H4), UBOs,
instancing and primitive restart.

What's emulated, in one list: RGTC (st), R11G11B10F / RGB9E5 sampling (shader
unpack), snorm (shader), integer sampling (shader scale or `ld`), sRGB
(shader), float filtering beyond what the TPU filters (four taps), primitive
restart and provoking vertex (CPU), instancing (one draw per instance),
texture arrays (dependent samples at a layer address), clip distances 7-8
(discard).

## Order of work

Driver-side pieces that need no new hardware fact come first; each goes in
behind its cap only when piglit and dEQP pass it.

1. CPU-side vertex and draw features: `NV_primitive_restart`,
   `ARB_draw_elements_base_vertex`, half-float and BGRA vertex formats,
   provoking vertex first (see "Status").
2. Formats by swizzle and conversion: R8 and RG8 (`ARB_texture_rg`'s 8-bit
   part, and RGTC through st), rectangle textures, then 8-bit snorm (decode
   in the shader, four taps when filtered, as sRGB).
3. Instancing by per-instance draws, `gl_InstanceID` from a driver constant
   (see "Status").
4. UBOs (the driver's half: addresses in SA; the compiler's half: `ld.a.d`).
5. The box tests: H2 and H1 (one slot covers H1-H3), then a replay of the
   4× capture on the open stack and an MSAA FBO capture (H6), H5, H4. The
   uAPI samples field goes to the kernel driver's owners.
6. Render targets: shader packing for 32-bit float and integer formats, then
   MRT once H1 says how.
7. Float and integer sampling formats (H2), texture arrays, transform feedback
   (H4), occlusion queries (H5), conditional render.
8. GL 3.2 is not planned (see "Verdict").

## Status

Done in the driver, built and unit-tested on the host, **not yet run on a
CE5300 board** (piglit `arb_draw_instanced`, `arb_instanced_arrays`,
`arb_base_instance`'s draws, `nv_primitive_restart` and
`gl-3.1-primitive-restart` are the gate):

- primitive restart, the NV extension and the fixed index: split on the CPU
  (`sgx_draw_vbo`);
- instancing: one draw per instance, `gl_InstanceID` in vertex constant
  `SGX_K_INSTANCE` as a float (`lower_instance_id`), instanced elements
  fetched at base instance + instance / divisor (`setup_fetch`); caps
  `vs_instanceid`, `vertex_element_instance_divisor`, `start_instance`. The
  cost is one draw an instance: a draw of thousands of instances becomes
  thousands of draws;
- pure integer vertex attributes keep their bits, with an integer 1 for a
  missing w.

Already there before this work: base vertex (`index_bias`), any plain vertex
format through the CPU gather (half-float and BGRA among them), R8 and RG8
stored as RGBA8 (so st's RGTC decompression has its formats), rectangle
textures (`lower_rect` with the size from `SGX_K_TEXSIZE`).

Provoking vertex first waits for flat shading, which the driver doesn't do
yet (DOUTI b25-26, STATE.md §3).

## Crossing the driver/compiler line

Agreed with the compiler (2026-11-11):

- **Integers:** the compiler's `native_integers` option is set for every
  shader once the screen reports integers. Gallium's cap is screen-wide, so
  GLSL ES 1.00 shaders then come with integers too, and dEQP-GLES2 has to
  stay where the float path has it.
- **gl_InstanceID** is the driver constant `SGX_K_INSTANCE`, lowered in the
  driver (`lower_instance_id`): a float today, raw int bits under
  `native_integers`.
- **gl_VertexID** is not the PDS's `in0`: the driver rebases indices on most
  paths (the CPU gather, the vertex cache, expanded primitives). The driver
  lowers `load_vertex_id` to an extra vertex input it gathers itself, the
  original index plus the base vertex.
- **textureSize, texelFetch:** driver constants in a block above the
  compiler's 1024 user constants, the base level's width, height and depth
  per unit; level sizes are computed in the shader. A texelFetch is a
  dependent sample at (i + 0.5) / size with its own texture state from the
  driver (NEAREST, no mips, clamp, the level's address), never the bound
  sampler's filter or wrap; the compiler marks those samples.
- **Driver constants**, above the user constants and the compiler's other
  numbers (`SGX_MAX_CONST_K` = 1280): 1024 + 3u the base level's width,
  height and depth of unit u (0-15); 1072 gl_InstanceID; 1080 + n UBO n's
  GPU address (`ubo_base_k` = 1080, up to 16); the rest of 1024-1279
  reserved. 960 + 4s are the border colors. `SGX_K_TEXSIZE` (1000) and
  `SGX_K_INSTANCE` (1016) move into the block when the driver takes the
  compiler's `native_integers`. Under `native_integers` the sizes and
  gl_InstanceID are raw integers, otherwise floats. The compiler's
  `sgx_lower_tex` runs first (textureGrad, offsets, rect, 1D, texelFetch); a
  texelFetch sample (`sgx_sample_info.fetch`) gets NEAREST, MIPMAP_NEAREST
  and clamp on the bound texture.
- **Flat varyings:** the compiler puts the first flat varying that fits one
  vec4 (a split int or ivec2 counts as four floats) on the colour source and
  says which in `sgx_varyings` and `sgx_shader`; the driver sets output
  select b11 and the DOUTI source 0x20 iteration for it. The driver
  replicates the provoking vertex for every other flat varying, with a
  vertex-program variant made on plain NIR before `sgx_compile` whose
  outputs, names and types are the original's.
- **Render-target formats:** the fragment variant is keyed on the bound
  surfaces' formats; the compiler's color epilogue packs each output into
  its surface's 32-bit word, and the driver names the o register of each
  output (o0 alone until H1).
- **UBOs:** `load_ubo` is an `ld.a.d` from the address in constant
  `ubo_base_k + n`.
- **Integer varyings** pass as two exact 16-bit halves until a test shows
  that a flat iteration keeps raw 32-bit words (needs flat shading in the
  driver first).

Still open:

- the o registers of outputs past the first, and of formats wider than 32
  bits (H1);
- transform feedback turns Mesa's varying packing back on: st disables
  packing only while `EXT_transform_feedback` is off (`st_extensions.c`, next
  to `max_tex_indirections`). The compiler's varying map has to handle packed
  slots first.
