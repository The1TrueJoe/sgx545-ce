# The shader compiler: upstream readiness (audit 2026-11-01, updated 2026-11-11)

This checks `mesa/src/sgx/compiler` against AGENTS.md §3.5-§3.7: licensing, what the
compiler's knowledge rests on, and Mesa's expectations. It also says what has to
change before the code is offered to Mesa.

The compiler is 7 files, about 10k lines:
- `sgx_nir.c`: NIR lowering;
- `sgx_compile.c`: instruction selection, register allocation, encoding;
- `sgx_program.c`: the finished program;
- `sgx_forms.c` and `sgx_forms.h`: the forms it emits;
- `sgx_compiler.h` and `sgx_priv.h`.

It encodes through `mesa/src/sgx/isa` (isaspec). The tools that test it
(`tools/usc`, `tools/isa`) stay in this repo and are not part of what goes to Mesa.

## 1. Licensing (§3.5): clean

- **Includes:** only Mesa (`nir*.h`, `util/*`, `compiler/*`) and the compiler's
  own headers. Nothing from `kernel/src`, `libpvr` or `libpvr/abi` (every
  `#include` checked).
- **No GPL material:** the compiler knows nothing about services, bridge
  ioctls or kernel structs. Its inputs are NIR and its outputs are instruction
  words, a constant map and an iteration list. No struct layout, table or text
  from `kernel/src` appears in it.
- **No proprietary binaries, and nothing derived from them,** are in `mesa/`. The
  DDK corpus USPs (`tools/isa/corpus`, compiler output from the DDK) and the hwt
  logs stay in `tools/` and must not go to Mesa.
