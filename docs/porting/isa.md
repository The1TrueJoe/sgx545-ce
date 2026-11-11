# Porting: the instruction set and the compiler

How to establish a core's shader instruction set and get the compiler generating code
for it. The SGX545's instruction set is in [ISA.md](../ISA.md); this page is the method
behind it and what changes for another core.

All methods here are black-box: run instructions on the hardware and read the results,
and compare against the output of a working driver. Never read the code of a
proprietary driver or compiler; a compiled shader it produces is fine as an oracle, its
code is not. [CLEANROOM.md](../CLEANROOM.md) has the rules for how docs and shipping
code are kept apart.

## What you need first

- **A way to run one instruction and read its result.** This is the instrument
  everything else depends on. Ours is [`tools/isa/hwt.py`](../../tools/isa/hwt.py):
  - A hand-assembled fragment program is poked into a frame that draws one triangle.
  - The program ends `mov o0, rN [end]`. The pixel back end stores `o0` unchanged in a
    BGRA8888 buffer, so every pixel inside the triangle is the 32-bit register.
  - Inputs come from `limm`.
  - The clear color is chosen so a program that never ran is obvious.
  - Suites are lists of `(name, program, expected word)`, e.g.
    [`hwt_p13.py`](../../tools/isa/hwt_p13.py). [`hwt_lib.py`](../../tools/isa/hwt_lib.py)
    has the load and readout helpers.
- **The frame to poke into.** You need any frame that runs a pixel program you can
  replace, its code address, and its PDS `DOUTU` (to give the program temporaries). On
  the SGX545 this was a captured frame replayed on the box. Once the open stack runs,
  [`sgx_hwt`](../../mesa/src/gallium/winsys/sgx/drm/sgx_hwt.c) does it without
  one: it assembles each test with Mesa's isa library, draws it through the winsys
  and reads the pixel back. [`tools/isa/hwt_open.py`](../../tools/isa/hwt_open.py)
  runs the same suites through it. On the SGX545 it reproduced all 650 values of the
  replay rig exactly.
- **Shader binaries from a working driver**, if one runs on your hardware. Compile
  one-feature shaders, capture the program words the driver uploads, and diff pairs
  that differ in one thing (a register, an operator, a swizzle, a precision qualifier).
  This is the "corpus" evidence in ISA.md.

