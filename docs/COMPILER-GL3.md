# The shader compiler past GLES2: what M7 and M8 need (look-ahead, 2026-11-01)

This is exploration, not a plan of record. GLES2 (M0) comes first. For each
feature of AGENTS.md M7 (GLES 3.0 / GL 3.0) and M8 (GL 3.2) that touches the
compiler, it says what the ISA offers (with how each fact was established: ISA.md
tags), what the compiler would do, what is unknown, and the hardware test that
would settle it. No hardware was used; the EAs are offline.

## Oracles, and their limits

- **libusc as a compiler oracle reaches only GLSL ES 1.00.** Its front end
  (glslcompiler) accepts nothing newer. ES 1.00 `int`s become floats (ISA.md
  "What GLSL integer code compiles to"), so no DDK compile shows us integer
  ALU code, texelFetch, UBOs or geometry shaders. libusc does contain more
  paths: its encoders cover the integer, memory and emit forms, and the
  geometry emit (0x101) exists "only on the geometry-shader path". But reaching
  them means feeding libusc its intermediate form directly. That's a sizeable
  RE project of its own and not obviously worth it.
- **libusc's encoder, run in the emulator (oracle.py), is still the
  encoding oracle** for every form. `check_int.py` / `check_mem.py` already
  sweep the integer, bitwise, memory and emit forms, so their *encodings* are
  known. Their *semantics* are what's missing.
- **Hardware tests (hwt_*) settle semantics,** one small hand-assembled
  program per question, read back from `o0`. That's the main M7 tool, as it
  was for F16 and C10.
- **Intel's Windows GL 3.0 ICD** runs the same ISA, so its output would show
  ES3 constructs compiled by a real compiler. It needs a Cedarview machine with
  Windows to capture from (AGENTS.md §7.1). It's the only oracle for geometry
  shaders.

## M7: GLES 3.0 / GL 3.0

### Integer ALU and bitwise

What the ISA has (ISA.md "What a backend would use beyond GLSL ES 1.0"):

| need | instruction | established by |
|---|---|---|
| iadd/isub/imul 32-bit | `ima32` (high word to an internal register, carry-in from i0-i2); `imae` 16x16+32 with carry | encoder; ima16/imae also corpus (indexing) |
| 16-bit add/sub/mul | `ima16` (iadd16, isub16, imul16, signed/unsigned) | encoder, corpus; used today for dynamic indexing (A/B on hardware) |
| and/or/xor/shl/shr/asr/rol | hw 0a-0e | **hardware** (hwt_p7 `bit_*`), including "a register shift by 33 gives 0" |
| integer compares | `test` with integer ALU ops (`test.isub16`, `test.iadd32`, `test.and`) | encoder, corpus (`test.and`, `test.shl` in flag tests) |
| int <-> float | `pck.s16.f32`, `pck.f32.s16`, the u16/s8/u8 variants | encoder, corpus (s16); hwt_p4: f32 -> s16 rounds to nearest-even |
| integer divide | none: frcp + multiply + fix-up | encoder (no form) |

Compiler work:

- Turn off `no_integers` for GLES3/GL3 shaders. Keep it for ES 1.00, which
  must stay float to match the DDK's pixels.
- Select NIR's integer ops to these forms. Let NIR lower what's missing:
  `nir_lower_idiv` for division, and 64-bit-free lowering.
- Booleans as 0/~0 integers, with `test` + predicates for control flow.

Unknowns, each an hwt test to write before relying on it:

1. **32-bit int <-> float.** pck has no 32-bit integer format. Is it two 16-bit
   halves plus `ima32`, or an efo/fpma path? Test `int(f)` for |f| > 65535 and
   negative values.
2. **ima32 semantics:** where the high word lands (an internal register),
   signed vs unsigned (h9), carry. Test a 32×32 multiply and an add with carry.
3. **Integer compares:** the `test.<int op>.<cond>` condition codes on
   negative and wrapped values.
4. **The `partial` bit** of the bitwise ops (h2, "16-bit/partial" is a
   guess). Leave it unset until tested.

### texelFetch and textureSize

- `smp` takes F32, F16 or C10 coordinates (cfmt). No integer coordinates are
  known. texelFetch can be lowered to a normalized coordinate,
  `(i + 0.5) / size`, with `.lod` replace (`smp ... [lod]` encoder; `.lod`
  exists, `tex_lod` corpus). That needs the size.
