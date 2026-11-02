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

Each is a place where `sgx545.xml` currently assumes something; they were
sent to the ISA.md owner and are tracked in the log entry.

1. hw 1 (frcp..fcos) h22: not described; required 0.
2. hw 3 / hw 4 h10: the sub-op is h9; h10 required 0.
3. The forms without src0 (fsubflr, fdp, fdsx, fdsy, fdpc): h7 (`|src0|`)
   required 0.
4. mov / movc h3-6: not described; required 0.
5. smp h19: not described; required 0.
6. testmask h2-3, h6; ima16 h15; ima32 h3, h20-21, h24; adif/adifsum/
   op16s1/bilin/ssum16 bits ISA.md doesn't name: required 0.
7. nop: predicate allowed (h24-26), h12 required 0.
8. Under format select, an `i<n>` operand: flagged only (`i1.f16`), not
   halved. The same assumed under efo's F16 format control.
9. test's fmt with a group-2 (C10) ALU op: indexed, immediate and constant
   operands assumed to take 6-bit numbers as under F16 format select.
10. smp's mask and ret share h12-15: the text's mask is read from h12-13
    when ret (h14-15) is set.
11. The text notation for several items isn't pinned down by ISA.md: abs
    (`|x|` here), `rpt=n` vs `rcnt`, smp's mnemonic and `drc=`, rnd's value
    names, fmad16 swizzle suffixes, efo's d/i/a/m value names, sop selects
    and when they're omitted, fdual's `sd=3` name, dot's src1/src2
    complement flags (`s1c`, `s2c` here).

## Switching the compiler over (not done yet)

`mesa/src/sgx545/compiler` encodes through `usse_forms.h` / `usse.c`, which
were generated from tables outside `docs/`. To move it onto this encoder:

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