Tag every fact with how it was found, as ISA.md does ("How each fact was
established"). A fact seen only in a capture says what the field *is*; what it *does*
comes from a hardware test.

## 1. A USSE core: confirm the SGX545 encodings

The SGX530, SGX535, SGX540 and SGX545 share the USSE instruction set. Start from
ISA.md and test it; don't assume it.

1. **The readout path.** Run [`hwt_sanity`](../../tools/isa/hwt_sanity.py) first:
   `limm`, a move to `o0`, a hardware constant, a high temporary. Until this is exact,
   no other result means anything.
2. **One suite per area.** Port the suites in ISA.md "Hardware checks" table by table:
   float modifiers and repeats, moves and tests, transcendentals, F16/C10, pack,
   integer classes, efo and fdual. Each confirms or corrects one ISA.md section. Keep
   each run's log (ours are `tools/isa/hwt_out/<suite>.txt`).
3. **Captures, if the core's working driver runs.** Decode every captured word with
   your disassembler, built from ISA.md. A word that matches no form, or has a bit no
   field explains, is a difference to chase. Text must re-encode to the same 64 bits.
4. **Write the differences down.** Record them as per-core notes in ISA.md's sections,
   with their evidence. Then add them to the instruction-set description (see "The
   compiler side").

What to check per core, roughly in the order it bites:

| Area | What may differ | Where ISA.md has the SGX545's answer |
|---|---|---|
| Register files | temporaries, PA, SA and output counts; internal registers i0-i2; which banks need an extension bit | "Registers", "Operand slots and bank codes", "Register files and operand limits" |
| Hardware constants | the `c<n>` table (c46/c47, c52, c57-c63 on the SGX545) | "Hardware constants" |
| Whole classes | fmad16, efo, fdual, the integer and multi-unit ops, F16/C10 format select and control | "Instruction classes" and each class |
| efo | the SGX545 uses h5/h4/h3 + ext for source modifiers; earlier cores use h3-8 as three 2-bit modifiers | "efo" |
| Texture sampling | `smp` forms and limits (write mask, repeat, PCF), PDS-driven samples, return formats | "smp", "The texture-sample sequence a compiler must emit", [STATE.md](../STATE.md) §5 |
| Same-word timing | which reads of a register written by the same word race | "efo", "fdual for a compiler" |
| Flow and phases | branch encoding, the visibility feedback and discard | "flow control", "Phases, visibility feedback and discard" |
| PDS | the PDS program words that feed the USSE | [PDS.md](../PDS.md) |

## 2. A USSE2 core: establish the encoding from scratch

The SGX543, SGX544 and SGX554 use USSE2, a different encoding. This project has
established nothing about it. Treat everything, the 64-bit word included, as a
hypothesis until a hardware test says otherwise. The overall model is likely to carry
over (banks, PDS-fed attributes, predicates, repeats); the bits are not.

1. **Bring up the instrument.** You need a frame you can replay and a pixel program you
   can replace (see "What you need first").
   - Find the smallest working program: capture what a working driver emits for
     `gl_FragColor = vec4(c)`, and find its end-of-program marker and its move to the
     output register.
   - Replace the program with that minimal one and check the pixels change as
     expected.
2. **A corpus of one-feature shaders.** Generate pairs that differ in one thing: a
   register number, a constant, an operator, a swizzle, a write mask, a precision
   qualifier. Compile them with the working driver and capture the words. Diffs give
   field positions: which bits move when the register number moves.
3. **Name every bit.**
   - For each class, hypothesize the opcode bits, the operand slots (number, bank,
     extension) and the flags.
   - Write a decoder that lists every bit it can't explain, and drive that list to
     empty over the whole corpus.
   - Write an assembler from the same tables, and require text → words → text to round
     trip.
4. **Confirm on hardware.** For every field the corpus showed, vary it in a
   hand-assembled program and read the result. A field's meaning is established only
   here. Test the modes a compiler will lean on:
   - modifiers;
   - repeats and their increments;
   - formats (F16 operand numbering, conversions, rounding);
   - same-word reads of registers being written;
   - the sample return layouts.
5. **Write the spec in ISA.md's shape.**
   - Encoding conventions first: bit names `l<n>`/`h<n>`, bank codes, the number field,
     unused slots, the text notation and its defaults.
   - Then one section per class, each field with its bit range and evidence tag.
   - Open questions at the end of each section.
6. **Validate the spec blind.** Someone who has read only the doc writes an encoder;
   score it against words from the corpus and from hardware tests. The shape of this
   check is [`tools/isa/blind.py`](../../tools/isa/blind.py). Every wrong word is a doc
   fix. Our two passes went from 120/122 to 120/120 words exact.
7. **Write the isaspec description** from the doc (see below).

## 3. The compiler side

The compiler is [`mesa/src/sgx/compiler`](../../mesa/src/sgx/compiler/README.md) (NIR to
USSE). It encodes through the isaspec description in
[`mesa/src/sgx/isa`](../../mesa/src/sgx/isa/README.md). A new core touches these parts.

- **Instruction-set description.**
  - A USSE core: per-core differences in `sgx.xml`, plus a feature list for classes the
    core lacks.
  - A USSE2 core: its own XML under the same tooling.
  - The forms the compiler emits are templates in `sgx_forms.c`; each is backed by a
    line in `isa/tests/*.txt` that must round-trip.
  - ISA README "What isaspec can't express" covers the modal state (format control,
    MOE) and the isaspec patch.
- **Per-core options.** The driver passes the core's facts in
  `struct sgx_compile_options`, filled from the Mesa driver's per-core table. Facts
  that differ per core:
  - temporaries (124 on the SGX545), PA (128) and the SA budget after the driver's
    reserved registers;
  - operand number widths and internal registers;
  - iteration and sample limits;
  - F16 and C10 support (`f16`), efo and fdual availability;
  - PDS-only samples (`pds_samples_alone`) and F16 sample returns (`f16_samples`);
  - texture channel order (`tex_bgra`);
  - the front-facing source (`front_sa`).

  Keep tunables (unroll limits, peephole limits) apart from hardware facts.
- **NIR lowering** (`sgx_nir.c`): lower what the core lacks, in NIR, with Mesa's
  passes where they exist:
  - integer and boolean math as floats if the core computes GLSL ES 1.0 `int` that
    way;
  - sin/cos range reduction to the unit's argument scale (quarter turns on the
    SGX545);
  - mediump demotion only where F16 forms exist;
  - the I/O and uniform numbering the core's PDS and constant loads use.
- **Register allocation** (`allocate()`, Mesa's `util/ra`):
  - register classes from the temporaries count;
  - placement straight into PA and output registers where the PDS and pixel back end
    allow it;
  - F16 values as halves, two per register;
  - internal registers kept out of the allocatable set.
- **Co-issue and scheduling** (`isa/sgx_isa_coissue.c`):
  - which same-word reads race, and which see the old value;
  - which instructions must be `nosched` to keep internal registers live (efo chains);
  - what helper instances must run for derivatives (skipinv clear).

  Measure each rule on the new core with a hardware test before the scheduler relies
  on it. hwt_p12's same-word timing tests are the model.
- **Checks**, each one passing before the next:
  1. `meson test --suite sgx`: the ISA round trip (`sgx_isa`) and the end-to-end gtests
     (`sgx_compiler`, shaders built with `nir_builder`, every word decoded).
  2. A corpus run ([`tools/usc/run_corpus.py`](../../tools/usc/run_corpus.py) over the
     corpus files): every emitted word decodes with no unknown bit and round-trips.
     Instruction and temporary counts should not regress
     ([`tools/usc/count.py`](../../tools/usc/count.py)).
  3. A hardware test for every form the compiler newly emits, before relying on it.
  4. dEQP-GLES2 on the hardware, then real applications
     ([PERF.md](../PERF.md), [PIGLIT.md](../PIGLIT.md)).

  [COMPILER-UPSTREAM.md](../COMPILER-UPSTREAM.md) §4 explains the XML choice, and
  [COMPILER-GL3.md](../COMPILER-GL3.md) what comes after GLES2.

## 4. Traps we hit

On the SGX545. Each is worth a hardware test on any new core.

- `efo d=i0` in a word that writes I0 (`wi0`) races: some pixels get the old value,
  some the new. ([ISA.md "efo"](../ISA.md))
- `efo d=i1` with `wi1` reads the old I1, consistently. Chains rely on it: the next
  chain's first efo stores the previous chain's result, and the previous chain's last
  efo is `nosched`. ([ISA.md "efo"](../ISA.md))
- efo has no end bit, so it can't be a program's last instruction.
  ([ISA.md "efo"](../ISA.md))
- An fdual primary read of a register the same word writes races.
  ([ISA.md "fdual for a compiler"](../ISA.md))
- F16 operand numbers count halves (float classes and `test` under format select,
  efo under format control); C10 operands are whole registers.
  ([ISA.md "Hardware checks"](../ISA.md))
- An assembler that silently drops a format suffix makes F16/C10 tests measure plain
  f32: reject what can't be encoded, then rerun the tests.
  ([ISA.md "Hardware checks"](../ISA.md))
- `setfc`'s first argument enables efo F16 (off at start), its second C10 (on at
  start). ([ISA.md "Hardware checks"](../ISA.md))
- f32 → f16 conversion truncates; fmad16 rounds to nearest; fmad rounds the product
  (not fused). ([ISA.md "Hardware checks"](../ISA.md))
- sin/cos take the argument in quarter turns. ([ISA.md "Hardware checks"](../ISA.md))
- `fdp` writes only the last iteration's destination.
  ([ISA.md "Hardware checks"](../ISA.md))
- `movc` with u8/i16/c10 tests per byte, half or channel; an f32 denormal tests as 0.
  ([ISA.md "Hardware checks"](../ISA.md))
- C10 is clamp(v/255, ±2), and C10 alpha reads channel 0.
  ([ISA.md "Hardware checks"](../ISA.md))
- `smp` takes write mask 1 and no repeat. ([ISA.md "smp"](../ISA.md))
- PDS iterations fill PA in program order: dropping an iteration means renumbering
  the rest, never a hole. ([PDS.md "DOUTI"](../PDS.md))
- An F16 sample return is two registers, R lo/G hi then B lo/A hi, in RGBA order even
  on a unit whose 8-bit texels arrive with red and blue swapped.
  (`sgx_sample_info.f16` in
  [`sgx_compiler.h`](../../mesa/src/sgx/compiler/sgx_compiler.h);
  [STATE.md §5](../STATE.md))
- The T3 sample word differs per sample kind (00800800 vs 01028800); untested kinds are
  listed as open. ([STATE.md §5](../STATE.md),
  [CLEANROOM-COVERAGE.md](../CLEANROOM-COVERAGE.md))
- Whatever feeds a derivative or a dependent sample's LOD must run on helper instances
  (skipinv clear). ([ISA.md "Program structure and calling conventions"](../ISA.md))
- A discard splits the fragment program into phases at the visibility feedback.
  ([ISA.md "Phases, visibility feedback and discard"](../ISA.md))
- Internal registers in slots without an extension bit encode as r124 + n; keep r124 and
  up out of the allocator. ([isa README](../../mesa/src/sgx/isa/README.md))
- Several isaspec `<override>`s on one field lost a flag in the generated encoder
  until patched. Round-trip every form after an isaspec change.
  ([isa README](../../mesa/src/sgx/isa/README.md))