- `smp` has return modes `rdata` (h14) and `rinfo` (h15). The names are
  guesses. If `rinfo` returns texture information such as the size, it is
  textureSize. **hwt:** sample a known texture with `.rinfo` and `.rdata` and
  read `o0`.
- Fallback that needs no new hardware facts: the driver uploads each bound
  texture's size per level as constants (Mesa's `nir_lower_tex` options plus
  a driver-supplied size uniform, as several gallium drivers do).
- Integer textures: the sample block unpacks raw texels itself (`pck.f32.u8`,
  `pck.f16.u8`). For R8UI and similar formats, unpack as unsigned with no
  `scale`. Wider integer formats depend on what TEX.md finds for the format
  table.

### UBOs

- Memory loads exist: `ld.<a|l|t>.<b|w|d> dst, base, off[, range]`, with
  repeat and MOE stepping, a DRC per load, and `wdf` to wait. Mode `a` is
  what every DDK secondary program uses to load uniforms from memory (`(p0)
  ld.a.d pa0, pa0, #0`, corpus). Mode `l` handles temp arrays and spills
  (corpus). Our compiler doesn't emit `ld` yet, so none of it has run in an
  A/B test.
- A UBO is then `ld.a.d` from a base address the driver puts in SA, with the
  offset immediate (0-127 registers) or in a register. Dynamic indexing uses
  `imae` (temp-array pattern in the corpus).
- An immediate offset counts dwords and reads base + 4·(i + 1) (ISA.md ld,
  "Address"). The compiler already uses this for constants that don't fit in
  SA (sgx_compiler.h `mem_dwords`).
- **Unknown, and needs hwt tests:** alignment, register offsets, offsets past
  #127, the `range` operand,
  whether loads see writes the CPU made since the last kick (data-cache
  control bits h1/h19 are guesses), and `ld.t`.
- Alternative for small UBOs: the driver copies them into SA as constants. That
  needs no new compiler code: lower UBO loads to uniform loads with
  `nir_lower_ubo_vec4` and a constant map, as GLES2 constants work today.
  It's limited by how many secondary attributes a program can address, minus
  the constants already there. ISA.md doesn't state that limit yet; an hwt
  test should establish it.

### Instancing

