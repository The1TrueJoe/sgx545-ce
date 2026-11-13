# SGX545 shader compiler

NIR in, USSE code out. The compiler handles GLES2 vertex and fragment
programs for the SGX545 (Intel GMA 600/3600/3650, Atom CE5300). It emits
words through the isaspec encoder in `../isa`, and it fills
`struct sgx_shader` with everything the driver needs to run the program:
constants, PDS iterations, texture samples, branches, register counts.

| file | what |
|---|---|
| `sgx_compiler.h` | the API: `sgx_compile()`, `struct sgx_compile_options`, `struct sgx_shader`, the varying-set assignment |
| `sgx_nir.c` | NIR lowering (`sgx_lower()`): I/O and uniforms to the hardware's numbering, mediump to F16, integers and booleans to floats, the optimization loops |
| `sgx_compile.c` | instruction selection, the backend passes, register allocation, encoding |
| `sgx_program.c` | the finished program: flat code with branch offsets resolved, the block list, samples, the constant and iteration lists |
| `sgx_forms.c`, `sgx_forms.h` | the instruction forms the compiler emits, as `struct sgx_isa_instr` templates for the encoder |
| `sgx_fixed.c`, `sgx_fixed.h` | the fixed programs a frame needs besides the shaders (state emit, clear, pixel events, ...), as assembly text assembled through `../isa` when a context is created |
| `sgx_priv.h` | the backend IR (`sgx_ins`, `sgx_ref`, vregs) and the compile context |
| `tests/sgx_compile_test.cpp` | end-to-end tests: shaders built with `nir_builder`, compiled, every word decoded |

## The pipeline

1. **Lowering** (`sgx_nir.c`, `sgx_lower()`):
   - assigns I/O and uniform locations, from the caller's `sgx_layout` or the
     compiler's own numbering;
   - with `f16`, demotes mediump fragment arithmetic to F16 and narrows
     F16-free values;
   - runs the general optimization loop;
   - lowers integers and booleans to floats, the way GLSL ES 1.0 `int` and
     `bool` are computed;
   - leaves the shader out of SSA, with vectors in registers.
2. **Analysis**: finds the patterns that need one specific emission, each
   recorded before selection:
   - `find_efo`: vector-times-scalar sums, co-issued as efo;
   - `find_projected`: projected texture reads that the PDS iterates;
   - `find_discard`: the single discard comparison;
   - `find_c10`: lowp colour expressions computed in C10;
   - `find_shadow_regs`: 16-bit registers that are only assembled, then read.
3. **Selection** (`emit_cf_list()`): one `sgx_ins` per USSE instruction over
   virtual registers. A vector op becomes one instruction with a repeat
   where its operands step together. F16 values are halves, two per
   register.
4. **Backend passes**, in order:
   - `merge_runs`: adjacent instructions become one repeat;
   - `fold_movs`: copies are folded into the instruction that wrote the value;
   - constants that don't fit in SA are loaded from memory;
   - `mark_helpers`: whatever feeds a derivative or a dependent sample's LOD
     runs on helper instances too (skipinv clear);
   - `efo_runs`: keeps efo runs free of deschedules.
5. **Register allocation** (`allocate()`): liveness, placement straight into
   PA and vertex output registers, then Mesa's `register_allocate` for the
   rest. Finally `drop_self_moves()` removes moves of a register to itself.
6. **Encoding** (`sgx_finish_program()`): each instruction is legalized
   (constants to `c<n>`, `limm` or literal slots), encoded, and checked
   against the co-issue hazards.

## Debug options

`SGX_COMPILER_DEBUG` takes a comma-separated list. Each flag turns off
one optimization, so a miscompile can be bisected without rebuilding.

| flag | turns off |
|---|---|
| `nopa` | allocating values straight into PA registers |
| `noout` | computing vertex outputs in their output registers |
| `nocoalesce` | repeats writing over the source they read |
| `nosplit` | splitting vector registers per component |
| `nofold` | folding copies into the defining instruction |
| `noselfmov` | dropping moves of a register to itself |
| `nolit32` | 32-bit literals from driver constant slots (uses `limm` instead) |
| `nonarrow16` | narrowing F16 arithmetic over converted values |
| `nomediump` | fragment mediump in F16 (fixed-function programs excepted) |
| `nominmax32` | F16 min/max/saturate of F32 values computed in F32 |
| `nocondmask` | selects reading a comparison's test mask |
| `nocsel1`, `nocsel32` | F16 select shortcuts |
| `nocoissue` | efo co-issue |
| `nof16const` | mediump uniforms loaded as F16 (loads F32 and converts) |
| `nof16iter` | F16 varying iteration |
| `nomaskbool` | or/and of comparisons kept as test masks (computes them as 1.0/0.0 floats instead) |
| `noiftest` | testing an if's comparison straight into the predicate (branches on its value instead) |
| `keepcoorditers` | dropping a sampled-only varying's plain iteration (keeps it) |
| `noefochain` | storing a chain's last lane with the next efo chain's first instruction (keeps each chain's `mov oN, i1`) |
| `nof16samples` | mediump texels returned as F16 by the sampler (unpacks RGBA8 instead) |
| `nophases` | splitting a discarding fragment program at its visibility feedback (one phase instead) |
| `flattenuniform` | ifs on a uniform condition flattened into selects (by default they stay branches) |
| `nointerleave` | each PDS-only sample placed after a plain iteration of another set, for the driver's DOUTI merge |
| `nodepf16` | F16 returns on dependent samples (when the driver sets `dep_f16_samples`) |
| `noprojalone` | dropping the plain iteration of a varying only projected (TXP) samples read |
| `noconstswap` | `1 * k - x` in F16 as `x * -1 + k` (no `limm` for src0) |
| `unrollexits` | keeping loops that can exit early as loops (unrolls them into select trees instead) |
| `notexcolor` | writing a texel that is the color with byte moves (unpacks and repacks it instead) |
| `nopackedcolor` | a uniform color packed to BGRA8 by the driver (packs it in the shader instead) |

`sgx_compile_options.verbose` prints the lowered NIR, the uniform layout and
the code words to stderr. The gallium driver sets it from its own debug
option.

## Tests

`meson test --suite sgx` runs these tests and the ISA round-trip tests.
`tests/sgx_compile_test.cpp` builds each case with `nir_builder` (47 cases:
selection, F16 and C10, textures, discard and phases, constants in memory,
helper instances, branches).