- **Headers:** every file carries `Copyright © 2026 Joseph Telaak` and
  `SPDX-License-Identifier: MIT` (Joseph's choice, 2026-11-01).

## 2. What the compiler rests on, and how it is checked

The compiler depends on three kinds of knowledge: instruction encodings, what to emit
for each GLSL feature, and what the hardware does with it. Each is written down in
`docs/` with its evidence (ISA.md "How each fact is established") and checked as
below. docs/CLEANROOM.md says how docs and code are kept apart.

### 2.1 Instruction encodings

The compiler fills a `struct sgx_isa_instr` per instruction (`sgx_forms.c`) and encodes
it with the isaspec encoder generated from `mesa/src/sgx/isa/sgx.xml`. `sgx.xml` is
written from docs/ISA.md. The field positions it uses are **oracle** facts there:
encoded through the vendor's assembler as a black box, swept over every field value,
and decoded with no unexplained bit.

Checks, all repeatable:

| check | what it shows | where |
|---|---|---|
| ISA round trip | text → struct → words → text for every form a GLES2 compiler emits, the integer classes and the co-issue spellings; expected words where a line gives them | `meson test --suite sgx` (`sgx_isa`, `mesa/src/sgx/isa/tests`) |
| corpus encode | every DDK corpus word (4154) re-encodes from its text to the same 64 bits | `tools/usc/check_enc.py` |
| emitted words | every word sgxc emits decodes with no unknown bit and round-trips | `tools/usc/check_out.py --no-oracle`, run_corpus |
| blind encoder | an encoder written from ISA.md alone matches random valid words (120/120) | `tools/isa/blind.py` |
| hardware: meaning | F16 numbering, rounding, C10, setfc, repeats, swizzles, efo and fdual units, same-word races (13 suites) | `tools/isa/hwt_*`, ISA.md "Hardware checks" |
| hardware: programs | the compiler's programs give the DDK's pixels in the A/B suites, and the open stack passes dEQP-GLES2 on the EA3 (16,637 pass, 0 unexpected, 2026-11-09) | `tools/usc/hw`, docs/PIGLIT.md |

A wrong field value in a form the compiler emits would show up as a wrong pixel or a
GPU fault in the A/B and dEQP runs. Per form:

| form (compiler use) | our words run in A/B, pixel-exact | DDK corpus words | hwt |
|---|---|---|---|
| pck.u8.f32 | 366 | 500 | p4 (u8 rounding) |
| mov | 247 | 454 | sanity |
| fmad | 174 | 338 | p1, p7 |
| fmad16 | 84 | 246 | p4, p7 |
| pck.u8.f16 | 66 | 197 | |
| pck.f32.u8 | 52 | 0 | p4 (unpack lanes) |
| testmask | 40 | 32 | p2, p7 |
| movc (f32, i32) | 51 | 65 | p2 |
| fsubflr | 39 | 211 | p1 |
| limm | 33 | 0 | sanity |
| pck.c10.c10 | 27 | 55 | |
| frcp, frsq, flog2, fexp2, fsqrt, fsin | 70 | 124 | p3 |
| fdp | 22 | 6 | p7 |
| pck.f16.f16 | 22 | 136 | |
| pck.f16.c10 | 20 | 12 | c10 (tests/c10: rounding) |
| pck.f16.u8 | 20 | 0 | |
| fminmax, fmaxmin | 17 | 18 | p4 |
| sopwm, sop3l, sop3 | 14 | 43 | p6 (sop2 family) |
| test | 7 | 229 | p2, p10 |
| smp | 6 | 37 | |
| pck.s16.f32, ima16 | 12 | 16 | p4 (s16) |
| br, nop | 11 | 44 | p2, ba |
| pck.c10.f32, pck.f16.f32 | 4 | 72 | p4 (f32 -> f16 truncates) |

(Counts from `docs/data/compiler-provenance.txt`. The hwt column names suites
whose tests exercise the form. It was compiled by hand from ISA.md "Hardware
checks", so it's indicative, not exhaustive.)

**Gaps:** these forms can be emitted, but the A/B suites don't run them yet (dEQP-GLES2 may).

- `fcos`: hwt_p3 confirms its value on hardware. No A/B shader uses cos yet.
- `fadm`, `fmsa`: hwt_p1/p5 confirm them. Selected only for some fmad shapes.
- `fdsx`, `fdsy` (derivatives): ours match the DDK's words for corpus c3
  dsx/dsy. piglit on a CE5300 board showed that their sources must run on helper
  instances (skipinv clear) and that the destination must not share the
  source's register. Retest after the register fix; they also need an hwt
  test before upstream.
- `depthf` (gl_FragDepth): the driver writes it at the feedback point; what
  it does with our depth is a guess until a hardware test.
- `pck.f32.f16`: the conversion is hwt_p4. No A/B shader takes this path.

### 2.2 What to emit for each GLSL feature

The iteration layout, discard, sample blocks, C10 for lowp, F16 for mediump and the
rounding choices are measured against the DDK's compiler used as a black-box reference.
That reference is the USPs it produces for one-feature shaders (the `gen*.py` corpora)
and pixel A/B on the same draws. The results are written in docs/ISA.md "What a
compiler needs" and docs/STATE.md, and the compiler follows those docs.

Comments in the compiler still name the vendor compiler as the behavior matched
("as libusc fuses a mediump a*b+c"): 30 such comments in `sgx_compile.c`,
`sgx_nir.c` and `sgx_compiler.h` (2026-11-11). No vendor function address is left in
`mesa/`. For upstream, each should cite the docs/ section and the test that fixed the
behavior instead: reviewers would read "as libusc does" as "copied from libusc", even
though it means "matched against it".

### 2.3 Hardware semantics

F16 numbering, rounding, C10 range and rounding, setfc state, repeat stepping, and
the internal registers' loss on a deschedule. The last was found by the A/B runs and is
now handled with `nosched`. All come from `hwt_*` and A/B runs, logged in `docs/log/`.

### 2.4 What is left before upstream

1. Done (2026-11-03): the Mesa-side ISA description is written from docs/ISA.md and
   validated black-box. `hw.py` stays in `tools/` as a test oracle; none of it goes
   to Mesa.
2. Close the gaps in 2.1 with hwt tests, derivatives first.
3. Confirm on hardware the ISA.md gaps a compiler relies on: skipinv, syncs and nosched
   (ISA.md "Gaps").
4. Rewrite the compiler comments of 2.2 to cite docs and tests.

## 3. DDK-compatibility code that doesn't go upstream

Some of the compiler exists so the DDK's GLES driver can run our programs,
which is what makes the A/B harness possible. An upstream driver doesn't need it:

- **`sgx_usp.c`, the USP container. Done (2026-11-01):** the compiler now
  publishes the program's structure in `struct sgx_shader` (flat code, blocks:
  code runs, labels, branches, the discard feedback point, samples; dependent
  samples with their smp template; SA slots). `tools/usc/sgx_usp.c` builds the
  USP from that for sgxc, and the gallium driver reads the fields instead of
  parsing a USP (`SGX_DUMP_USP` became `SGX_DUMP_CODE`). Every corpus
  and test set compiles to byte-identical USPs and code before and after
  (6841 files, F32 and F16).
- **glslcompiler's numbering.** This covers the pins, the varying-to-set
  allocation in `sgx_assign_varyings`, sampler numbering by first use, the
  constant numbers measured by `calib.py`, and the USP label numbers 3/4/5.
  All of it matches glslcompiler so that our USPs drop into the DDK's driver.
  Upstream, the driver decides layouts. **The pins are done (2026-11-01):** the
  compiler takes a `struct sgx_layout` (place a name, first free number) from
  its caller; the name tables and glslcompiler's numbers live in sgxc, and the
  gallium driver answers with st's `driver_location` for attributes. Still in
  the compiler: the varying-set allocation and first-use sampler order (the
  compiler's defaults, which match glslcompiler), and the n_pa count of the
  DDK driver's placeholder color iteration.
- **The list A dummy color iteration** is USP-only (written by
  `tools/usc/sgx_usp.c`).
- **Ours, and staying:** the driver-only constant kinds 0x10 (F16 of a C10
  value) and 0x11 (literals) are our own interface, not the DDK's. Express them
  in `sgx_const_map`; the list C records exist only for the harness.

## 4. ISA definitions as XML (evaluated)

The candidate is **Mesa's `isaspec`** (`src/compiler/isaspec`, used by
freedreno's ir3 and etnaviv), not genxml or csbgen. isaspec describes
instruction encodings and generates the encoder and the decoder, so Mesa also
gets a disassembler. genxml and csbgen describe state packets, the right tool
for the driver's PDS and control-stream words (STATE.md, PDS.md), not for
USSE.

Size and fit:

- 174 forms (42 used), 1611 field instances, 520 operand slots.
- **60 fields are split** across non-adjacent bits: the sop2/sop3 color and
  alpha selects, sopwm's write mask (`h12,h13,h14,h11`), fmad16's swizzles
  (`h13,h18`), the movc and ima8 condition bits. isaspec fields are contiguous
  ranges. A split field becomes one `<field>` per range, an `<encode><map>`
  expression for each part, and a `<derived>` field that reassembles the value
  for display. That's mechanical.
- **Operands** (bank + extension bit + number, the format bit that makes
  numbers 6-bit under format control, F16 halves) map to isaspec types per
  slot kind.
- **Modal state:** format control (`setfc`) changes how numbers are read. The
  encoder takes it as a parameter (`struct sgx_isa_fc`), and the disassembler tracks
  `setfc` from the program-start state (mesa/src/sgx/isa/README.md).

Done (2026-11-03), in this order:

1. Write the forms the compiler uses in isaspec XML from docs/ISA.md.
2. Generate the encoder at build time (meson `custom_target`). No generated
   file is committed, which is Mesa's convention.
3. Encode through `struct sgx_isa_instr` (`sgx_forms.c`).
4. Check that the generated encoder and the old tables gave the same words on every
   word the compiler emits in the suites, and on the corpus. They did, and the old
   encoder is deleted.

## 5. Other Mesa expectations (§3.7)

Status: docs/UPSTREAM-READINESS.md, "Compiler" (C1-C9), tracks these now.

- **Style:** C11, Mesa formatting, no global state (`hw_const`'s static table
  should become a const table).
- **Debug options:** `opts.verbose`, `SGX_PRINT_PRE_I2F` and sgxc's `-v`
  belong in the driver's debug-option table (`SGX_DEBUG=nir,...`), not in
  environment variables read by the compiler.
- **Tests in Mesa:** an encoder round-trip unit test (meson `test`) over
  programs the compiler itself emits (not DDK corpus words), plus dEQP-GLES2
  through the driver. The DDK corpus and the oracle tools stay here.
- **Placement and naming:** `src/gallium/drivers/sgx` with the compiler in
  it, or `src/imagination/sgx/...`. This is AGENTS.md §7.2, for the Rogue
  maintainers. The compiler has no dependency on that choice.
