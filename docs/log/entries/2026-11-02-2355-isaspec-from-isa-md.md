# The USSE instruction set as a Mesa isaspec description, from ISA.md alone

`mesa/src/sgx545/isa/sgx545.xml` now describes every instruction class of
`docs/ISA.md` (hw 00-1f) in Mesa's isaspec format, and isaspec generates the
encoder and the disassembler from it. Nothing links it yet; the compiler
still encodes through `usse_forms.h`.

## What's there

- `sgx545.xml`: 109 instruction leaves over 4 operand hierarchies (dst,
  src1/src2, src0, the 1-bit-bank dst). Each leaf defines all 64 bits
  (isaspec checks that at generation time), so a word with a bit nobody
  described doesn't decode.
- `sgx545_isa.h`: `struct sgx_isa_instr` (operands by slot, as ISA.md
  names them) and `sgx545_isa_encode()`, `sgx545_disasm()`.
- A text assembler for ISA.md's notation (`sgx545_isa_asm.c`), so tests
  and hand-written programs are lines of ISA.md text.
- `tests/sgx545_isa_test`: text -> struct -> words -> text, and a fuzz mode.
  `tests/standalone.sh` builds it against an unpacked Mesa tree in seconds.
- `mesa/patches/0004`: two isaspec changes (below).

## Results

| check | result |
|---|---|
| Words ISA.md quotes (phas.i x3, nop, mov rcnt, the driver's atst8, pcoeff) | 7/7 bit-exact |
| Round trip, `tests/gles2.txt` + `tests/int.txt` | 243/243 |
| Fuzz: 400,000 random words, each that decodes re-assembled from its text | 211,625 decode, 0 mismatches |
| Mesa build (zig, x86_64-linux, Mesa's warning flags) | `libsgx545_isa.a` builds, no warnings |
| Oracle comparison, `gles2.enc.txt` (177 GLES2 instructions) | 177/177 bit-exact; rejects were spellings only, fixed after ISA.md d7f97e4 |
| Oracle comparison, `int.enc.txt` (66 integer / fdual) | pending |

## isaspec: what it couldn't express

- **Split fields.** A register operand is number + bank + extension in
  three places. Each piece is a field; a `<derived>` field joins them and
  is typed with an operand hierarchy whose leaf patterns are ISA.md's bank
  tables. isaspec decoded that but didn't accept `<param>`s on a derived
  field: patch 0004 part 1.
- **The same field in several `<override>`s** generated an encoder with a
  dangling `else` that ate the next field (the F16 flag of an indexed
  operand under format select went missing). Patch 0004 part 2. Both are
  candidates for upstream isaspec.
- **Modal state.** Format control reaches the XML's expressions as
  `ISA_GPU_ID()`, which isaspec already gives both the encoder (`s->gen`)
  and the decoder (`gpu_id`); the disassembler tracks `setfc` in a straight
  line. MOE state isn't in the word and isn't modelled.
- **"All values but N"** (movc type 1-5 vs mov 0): one leaf per value.

## Gaps in ISA.md

Listed in `mesa/src/sgx545/isa/README.md`. Answered in ISA.md d7f97e4 and
e77b9f9: undescribed bits are 0 (hw1 h22, hw3/4 h10, h7 without src0, mov
h3-6, smp h19, ...), nop has no predicate, internal registers are
flag-only under format select and format control, 6-bit numbers in every
slot under test's fmt, the smp mask/ret overlap, and the text notation.
Open: efo's value names, the sop selects' defaults, dot's complement names,
the integer classes' undescribed bits.

## Next

1. The oracle comparison of `int.enc.txt`.
2. Switch the compiler over (README.md "Switching the compiler over").
