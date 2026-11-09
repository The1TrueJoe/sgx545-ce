# SGX545 PDS programs (isaspec)

`sgx_pds.xml` describes the PDS (programmable data sequencer) programs of
the PowerVR SGX545 in Mesa's isaspec format. isaspec generates the encoder
and the disassembler from it; the C files here are the glue and a small text
assembler. The instruction set and its text notation are documented in
`docs/PDS.md` of the sgx545-ce repository; the comments cite its sections by
name.

| file | what |
|---|---|
| `sgx_pds.xml` | the instructions (`#instruction`) and the DOUT data-word layouts (encode-only roots) |
| `sgx_pds.h` | the API: `struct sgx_pds_instr`, `sgx_pds_encode()`, the DOUT word structs and encoders, the disassembler, the program layout |
| `sgx_pds_encode.c` | leaf selection for the generated encoder; the data-word encoders |
| `sgx_pds_disasm.c` | the generated decoder, number printing, whole-program dumps |
| `sgx_pds_asm.c` | the disassembler's text back into `struct sgx_pds_instr` |
| `tests/sgx_pds_test.c`, `tests/pds.txt` | text -> struct -> word -> text, PDS.md's words, the DOUT words PDS.md quotes, a random-word fuzz (`-f N`); `-d` disassembles words from stdin |

The generated files are `sgxpds-isa.*`: isaspec names its functions after
the part of the output file's name before the first `-`, so `sgx-pds`
would collide with the USSE library's `sgx_isa_disasm`. The enums come
from `../isa/sgx_isa_enums.py` (`--prefix sgx_pds`).

## The instructions

The text is PDS.md's "Text notation".

| op | s | form | text |
|---|---|---|---|
| 0, 2 | x | MOVS (DOUT) | `<dout>[.x2][.s27][.ccN] w0, w1, w2, w3 [r1=n] [r2=n]` |
| 4 | 0 / 1 | ADD / ? | `add dst, src1, src2`, `op0x4.s1` |
| 6 | 0 / 1 | MUL16 / ? | `mul16 dst, src1, src2`, `op0x6.s1` |
| c | 0 / 1 | OR / AND | `or`, `and` |
| d | 0 / 1 | XOR / ? | `xor`, `op0xd.s1` |
| 8 | 0 | TST | `tst src` (a ds1 source: the src2 form) |
| 9 | 0 | BRA | `bra #n` |
| a | 1 | HALT | `halt` |
| f | 0 / 1 | SHL / SHR | `shl dst, src, #n` |

- The condition: 7 is not printed, 0 is a `(p0) ` prefix, anything else a
  `.cc<n>` suffix (MOVS's `.cc3` is the vertex-fetch DOUTU form).
- A MOVS prints the four words it sends, each from select n: `ds0[n]`,
  `ds1[n]`, `in<n>` (DS0 words 0x40+). A row no select reads is printed as
  `[r1=n]` / `[r2=n]` when it isn't 0, so every word prints losslessly.
- Operands: U (`ds0[n]`, `ds1[n]`, `in<n>`) for the ALU destination and the
  shifts; S1 (`ds0[n]`, `in<n>`) and S2 (`ds1[n]`), with `.hi` for the high
  half. Numbers below 10 print in decimal, the rest in hex.
- A bit no PDS.md field names is 0 (PDS.md "Bits no field names"); a word
  that sets one does not decode.

## The data words

DOUTU words 0 and 1, the DOUTD control word, DOUTI words 0 and 1 and the
DOUTA control word are encode-only roots: they sit in the data segment, which
the decoder never sees (isaspec decodes from `#instruction` only). Each has a
struct (`struct sgx_pds_doutu`, ...) and an encoder
(`sgx_pds_doutu_w0()`, ...). The DOUTT words are texture state
(`src/sgx/csbgen/tex.xml`), not described here.

## Who uses it

- The winsys (`src/gallium/winsys/sgx/drm/sgx_frame.c`) builds every PDS
  program through it: the pixel, secondary/state, DMA, vertex-fetch,
  pixel-event, background and TA-terminate programs, and their DOUTU, DOUTD,
  DOUTI and DOUTA words.
- The driver (`src/gallium/drivers/sgx/sgx_program.c`) describes a fragment
  program's iterations as `struct sgx_pds_douti` (`struct sgxws_prog`);
  the winsys encodes them.

## Tests

`meson test --suite sgx` runs `sgx_pds` (with `-Dbuild-tests=true`):
each line of `tests/pds.txt` goes text -> instruction -> word -> text, a
line that gives a word must encode to it, and the DOUT data words PDS.md
quotes are checked field by field. `sgx_pds_test -f N` fuzzes N random
words through decode and re-assembly; `-d` disassembles words read from
stdin.
