# SGX545 USSE instruction set (isaspec)

`sgx.xml` describes the 64-bit USSE (universal scalable shader engine)
instructions of the PowerVR SGX545 in Mesa's isaspec format. isaspec
generates the encoder and the disassembler from it; the C files here are the
glue and a small text assembler. The instruction set and its text notation
are documented in `docs/ISA.md` of the sgx545-ce repository; the comments
cite its sections by name.

| file | what |
|---|---|
| `sgx.xml` | the instruction set: one bitset hierarchy per class (h27-31), operand hierarchies per slot |
| `sgx_isa.h` | the API: `struct sgx_isa_instr`, `sgx_isa_encode()`, `sgx_disasm()` |
| `sgx_isa_encode.c` | leaf selection for the generated encoder, the helpers the XML's `<map>`s call |
| `sgx_isa_disasm.c` | the generated decoder plus custom fields, setfc tracking, bracket tidying |
| `sgx_isa_asm.c`, `sgx_isa_asm_int.c` | ISA.md's text notation into `struct sgx_isa_instr` |
| `sgx_isa_coissue.c` | co-issue for a compiler: what an fdual's two halves read and write (`sgx_isa_fdual_use()`), and the same-word races of fdual and efo (`sgx_isa_coissue_hazard()`), from ISA.md "fdual for a compiler" and "efo" |
| `assembler.c`, `disasm.c` | `sgx-asm` and `sgx-disasm`, the command-line tools (`-Dtools=sgx`) |
| `sgx_isa_enums.py` | generates `sgx-isa-enums.h`: the opcode and operand-kind enums the encoder switches on, and the field-value enums |
| `tests/sgx_isa_test.c` | text -> struct -> words -> text round trip; checks expected words where a line gives them |
| `tests/tools_test.py` | the lists again through `sgx-asm` and `sgx-disasm` |
| `tests/*.txt` | the test lists (`gles2.txt`: the forms a GLES2 compiler emits; `int.txt`: the integer, fixed-point and dual-issue classes; `coissue.txt`: the efo control words and the other spellings of efo and fdual, as `a == b` lines, and the co-issue hazards) |

Generated: `sgx-isa.c` / `sgx-isa.h` (decoder), `sgx-isa-encode.h`
(encoder), `sgx-isa-enums.h`.

## Tests

`meson test --suite sgx` runs `sgx_isa` (with `-Dbuild-tests=true`):
every line of the lists is assembled, encoded, disassembled and compared
with its input, and a line that gives `; <lo> <hi>` must encode to those
words. `sgx_isa_tools` puts the same lines through `sgx-asm` and
`sgx-disasm`, as hex and as binary.

## Tools

With `-Dtools=sgx` (patch 0006), Mesa builds two command-line tools; both
only read and write files.

    sgx-asm [-c core] [-F fc] [-b | -L] [-o out] [file...]
    sgx-disasm [-c core] [-F fc] [-x] [-w] [file...]

`sgx-asm` assembles a whole file in one run, one instruction per line. It
writes one output per instruction, in order: `<lo> <hi>` in hex (the
default), the words in binary, low word first and little endian (`-b`), or a
listing of offset, words and disassembly (`-L`). Blank lines and lines
starting with `#` are skipped, and `;` starts a comment. An error is reported
as `<file>:<line>: <text>: <message>`, and the exit status is then 1.
`sgx-disasm` reads binary, or with `-x` the first two hex words of each line
(so the output of `sgx-asm` and of `sgx-disasm -w` read back). It prints the
text, or with `-w` the words and the text.
`-c` selects the core (545, the default), and `-F` the starting format
control (1 = C10, 2 = F16; 1 by default). Both tools follow `setfc` from
there. `mesa/tests/isa-standalone/standalone.sh` builds both without a Mesa
build.

## Cores

`ISA_GPU_ID()` (the encoder's `gen`, the decoder's `gpu_id`) is
`core * 100 + format control`, from `sgx_isa_gpu_id()`: the core's number in
the hundreds, so a `<gen>` range selects a core (`min="54500"
max="54599"` is SGX545), and the format-control bits in the low digits. The
root bitset carries SGX545's range, so a word for a core `sgx.xml` doesn't
describe doesn't decode, and `sgx_isa_core_supported()` refuses it.
`struct sgx_isa_fc` carries the core; 0 means SGX545. A field another core
encodes differently gets a `<gen>` range, or an override that reads
`ISA_GPU_ID() / 100`, once ISA.md documents it for that core.

## isaspec change

This needs one isaspec change, carried as
`mesa/patches/0003-isaspec-derived-bitset-params-and-override-fields.patch`
in sgx545-ce and to be sent upstream ahead of this tree (see below).

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
  fields printed by `sgx_isa_disasm.c` only when they differ.

## What isaspec can't express, and what is done instead

- **Fields made of several bit ranges.** One field per range plus a
  `<derived>` field. isaspec decodes a derived field of bitset type, but its
  Python didn't accept `<param>`s on one, and the encoder's parameter struct
  didn't know their names: patch 0003, part 1.
- **Several `<override>`s defining the same field.** The generated encoder
  left a dangling `else` that swallowed the next field (it lost the F16 flag
  of an indexed operand under format select). Patch 0003, part 2: the
  definition in the case being encoded wins.
- **Modal state outside the word.**
  - Format control (`setfc`): the encoder takes it as `struct
    sgx_isa_fc` and hands it to the XML's expressions in the low digits of
    `ISA_GPU_ID()` (bit 0 = C10 enable, bit 1 = efo F16 enable; see
    "Cores"). The disassembler and the tools track
    `setfc` in a straight line from the program-start state (they do not
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
  XML and `sgx_disasm()` collapses the runs.

## Users

`src/sgx/compiler` encodes through this library: it fills a `struct
sgx_isa_instr` per instruction and calls `sgx_isa_encode()` at the
program-start format control (it emits no `setfc`).
`sgx_isa_fdual_use()` and `sgx_isa_coissue_hazard()` give a scheduler
the co-issue rules of fdual and efo.
