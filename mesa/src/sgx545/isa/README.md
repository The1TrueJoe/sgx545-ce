# SGX545 USSE instruction set (isaspec)

`sgx545.xml` describes the SGX545's 64-bit USSE instructions in Mesa's
isaspec format, written from `docs/ISA.md`. isaspec generates the encoder
and the disassembler from it; the C files here are the glue and a small
text assembler.

| file | what |
|---|---|
| `sgx545.xml` | the instruction set: one bitset hierarchy per class (h27-31), operand hierarchies per slot |
| `sgx545_isa.h` | the API: `struct sgx_isa_instr`, `sgx545_isa_encode()`, `sgx545_disasm()` |
| `sgx545_isa_encode.c` | leaf selection for the generated encoder, the helpers the XML's `<map>`s call |
| `sgx545_isa_disasm.c` | the generated decoder plus custom fields, setfc tracking, bracket tidying |
| `sgx545_isa_asm.c`, `sgx545_isa_asm_int.c` | ISA.md's text notation into `struct sgx_isa_instr` |
| `sgx545_isa_enums.py` | generates `sgx545-isa-enums.h`: the opcode and operand-kind enums the encoder switches on, and the field-value enums |
| `tests/sgx545_isa_test.c` | text -> struct -> words -> text round trip; checks expected words where a line gives them |
| `tests/*.txt` | the test lists (`gles2.txt`: the forms a GLES2 compiler emits; `int.txt`: the integer, fixed-point and dual-issue classes) |
| `tests/standalone.sh` | builds the library and the test against an unpacked Mesa tree, without a Mesa build |

`mesa/patches/0004-isaspec-derived-bitset-params-and-override-fields.patch`
is the isaspec change this needs (see below).

## How the XML is laid out

- Bit n < 32 is ISA.md's `l<n>`, bit 32 + n is `h<n>`.
- Each class is a bitset extending `#instruction` with its opcode pattern on
  bits 59-63; sub-ops are leaf patterns.
- **Operands.** A register operand's number, bank and extension bits are
  spread over the word (dst: l21-27, h0-1, h19). Each slot is one field per
  piece plus a `<derived>` field that joins them (`NUM | BANK << 7 | EXT <<
  9`) and is typed with an operand hierarchy (`#dst`, `#src`, `#src0`,
  `#dst1`) whose leaf patterns are ISA.md's bank tables. A `MODE` parameter
  tells the operand how its number reads (plain, format select F16/C10,
  format control C10/F16): F16 numbers count halves (`r3.f16.hi`), C10
  numbers are flagged whole registers (`pa4.c10`), indexed operands shrink
  their offset to 3 bits under format select.
- **Split fields** (`h6-8, h24`, limm's three-part immediate, fmad16's
  src1 swizzle) are one field per range and a `<derived>` field for display.
  The encoder fills each range from its own `<map>`.
- **Defaults in the text** ("a field the text doesn't show holds its
  default") are enums whose default value displays as nothing, or `custom`
  fields printed by `sgx545_disasm.c` only when they differ.

## What isaspec can't express, and what is done instead

- **Fields made of several bit ranges.** One field per range plus a
  `<derived>` field. isaspec decodes a derived field of bitset type, but its
  Python didn't accept `<param>`s on one, and the encoder's parameter struct
  didn't know their names: patch 0004, part 1.
- **Several `<override>`s defining the same field.** The generated encoder
  left a dangling `else` that swallowed the next field (it lost the F16 flag
  of an indexed operand under format select). Patch 0004, part 2: the
  definition in the case being encoded wins.
- **Modal state outside the word.**
  - Format control (`setfc`): the encoder takes it as `struct
    sgx545_isa_fc` and hands it to the XML's expressions as `ISA_GPU_ID()`
    (bit 0 = C10 enable, bit 1 = efo F16 enable); the disassembler tracks
    `setfc` in a straight line from the program-start state (it does not
    follow branches).
  - MOE state (`smlsi`, `smbo`, ...) and the repeat it drives: only the
    word's own fields (`moe`, `inc0-2`, `rpt`) are encoded. Which registers
    a repeated operand steps through is the compiler's business.
  - Dual issue: `fdual` is one 64-bit word, so it is one form with the
    primary and secondary ops as fields; nothing pairs two instructions.
- **"Any value but N" patterns** (movc's type 1-5 next to mov's 0;
  ssum16's format 0-2 next to ssum16i's 3): one leaf per value, with the
  value as a constant `<derived>` field for the display.
- **Internal registers in slots without an extension bit** become r(124 +
  n) in the encoder's glue (`SGX_DSTX`, `SGX_SRCX`, the src0 and 1-bit-bank
  leaf selection); the disassembler prints them as `r124`-`r127`.
- **Text-only details**: empty `[]`, the space before `[`, items printed
  only when non-default, hex for large immediates. isaspec strips leading
  spaces from field displays, so bracket items are space-separated in the
  XML and `sgx545_disasm()` collapses the runs.

## ISA.md gaps hit while writing this

Sent to the ISA.md owner. Answered in ISA.md (um-re d7f97e4, e77b9f9), and the
XML follows the answers:

1. Bits no field names are 0: hw 1 h22, hw 3/4 h10, h7 in the forms
   without src0, mov/movc h3-6, smp h19, testmask h2-3 and h6, the
   special class's unlisted bits ("Unused slots and fixed patterns").
2. nop and op0x98 have no predicate (h24-26 = 0); a prefix on a form
   without a predicate field is an error.
3. An `i<n>` operand under format select or efo's F16 control is
   flag-only (`i1.f16`).
4. Every slot that follows format select has 6-bit numbers whatever the
   ALU op (test with fmt on a C10 op included).
5. smp's mask and ret share h12-15; ret sets h14-15 over the mask.
6. The text notation ("Text notation and defaults"): `|x|`, `(pn)`,
   `rcnt rpt=n`, smp's suffix order, `drc=n`, decimal numbers, fmad16's
   `swz0-2=`, fdual's `sd`/`sb`/`sc`/`msrc` names, numbers for enum values.

Still open (asked with the integer table): efo's d/i/a/m value names, when
the sop selects are omitted, dot's src1/src2 complement names, the
undescribed bits of the integer classes, and whether efo's destination
follows the F16 format control.

## Switching the compiler over (not done yet)

`mesa/src/sgx545/compiler` encodes through `usse_forms.h` / `usse.c`, which
`tools/usc/gen_forms.py` generates. To move it onto this encoder:

1. Link `idep_sgx545_isa` into `libsgx545_compiler` (meson), and include
   `sgx545_isa.h` where the compiler emits instructions.
2. Replace each call that builds a word through `usse.c`'s per-form helpers
   with a filled `struct sgx_isa_instr` and `sgx545_isa_encode(&instr,
   &fc)`, where `fc` is the format-control state at that point (the
   compiler's default is `SGX545_ISA_FC_START`; it changes only after a
   `setfc` the compiler emits). Operands are given by slot (dst, src0,
   src1, src2) exactly as ISA.md names them; a one-source form uses src1.
3. Use `sgx545_disasm()` for the compiler's debug output and in place of any
   disassembly through the old tables.
4. Delete `usse_forms.h`, `usse.c` and `usse.h`, and `tools/usc/gen_forms.py`
   with them; `tools/isa/hw.py` stays in `tools/` as a test oracle only.
5. Run the dEQP-GLES2 suite before and after: the words must not change for
   any shader (a word-level A/B over the corpus is the quickest check).

Until then nothing links this library; it builds with Mesa's tests
(`-Dbuild-tests=true`) and with `tests/standalone.sh`.