- The PDS vertex program receives the instance index as `in1`, and the
  instance divisor already exists (PDS.md: "`in0` is the vertex index and
  `in1` the instance index"; DOUTA of in0/in1; the confirmed divisor shift).
- gl_InstanceID and gl_VertexID are then a PDS change: DOUTA the index into
  a primary attribute. The compiler's part is mapping
  `load_instance_id`/`load_vertex_id` to that PA register, the way
  attributes map today.

### Other M7 items with a compiler side

- **MRT, float render targets, sRGB:** the fragment output goes to the PBE.
  Today it's one packed BGRA8 color in pa0, header 0x66. MRT and float
  formats need what the DDK never shows us in GLES2: several outputs, or
  wider ones (o registers, emit to the PBE: TEX.md, STATE.md). Mostly driver
  and state work. The compiler would write each output's register and format.
- **Transform feedback:** vertex outputs written to memory with `st`, or a
  hardware path behind the TA. Unknown. Windows exposed it (4 separate
  attribs × 4 components).
- **8 clip distances:** lower beyond the hardware's 6 planes in NIR.

## M8: GL 3.1 / 3.2

### Geometry shaders

- The `emit` form exists. libusc's geometry path is the only producer of
  0x101 (`emit.t1`, "geometry-shader vertex emit"). The f3 0x100 flag that
  sets h13 is libusc's geometry "emit and cut". Targets t0-t3, an `n`
  operand of 0-5, an `end` bit (ISA.md hw 1f, **encoder**).
- Unknown: the whole pipeline around it. Where a GS runs (VDM, the TA), how
  vertices reach it, what the targets mean, and the output buffer. The open
  microkernel and the TA state (STATE.md) would have to support it first.
- Oracles: the Windows ICD on a Cedarview machine (GL 3.2 is listed for this
  core by Imagination, but Intel's ICD stopped at 3.0, so perhaps not even
  that), or reaching libusc's geometry path via its intermediate form. Both are
  expensive. I'd put this last.

### The rest

- **TBOs:** a buffer as a 1D texture. Linear layout, so it's the texture path
  plus the driver.
- **Primitive restart:** CPU fallback.
- **Rect textures:** unnormalized coordinates lowered in NIR (`nir_lower_tex`).
- **Depth clamp, sync objects, multisample textures:** driver and state. The
  compiler is only involved for sample masks and sample IDs (unknown).

## What to do first, when M7 starts

1. hwt tests for the integer unknowns above: 32-bit conversion, ima32, integer
   test conditions. They're small and each unblocks a NIR op family.
2. `smp .rinfo` / `.rdata` on hardware: one test decides how texelFetch and
   textureSize get done.
3. `ld.a.d` from a driver-provided address on hardware: decides between real
   UBOs and SA copies.
4. Only then the compiler: integer isel behind a "GLSL 3" switch, keeping the
   ES 1.00 float path pixel-exact against the DDK.

## Status (2026-11-11)

The compiler side of GLSL 1.30 and 1.40 is in, behind
`sgx_compile_options.native_integers`, which the driver sets screen-wide
once it reports integers (GL3.md, "Crossing the driver/compiler line").
Without it nothing changes: GLSL ES 1.00 integers and booleans stay floats,
and the corpus output is byte-identical. GLSL 1.50 is not planned (GL3.md:
no geometry-shader path).

| GLSL feature | how | relies on |
|---|---|---|
| int/uint add, sub, negate, multiply | `ima32` (`a * #1 + -b`, `-a` as `a * c56`) | **hardware** (hwt_int) |
| `umulExtended`-style high words (division) | `ima32 [hi=i1]`, read by the next instruction | **hardware** (hwt_gaps, hwt_int: unsigned and `.sgn`) |
| bitwise ops, shifts | hw 0a-0d; constants as rotated 16-bit immediates; register counts masked to 0-31 | **hardware** (hwt_p7, hwt_int) |
| compares | `testmask.xor.eq/ne.dword`; lt/ge from a - b and the operands' signs (no overflow), then `testmask.or.neg/pos.dword` | **hardware** (hwt_int) |
| min, max, abs, sign, selects | `movc.ltz/z/nz.i32` | **hardware** (hwt_p2) |
| `/`, `%` | `nir_lower_idiv` (reciprocal, two fix-ups) | frcp exactly rounded on the five values hwt_int tried; the fix-up steps don't need it |
| int/uint ↔ float | the two 16-bit halves through `pck.f32.s16/u16` and one `fmad`; back by trunc, floor(\|t\| / 65536), `pck.u16.f32` | **hardware** (hwt_int; trunc and floor hwt_p4) |
| booleans | 0 / ~0 (`nir_lower_bool_to_int32`); an `if` tests the comparison into p0 (`test.xor` for ieq/ine) or `test.and.eq` on the mask | **hardware** (hwt_p13, hwt_int) |
| `switch` | GLSL's loop-once lowering, emitted without branches | — |
| `textureLod`, vertex texture reads | dependent `smp` with LOD replace (vertex: LOD c48) | **corpus** |
| `texelFetch` | `sgx_lower_tex`: txl at (i + 0.5) / max(size >> lod, 1), marked `sgx_sample_info.fetch` for NEAREST state of its own | — |
| `textureSize`, `textureGrad`, the Offset variants, rect, 1D | `sgx_lower_tex` (`nir_lower_tex`); txs is the driver's (`SGX_K_TEXSIZE`) | — |
| isampler / usampler | an RGBA8 texel's bytes, sign- or zero-extended | — |
| integer varyings | two sets per row, the 16-bit halves as exact floats (`sgx_varying.split`) | GL3.md: flat iteration only on the colour source |
| uniform blocks | `ld.a.d` from the address in constant `ubo_base_k + n`; word d at `#(d - 1)`, others through an `ima32` address | ld.a.d immediates: **hardware** (hwt_gaps) |
| gl_VertexID, gl_InstanceID | the driver's (an input it gathers, a constant) | — |

Checks: `tools/usc/usse_sim.py` models the integer and conversion forms the
compiler emits, with ISA.md's semantics, and `tools/usc/int_check.py` runs
43 GLSL 1.30 operations through sgxc and the model on random inputs; all
give exact results. `tools/isa/hwt_int.py` checked the semantics the model
assumes on a CE5300 board (2026-11-11): all 50 expected words, so every
form in the table is now **hardware** (log in `tools/isa/hwt_out/open/`).
What is not yet run on hardware is the compiled programs themselves
(piglit's GLSL 1.30 tests, once the driver exposes integers).

Not done here: integer and float textures wider than RGBA8 (the texel
formats are GL3.md's H2), MRT and packed render-target outputs (H1),
transform feedback (H4), texture arrays (GL3.md's layer-address path), and
shadow-compare and cube-array variants beyond what Mesa lowers.
