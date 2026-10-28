# SGX545 USSE instruction set (DDK 1.7): the first, bit-flip-fuzzing pass

**Superseded by ISA.md**, which reads every class from libusc's decompiled
encoder and checks it with the emulated encoder. Kept for history: its tables
come from `fields.json` (dis_fz.py, asm_fz.py) and some of its readings are
wrong (dependent reads are not `smp` in the main program; `smoe` is `smlsi`).

What the Cedarview DDK's shader compiler emits for the SGX545, worked out from
its own output and its own encoder. `dis.py` disassembles USP binaries with
these tables, and `usp.py` parses the container.

## How each fact was established

Every field below carries one of these tags:

- **encoder**: libusc's own assembler moved these bits. `encfuzz.c` hooks
  `UseAssembleInstruction` (libusc + 0x109f50) inside a real compile, logs
  each USEASM instruction it is given with the 64 bits it returns, then
  re-encodes copies with one input bit flipped, the opcode set to 0..0x1ff,
  and each argument's register type set to 0..31. "Input bit X moves output
  bits Y" is then a fact about the hardware encoding, not an inference. What
  it can't give is the *meaning* of an input bit, beyond the USEASM field it
  sits in (argument number, type, flags; op flags).
- **corpus**: a one-op GLSL shader (`gen.py`, `gen2.py`) compiles to this
  opcode, or toggling the GLSL feature (negate, abs, precision, constant)
  toggles exactly this bit.
- **guess**: a name or meaning that fits but isn't backed by either.
  Mnemonics for USEASM opcodes that no corpus shader uses come from where the
  encoder puts them (hardware opcode, sub-op) plus the order of IMG's USEASM
  opcode enum as best remembered (FMAD, FADM, FMSA, FSUBFLR, FRCP, FRSQ, FLOG,
  FEXP, FSQRT, FSIN, FCOS, FDP, FMIN, FMAX, FDSX, FDSY, FMAD16, FMINMAX,
  FMAXMIN, EFO, …). That order is **confirmed** where the corpus reaches it:
  inversesqrt → 5, log2 → 6, exp2 → 7, sqrt → 8, sin → 9, cos → 0xa, all on
  hardware opcode 1 with sub-ops 1, 2, 3, 4, 5, 6.

Bit names: `l<n>` is bit n of the low word, `h<n>` bit n of the high word.
In a USP binary an instruction is stored low word first.

## The USEASM instruction the encoder takes

`USE_INST` is 0x100 bytes (the encoder copies 0x40 dwords): dword 0 the opcode,
1-3 flag words, 4 the test/condition word, then arguments of 5 dwords each from
dword 5: (register number, register type, flags, index, ?). Argument 0 is the
destination for ALU ops. Register types, numbered as libusc numbers them:

| type | prefix | evidence |
|---|---|---|
| 0 | `r` | temporaries in multi-instruction main programs (corpus) |
| 1 | `o` | encodes to the output bank (encoder); vertex outputs (corpus, `v_*`) |
| 2 | `pa` | varyings arrive here in pixel programs (list A; corpus); the fragment colour is written back here; the secondary program's loaded constants |
| 3 | `sa` | uniform values the main program reads, results of the secondary program (corpus: `gl_FragColor = u` is `mov pa0, sa9`) |
| 5 | `g` | same bank as 6 with register-number bit 6 set (encoder); special registers (**guess**) |
| 6 | `c` | hardware constant table; the compiler's "special constant zero" is type 6 number 0 (libusc 0x80e0) |
| 7 | `i` | internal registers: as src0 they become r124+n, elsewhere their own bank (encoder) |
| 8 | `#` | immediate; unused sources are type 8 value 0 (encoder, I-lines) |

## Common encoding (most ALU classes)

| bits | field | tag |
|---|---|---|
| h27..h31 | hardware opcode | encoder (every opcode sweep only changes these plus a sub-op) |
| h24..h26 | predicate (USEASM flags1 bits 13-15) | encoder; the value mapping isn't linear (b15 sets h25+h26) |
| h23 | flags1 bit 0; set on almost every compiled instruction | encoder; "skip invalid instances" is a **guess** |
| h20 | flags1 bit 2 | encoder; meaning unknown |
| h11 | flags1 bit 3 (h22 in the pack class) | encoder; "no schedule" is a **guess** |
| h18 | flags1 bit 17 | encoder; meaning unknown |
| h12..h15, h21 | flags1 bits 4-12. Pack class: bits 9-12 drive h12-h15 one-to-one, bits 4-8 drive codes in h12-h15 plus h21. fmad: bits 4-6 drive h12+h13, bit 10 drives h12. `dis.py` prints the raw field as `rm[...]` | encoder for the positions; bits 4-8 being a repeat count is **corpus** (vec4 ops set it, one-component ops don't); 9-12 being a channel mask selected by h21 is a **guess** |
| l21..l27 | destination register number (7 bits) | encoder |
| h0, h1 | destination bank: 0 r, 1 o, 2 pa, 3 indexed | encoder |
| h19 | destination bank extension: with it, bank 0 is sa, 3 is g/i | encoder |
| l14..l20 | src0 register number | encoder |
| h2 | src0 bank: 0 r, 1 pa (only these two; i0-i3 are r124-r127) | encoder |
| l7..l13 | src1 register number | encoder |
| l30, l31 | src1 bank: 0 r, 1 o, 2 pa, 3 sa | encoder |
| h17 | src1 bank extension: with it, 0 indexed, 1 c (g if number bit 6), 2 #, 3 i | encoder |
| l0..l6 | src2 register number | encoder |
| l28, l29 | src2 bank, same values as src1 | encoder |
| h16 | src2 bank extension | encoder |

An indexed operand (any bit of the USEASM index dword set) uses bank 3 for the
destination (bank 0 + extension for sources) and moves number bits 4 and 6
(l25/l27, l11/l13, l4/l6), so the index register and mode are packed into the
number field. Which bits mean what there isn't mapped.

Source modifiers, on the fmad class: argument flags bit 0 on src0 or src1 is
h5, a single negate on the product (**corpus**: `-a * b` and `a * -b` both
set h5; the encoder maps both there); bit 1 is abs, h7 for src0 and h6 for
src1 (corpus: `abs(a) * b`); on src2 they are h3 (negate) and h4 (abs)
(encoder). `a - b` is `fmad pa0, -pa4, c52, pa0`: b is the product operand,
so the subtract is h5 (corpus). Flags bit 22 on any source sets h22 and
number bit 6; an f16/f32 or high-half select is a **guess**.

## The USP container

Read from libusc's writer (`tools/intern/usc2/uspbin.c`; in
`libusc.so.1.7.862890`: header and lists `0xf28a0`, code-block walker
`0xf42a0`, code block `0xf4090`, branch `0xf26e0`, labels `0xf2380`/`0xf23d0`,
secondary-program writer `0xf2660`, record kind 3 `0xf2250`). **Confirmed**:
`usp.py` parses all 370 shaders of both corpora and every one ends exactly on
the end block at the declared size.

Everything goes through u32 / u16 / raw-byte write callbacks, packed with no
padding, so a u16 field leaves the next u32 unaligned.

| offset | type | meaning |
|---|---|---|
| 0x00 | u32 | magic `0x55535020` ("USP ") |
| 0x04 | u32 | version `0x23` |
| 0x08 | u32 | size of the binary **minus 12** (the old note said total size; it is 12 short) |
| 0x0c | u32 | 1: block type "PC shader header" |
| 0x10 | u32 | program type: 0 fragment, 1 vertex |
| 0x14 … 0x4d | | flags, register counts, constants-buffer sizes (not decoded) |
| 0x4e | u16 | list A entries |
| 0x50 | u16 | list C records |
| 0x52 | u32 | list B records |
| 0x56 … 0x7b | | more counts (not decoded) |
| 0x7c | u16 | instructions in the secondary update program |
| 0x7e … 0x83 | | (0x82 is always 0x80) |

After the header, in order:

1. **List A**: 12 bytes (6 u16) per entry. These are the iterated inputs; a
   fragment shader reading varying `a` has `(4, 0, 1, 4, 1, 4)` (highp),
   `(4, 0, 1, 4, 2, 2)` (mediump), `(4, 0, 1, 4, 3, 2)` (lowp). Field 3 looks like
   the varying index, field 5 the precision. **Guess** beyond that.
2. **List B**, then **List C**: records whose first u16 is a kind. Kinds 1, 2
   and 4 are 8 bytes; kinds 3 and 5 are 10. Kind 1 `(1, n, n, 0)` is one per
   uniform component the shader reads (constant n → register n). Kinds 2, 3,
   4 and 5 aren't in the corpus, so only their sizes are known (from the writer).
3. **Secondary update program**: header `0x7c` instructions, 8 bytes each.
   **Confirmed** to be a separate program from the main one: every fragment
   shader's starts with the same four instructions, and the compiler moves all
   uniform-only math here (`gl_FragColor = -u` puts the negate here and leaves
   one instruction in the main program; `-a * b` with varyings puts it in the
   main program and leaves this empty). That it is *the* SGX secondary update
   program (runs once per draw, writes secondary attributes) is a **strong
   guess** from that behaviour.
4. Vertex programs only: a bitmask of `ceil(n/32)` u32. n isn't in the header,
   so `usp.py` takes the smallest word count that makes the block stream end
   exactly at the declared size. Every vertex shader in the corpus has one word.
5. Four u32. The writer takes them from the compile state at +0x7fc.
6. **Block stream**, each block a u32 type and a body, until type 7:

| type | body | meaning |
|---|---|---|
| 2 | u16 n, u16 flags, 4 u16 MOE increments, 4 u16 MOE base offsets, n u16 per-instruction flags, n × 8-byte instructions | straight-line code |
| 3 | 8-byte instruction, u16 label | branch (the target is patched at finalize) |
| 4 | u16 label | label |
| 5 | 56 bytes | texture sample (non-dependent part); per writer 0xf48da |
| 6 | 74 bytes | texture sample; per writer 0xf46e0 |
| 9 | 78 bytes | texture sample variant; per writer 0xf43a0 |
| 7 | none | end |

The sample blocks hold no instructions. The GLES driver's USP finalizer
generates the sampling code from them at link time, which is why
`texture2D(s, uv)` alone compiles to no instructions.

Each instruction is 64 bits, stored as (low word, high word). The disassembler
names the bits l0..l31 and h0..h31.

## What the corpus shows about how programs are built

- **A vec4 op is one instruction with a repeat count**, not four (corpus:
  `a + b` highp is the single `fmad pa0, pa4, c52, pa0` with `rm` = repeat 4;
  `s_add`, one component, has none). Flags1 bits 4-8 carry the count (encoder).
- **Swizzles are MOE increments, not per-source selects** (corpus: `a.yzwx * b`
  is `smoe` with new increments, the `fmad`, then `smoe 1,1,1,1` to restore).
  `fa100000 01010101` (increments 1, 1, 1, 1) is the reset, in the secondary
  program and after every swizzle.
- **Hardware constants** (corpus, from what each one replaces): c48 = 0.0
  (`a * b` is `fmad a, b, -c48`: a·b + (−0)), c52 = 1.0 (`a + b` is
  `fmad b, c52, a`), c4 = 2.0 (`a * 2.0`), c14 = 0.5 (`a * 0.5`); −1 is `-c52`.
  3.0, 100.0 and 3.25 aren't in the table and come in as uniforms in sa.
- **Fragment colour**: computed in pa (in place over the varyings), packed to
  8 bits per channel into pa0 by two `pck.u8.*` instructions. Each pack writes
  two bytes, chosen by destination-flag bits 7-10 (h2-h5): f8+f9, then f7+f10
  (corpus; which byte is which channel isn't checked).
- **Vertex outputs** go to the `o` bank: `gl_Position = m * p` writes o0-o3
  with four `efo` (corpus `v_mvp`).
- **Predication**: `if (a.x > b.x) c else d` is one `fsub.neg.test` that sets a
  predicate, then the `mov` of the chosen colour with `p=5` (corpus `if_gt`).
  Loops use the same test plus a `br` block (block type 3) back to a label.
- **Secondary update program prologue**, the same in every fragment shader:
  `op0xf1` (`f8000940 00000000`), `and.test #0, #0`, `ld pa0 ← [#0]` (the
  uniform load, predicated p=1), `wdf` (wait for the load). After it comes the
  uniform-only math, written to pa and copied out at the end. Names are
  **guesses**; the load→wait→compute shape is what the code does.
- **lowp (C10 fixed point)** uses its own classes, hw 10, 11, 12, 18, 19, with
  per-channel source selects in the high word (the `a3`..`a6` arguments of
  useasm 0x58/0x59 map to h3-h10, h15, h20, h21, h24). **mediump** is mostly
  `fmad16` (hw 06) on f16 pairs.
- **Texture sampling**: non-dependent reads (`texture2D(s, uv)` straight from
  a varying) are USP sample blocks that the driver turns into code later.
  Dependent reads are `smp` (hw 1c) in the main program: `smp` for a computed
  coordinate, `smp.bias` for `texture2D(s, uv, bias)`, `smp.lod` for the
  vertex shader's `texture2DLod`.

## Instruction forms

Everything from the table on is generated: `python3 mkisa.py fields.json
names.json <encfuzz log>` (the log is `corpus/encfuzz.log.gz`, unpacked).

One row per hardware opcode (h27-h31). "Forms in the corpus" are the USEASM
opcodes the compiler actually emitted with that hardware opcode (fuzzed, so
their fields below are encoder-backed). "Other useasm ops" are ones the opcode
sweep encoded there from some base instruction. They show which USEASM
opcodes share hardware opcode h, but a sweep from a TEST-flagged base lands on
the TEST opcodes (09, 0f) whatever the op, so read 09 and 0f as "test forms".

Per form, below: the shaders that use it, the fixed bits (bits that never
changed across its encodings and that no input moves, used to recognise it),
each op-flag input bit and the output bits it moves, and per argument the
register-number bits, the bank bits with the value each register type encodes
to, the argument flag bits, and the types the compiler really used. Where a
number bit moves two output bits (e.g. `b3→l26+l27`), the field is packed in a
way not worked out yet. An argument with nothing but "compiler used" has no
fields (an unused slot). Forms with no fuzzed instance list only fixed bits.

| hw opcode | forms in the corpus (useasm op: mnemonic) | other useasm ops the sweep encodes here |
|---|---|---|
| 00 | 0x0: `fmad`, 0x2: `fmsa` (guess), 0xcf: `ffrc` | 0x1 0x3 0xa4 0xa5 0xa6 0xa9 |
| 01 | 0x4: `frcp`, 0x5: `frsq`, 0x6: `flog2`, 0x7: `fexp2`, 0x8: `fsqrt`, 0x9: `fsin`, 0xa: `fcos` | - |
| 02 | 0xb: `fdp` | 0xc2 0xc3 |
| 03 | 0x11: `fmin`, 0x12: `fmax` | 0xc 0xd |
| 04 | - | 0xe 0xf |
| 05 | 0xa3: `mov`, 0xf2: `movc` (guess) | - |
| 06 | 0x10: `fmad16` | - |
| 07 | 0x13: `efo` (guess) | - |
| 08 | 0x14: `op0x14` (guess), 0x15: `op0x15` (guess), 0x1c: `op0x1c` (guess), 0x20: `pck.u8.f32`, 0x21: `pck.u8.f16`, 0x2c: `op0x2c` (guess), 0x31: `op0x31` (guess), 0x3e: `op0x3e` (guess), 0x4d: `op0x4d` (guess), 0x4e: `pck.u8.c10` | 0x16 0x17 0x18 0x19 0x1a 0x1b 0x1d 0x1e 0x1f 0x22 0x23 0x24 0x25 0x26 0x27 0x28 0x29 0x2a 0x2b 0x2d 0x2e 0x2f 0x30 0x32 0x33 0x34 0x35 0x36 0x37 0x38 0x39 0x3a 0x3b 0x3c 0x3d 0x3f 0x40 0x41 0x42 0x43 0x44 0x45 0x46 0x47 0x48 0x49 0x4a 0x4b 0x4c 0xef 0x16f 0x170 |
| 09 | 0x4f: `and.test` (guess), 0x52: `shl.test` (guess), 0xa6: `fsub.test` (guess), 0xa9: `fsub.neg.test` (guess) | 0x3 0x4 0x5 0x6 0x7 0xb 0xc 0xd 0xe 0xf 0x50 0x51 0x53 0x54 0x55 0xa4 0xa5 0xc2 0xc3 0xcf 0xe4 0xe5 0xe7 0xe8 0xea 0xed 0xee 0xf0 0xf7 |
| 0a | - | 0x4f 0x50 |
| 0b | - | 0x51 |
| 0c | - | 0x52 0x55 |
| 0d | - | 0x53 0x54 |
| 0f | 0xa9: `fsub.neg.testmask` (guess) | 0x3 0x4 0x5 0x6 0x7 0xb 0xc 0xd 0xe 0xf 0x13 0x4f 0x50 0x51 0x52 0x53 0x54 0x55 0xa4 0xa5 0xa6 0xc2 0xc3 0xcf 0xe4 0xe5 0xe6 0xe7 0xe8 0xe9 0xea 0xeb 0xec 0xed 0xee 0xf0 0xf5 0xf6 0xf7 0xf8 0xf9 |
| 10 | 0x57: `op0x57` (guess) | - |
| 11 | 0x59: `op0x59` (guess), 0xe1: `op0xe1` (guess) | - |
| 12 | 0x58: `op0x58` (guess) | 0xfa |
| 13 | - | 0x62 0xeb 0xec 0xf5 0xf6 |
| 14 | 0x63: `op0x63` (guess) | 0xe4 0xe5 0xe6 0xe7 0xe8 0xe9 |
| 16 | - | 0x67 0x68 |
| 18 | 0x5a: `dp3.c10`, 0x5b: `dp4.c10` | 0x5c 0x5d 0x5e 0x5f 0x60 0x61 |
| 19 | 0xf4: `op0xf4` (guess) | - |
| 1a | - | 0x66 0x167 0x168 |
| 1b | 0x0: `fdual.fmad` (guess), 0x1: `fdual.fadm` (guess), 0xa3: `fdual.mov` (guess), 0xa7: `fdual.op0xa7` (guess), 0xc7: `fdual.op0xc7` (guess) | 0x2 0x11 |
| 1c | 0x70: `smp`, 0x73: `smp.bias`, 0x76: `smp.lod` | 0x6f 0x71 0x72 0x74 0x75 0x77 0x78 0x79 0x7a |
| 1d | 0x7d: `ld` (guess) | 0x7b 0x7c 0x83 0x84 0x85 0x8b 0x8c 0x8d |
| 1e | - | 0x7f 0x80 0x81 0x87 0x88 0x89 0x8f 0x90 0x91 |
| 1f | 0x96: `br` (guess), 0x9a: `wdf` (guess), 0xbe: `smoe` (guess), 0xc1: `op0xc1` (guess), 0xf1: `op0xf1` (guess) | 0x13 0x95 0x97 0x98 0x9b 0xbc 0xbd 0xbf 0xc0 0xcd 0xce 0xd1 0xd2 0xd3 0xd6 0xd7 0xd8 0xfe 0xff 0x100 0x101 0x102 0x103 0x104 |

### hw 00 / useasm 0x0: `fmad` (corpus)

- in 100 corpus shaders: neg, abs, floor, sin, cos, sign, norm3, len3, add, sub, mul, div …
- 120 distinct encodings, 6 fuzzed
- fixed bits: hi `00000000` under mask `f8000600`, lo `00000000` under mask `00000000`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13, b5→h12+h13, b6→h12+h13, b10→h12, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b28→h21
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l14..l20; bank [l16 l17 l18 l19 l20 h2]: r=000000/010000/110000, pa=000001/010001/110001, i=111110; flags b0→h5, b1→h7, b22→h22+l20; compiler used r, pa
- a2: number b0-5→l7..l12; bank [l30 l31 h17]: r=000, o=100, pa=010, sa=110, c=101, i=111, #=011; flags b0-1→h5..h6, b22→h22; compiler used c
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h3..h4, b22→h22+l6; index (any bit) → h16+l4+l6+l29; compiler used pa, c
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used #, 32

### hw 00 / useasm 0x2: `fmsa` (guess)

- in 3 corpus shaders: norm3, len3, norm3_m
- 4 distinct encodings, 4 fuzzed
- fixed bits: hi `00000400` under mask `f800c700`, lo `00000000` under mask `00000000`
- f1: b0→h23, b2→h20, b3→h11, b5→h12, b6→h12+h13, b10→h12, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b28→h21
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l14..l20; bank [l16 l17 l18 l19 l20 h2]: r=000000/000010/010000, pa=000001/000011/010001, i=111110; flags b1→h7, b22→h22+l20; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b0-1→h5..h6, b22→h22+l13; index (any bit) → h17+l10+l11+l12+l13+l31; compiler used pa
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b0-1→h3..h4, b22→h22+l6; index (any bit) → h16+l3+l4+l5+l6+l29; compiler used pa
- a4: compiler used 10, 32
- a6: compiler used r

### hw 00 / useasm 0xcf: `ffrc` (corpus)

- in 20 corpus shaders: floor, fract, ceil, sin, cos, mod, mod_m, mod_l, floor_m, floor_l, fract_m, fract_l …
- 41 distinct encodings, 6 fuzzed
- fixed bits: hi `00000600` under mask `f8000684`, lo `00000000` under mask `001fc000`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13, b5→h12+h13, b6→h12+h13, b10→h12, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b28→h21
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b22→h22+l13; index (any bit) → h17+l11+l13+l31; compiler used pa, c
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h3..h4, b22→h22+l6; index (any bit) → h16+l4+l6+l29; compiler used pa
- a3: compiler used c, #
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used #, 32

### hw 01 / useasm 0x4: `frcp` (corpus)

- in 14 corpus shaders: exp, rcp, div, div_m, div_l, mod, mod_m, mod_l, sqrt_m, sqrt_l, rcp_m, rcp_l …
- 43 distinct encodings, 6 fuzzed
- fixed bits: hi `08000000` under mask `f8400600`, lo `00000000` under mask `301fc07f`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13+h21, b5→h13+h21, b6→h13+h14+h21, b7→h13+h14+h15+h21, b8→h13+h14+h15+h16+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b22→h2
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b6→h3, b22→h7, b24→h7+h8; index (any bit) → h17+l11+l13+l31; compiler used pa
- a2: compiler used #
- a3: compiler used #
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used r, #, 32

### hw 01 / useasm 0x5: `frsq` (corpus)

- in 12 corpus shaders: rsq, norm3, sqrt_m, sqrt_l, rsq_m, rsq_l, norm3_m, norm3_l, s_rsq, s_rsq_m, v_rsq, v_norm
- 20 distinct encodings, 6 fuzzed
- fixed bits: hi `08000200` under mask `f8400600`, lo `00000000` under mask `301fc07f`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13+h21, b5→h13+h21, b6→h13+h14+h21, b7→h13+h14+h15+h21, b8→h13+h14+h15+h16+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b22→h2
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, x=0011, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b6→h3, b22→h7, b24→h3+h4+h7+h8; index (any bit) → h17+l11+l13+l31; compiler used pa
- a2: compiler used pa, #, 48
- a3: compiler used pa, #
- a4: compiler used #, 10, 32
- a5: compiler used #
- a6: compiler used r, #, 32

### hw 01 / useasm 0x6: `flog2` (corpus)

- in 9 corpus shaders: log2, log, pow, pow_m, pow_l, log2_m, log2_l, s_log2, s_log2_m
- 32 distinct encodings, 6 fuzzed
- fixed bits: hi `08000400` under mask `f8400600`, lo `00000000` under mask `301fc07f`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13+h21, b5→h13+h21, b6→h13+h14+h21, b7→h13+h14+h15+h21, b8→h13+h14+h15+h16+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b22→h2
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b22→h7, b24→h8; index (any bit) → h17+l11+l13+l31; compiler used pa
- a2: compiler used #
- a3: compiler used #
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used r, #, 32

### hw 01 / useasm 0x7: `fexp2` (corpus)

- in 9 corpus shaders: exp2, exp, pow, pow_m, pow_l, exp2_m, exp2_l, s_exp2, s_exp2_m
- 27 distinct encodings, 6 fuzzed
- fixed bits: hi `08000600` under mask `f8400600`, lo `00000000` under mask `301fc07f`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13+h21, b5→h13+h21, b6→h13+h14+h21, b7→h13+h14+h15+h21, b8→h13+h14+h15+h16+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b22→h2
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b22→h7, b24→h8; index (any bit) → h17+l11+l13+l31; compiler used pa, i
- a2: compiler used pa, #
- a3: compiler used pa, c, #, t16:
- a4: compiler used pa, #, 10
- a5: compiler used #
- a6: compiler used r, #, 32

### hw 01 / useasm 0x8: `fsqrt` (corpus)

- in 2 corpus shaders: sqrt, len3
- 4 distinct encodings, 4 fuzzed
- fixed bits: hi `08010000` under mask `f8410618`, lo `00000000` under mask `301fc07f`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13+h14+h21, b5→h13+h14+h21, b6→h12+h13+h14+h21, b7→h13+h14+h15+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b22→h2
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b22→h7, b24→h8; index (any bit) → h17+l11+l13+l31; compiler used pa
- a2: compiler used r, pa, #
- a3: compiler used r, pa, #
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used r, #, 32

### hw 01 / useasm 0x9: `fsin` (corpus)

- in 3 corpus shaders: sin, sin_m, sin_l
- 13 distinct encodings, 6 fuzzed
- fixed bits: hi `08010200` under mask `f8410610`, lo `00000000` under mask `301fc07f`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b22→h2
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, x=0011, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b22→h7, b24→h8; compiler used i
- a2: compiler used pa, c
- a3: compiler used c
- a4: compiler used #
- a5: compiler used #
- a6: compiler used #

### hw 01 / useasm 0xa: `fcos` (corpus)

- in 3 corpus shaders: cos, cos_m, cos_l
- 13 distinct encodings, 6 fuzzed
- fixed bits: hi `08010400` under mask `f8410610`, lo `00000000` under mask `301fc07f`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b22→h2
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, x=0011, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b22→h7, b24→h8; compiler used i
- a2: compiler used pa, c
- a3: compiler used c
- a4: compiler used #
- a5: compiler used #
- a6: compiler used #

### hw 02 / useasm 0xb: `fdp` (corpus)

- in 10 corpus shaders: sign, dp3, dp3_m, dp4, dpx, dpx_m, sign_m, s_dp3, s_dp3_m, v_dp4
- 10 distinct encodings, 6 fuzzed
- fixed bits: hi `10000000` under mask `f8000684`, lo `00000000` under mask `001fc000`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13, b5→h12+h13, b6→h12+h13, b10→h12, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b28→h21
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b0-1→h5..h6, b22→h22+l13; index (any bit) → h17+l11+l13+l31; compiler used pa, c
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b0-1→h3..h4, b22→h22+l6; index (any bit) → h16+l4+l6+l29; compiler used pa, c
- a3: compiler used #
- a4: compiler used 10
- a6: compiler used r, i, 32

### hw 03 / useasm 0x11: `fmin` (corpus)

- in 5 corpus shaders: min, min_m, s_min, s_min_m, v_psize
- 14 distinct encodings, 6 fuzzed
- fixed bits: hi `18000200` under mask `f8000600`, lo `00000000` under mask `00000000`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13, b5→h12+h13, b6→h12+h13, b10→h12, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b28→h21
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l14..l20; bank [h2]: r=0, pa=1; flags b1→h7, b22→h22+l20; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b0-1→h5..h6, b22→h22+l13; index (any bit) → h17+l11+l13+l31; compiler used pa
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b0-1→h3..h4, b22→h22+l6; index (any bit) → h16+l4+l6+l29; compiler used pa
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used #, 32

### hw 03 / useasm 0x12: `fmax` (corpus)

- in 4 corpus shaders: max, max_m, s_max, s_max_m
- 13 distinct encodings, 6 fuzzed
- fixed bits: hi `18000000` under mask `f8000600`, lo `00000000` under mask `00000000`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13, b5→h12+h13, b6→h12+h13, b10→h12, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b28→h21
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l14..l20; bank [h2]: r=0, pa=1; flags b1→h7, b22→h22+l20; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b0-1→h5..h6, b22→h22+l13; index (any bit) → h17+l11+l13+l31; compiler used pa
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b0-1→h3..h4, b22→h22+l6; index (any bit) → h16+l4+l6+l29; compiler used pa
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used #, 32

### hw 05 / useasm 0xa3: `mov` (corpus)

- in 112 corpus shaders: neg, abs, floor, fract, ceil, sqrt, rsq, exp2, log2, exp, log, sin …
- 21 distinct encodings, 6 fuzzed
- fixed bits: hi `28010000` under mask `f84107fc`, lo `20000000` under mask `301fc07f`
- f1: b0→h23, b2→h20, b3→h11, b4→h12+h13+h14+h21, b5→h13+h14+h21, b6→h12+h13+h14+h21, b7→h13+h14+h15+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13→h24+h25+h26, b14→h25+h26, b15→h25+h26, b17→h18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; index (any bit) → h17+l11+l12+l13+l30+l31; compiler used sa, c
- a2: compiler used r, sa, #
- a3: compiler used r, c, #
- a4: compiler used #
- a5: compiler used #
- a6: compiler used #

### hw 05 / useasm 0xf2: `movc` (guess)

- in 10 corpus shaders: sign, step, step_m, lt, lt_m, lt_l, abs_l, sign_m, sign_l, if_mul
- 25 distinct encodings, 6 fuzzed
- fixed bits: hi `28800000` under mask `ff94c878`, lo `00000000` under mask `00000000`
- f2: b19→h8+h9+h10, b20→h8+h9+h10
- test: b0→h7+h22, b1→h7+h22
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l14..l20; bank [h2]: r=0, pa=1; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; index (any bit) → h17+l11; compiler used r, c
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; index (any bit) → h16+l4+l6+l29; compiler used pa, c
- a4: compiler used #
- a5: compiler used #
- a6: compiler used #

### hw 06 / useasm 0x10: `fmad16` (corpus)

- in 57 corpus shaders: add_m, sub_m, mul_m, div_m, dp4_m, pow_m, mod_m, mad_m, mix_m, neg_m, abs_m, floor_m …
- 61 distinct encodings, 6 fuzzed
- fixed bits: hi `30000000` under mask `f8000000`, lo `00000000` under mask `00000000`
- f1: b0→h23, b2→h20, b3→h11, b13-14→h24..h25, b15→h25+h26
- f2: b29→h15, b30→h14, b31→h8
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l14..l20; bank [h2]: r=0, pa=1; flags b0→h5, b1→h7, b25-26→h9..h10; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h5..h6, b25→h13, b26→h18; index (any bit) → h17+l11+l13+l31; compiler used pa, c
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0-1→h3..h4, b25-26→h21..h22; index (any bit) → h16+l4+l6+l29; compiler used pa, c
- a4: compiler used 10
- a6: compiler used r, 32

### hw 07 / useasm 0x13: `efo` (guess)

- in 39 corpus shaders: exp, log, sin, cos, norm3, mix, swzadd_xxxx, swzadd_yyyy, swzadd_wwww, swzadd_xyxy, vmvp, sin_l …
- 65 distinct encodings, 6 fuzzed
- fixed bits: hi `38000000` under mask `f8000000`, lo `00000000` under mask `00000000`
- f1: b0→h23, b3→h11, b5→h12, b6→h12+h13, b13-14→h24..h25
- f3: b10→h8
- a0: number b0-6→l21..l27; bank [h0 h1]: r=00, o=10, pa=01; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l14..l20; bank [h2 h6]: r=00, o=01, pa=10, sa=11; flags b0→h5, b22→l20; compiler used pa
- a2: number b0-6→l7..l13; bank [l30 l31]: r=00, o=10, pa=01, sa=11; flags b0→h4, b22→l13; compiler used pa
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h7]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0→h3, b22→l6; index (any bit) → h7+l4+l6+l29; compiler used pa, c
- a4: number b0→h0, b3-22→h3..h22, b24-26→h24..h26, b30-31→h30..h31; compiler used #
- a5: compiler used #
- a6: compiler used r, #

### hw 08 / useasm 0x14: `op0x14` (guess)

- in 5 corpus shaders: step_l, mod_m, floor_m, sin_m, cos_m
- 8 distinct encodings, 6 fuzzed
- fixed bits: hi `40000d4c` under mask `f8000fcc`, lo `00000000` under mask `00080000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h13+h21, b5→h12+h21, b6→h13+h14+h21, b7→h13+h14+h15+h21, b8→h13+h14+h15+h16+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18, b24→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11; compiler used r, c
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; index (any bit) → h16+l4+l6+l29; compiler used r, pa, #
- a3: number b0→l20; compiler used t16:
- a4: compiler used 10, 32
- a6: compiler used r, pa

### hw 08 / useasm 0x15: `op0x15` (guess)

- in 8 corpus shaders: div_m, step_l, pow_m, mod_m, swm_yzwx, swm_wzyx, swm_zxyw, swm_xwyz
- 30 distinct encodings, 6 fuzzed
- fixed bits: hi `40000b40` under mask `f8000fc0`, lo `00000000` under mask `00080000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13→h24+h25+h26, b14→h25+h26, b15→h25+h26, b17→h18, b24→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; index (any bit) → h0+l25+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11+l12+l13+l30+l31; compiler used r, pa, sa
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; index (any bit) → h16+l4+l6+l29; compiler used pa, #
- a3: number b0→l20; compiler used t16:
- a4: compiler used 10
- a6: compiler used 32

### hw 08 / useasm 0x1c: `op0x1c` (guess)

- in 1 corpus shaders: varr
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `40000d0c` under mask `f8000ffc`, lo `00000000` under mask `00180000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b8→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11+l13+l31; compiler used pa
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; compiler used #
- a3: compiler used t16:
- a4: compiler used 10
- a5: compiler used r
- a6: compiler used r

### hw 08 / useasm 0x20: `pck.u8.f32` (corpus)

- in 183 corpus shaders: neg, abs, floor, fract, ceil, sqrt, rsq, exp2, log2, exp, log, sin …
- 84 distinct encodings, 6 fuzzed
- fixed bits: hi `40000c00` under mask `f8000fc0`, lo `00000000` under mask `00080000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b8→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101; flags b7-10→h2..h5; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11+l13+l31; compiler used pa, i
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; index (any bit) → h16+l4+l6+l29; compiler used pa, i
- a3: number b0→l20; compiler used t16:
- a4: compiler used pa, #
- a5: compiler used #
- a6: compiler used r, #

### hw 08 / useasm 0x21: `pck.u8.f16` (corpus)

- in 75 corpus shaders: add_m, sub_m, mul_m, div_m, step_l, pow_m, mod_m, mad_m, mix_m, c0, c1, c_half …
- 34 distinct encodings, 6 fuzzed
- fixed bits: hi `40000a00` under mask `f8000fc0`, lo `00000000` under mask `00080000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b8→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101; flags b7-10→h2..h5; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11+l13+l31; compiler used r, pa
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; index (any bit) → h16+l4+l6+l29; compiler used r, pa
- a3: number b0→l20; compiler used t16:
- a4: compiler used 10
- a6: compiler used r, 32

### hw 08 / useasm 0x2c: `op0x2c` (guess)

- in 6 corpus shaders: mod_l, dpx_l, floor_l, sin_l, cos_l, sign_l
- 15 distinct encodings, 6 fuzzed
- fixed bits: hi `40000dc0` under mask `f8000fc0`, lo `00000000` under mask `00080000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b8→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; flags b7-10→h2..h5; compiler used pa, i
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11+l13+l31; compiler used r, pa, c
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; index (any bit) → h16+l4+l6+l29; compiler used r, pa, #
- a3: number b0→l20; compiler used t16:
- a4: compiler used r, t16:
- a5: compiler used t16:
- a6: compiler used t16:

### hw 08 / useasm 0x31: `op0x31` (guess)

- in 1 corpus shaders: sign_m
- 4 distinct encodings, 4 fuzzed
- fixed bits: hi `40000bbc` under mask `f8000ffc`, lo `00000000` under mask `00080000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18, b24→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, x=0011, g=1101, i=0111; index (any bit) → h0+h1+l25; compiler used r
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11+l13+l31; compiler used pa
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; compiler used #
- a3: number b0→l20; compiler used t16:
- a4: compiler used 32

### hw 08 / useasm 0x3e: `op0x3e` (guess)

- in 4 corpus shaders: mod_l, floor_l, sin_l, cos_l
- 7 distinct encodings, 6 fuzzed
- fixed bits: hi `40000f7c` under mask `f8000ffc`, lo `00000000` under mask `00080000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b8→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, x=0011, g=1101, i=0111; index (any bit) → h0+h1+l25+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; compiler used i
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; compiler used i
- a3: number b0→l20; compiler used t16:
- a4: compiler used r, t16:
- a5: compiler used t16:
- a6: compiler used t16:

### hw 08 / useasm 0x4d: `op0x4d` (guess)

- in 7 corpus shaders: dp3_l, dp4_l, dpx_l, wm_x, wm_xy, sin_l, cos_l
- 7 distinct encodings, 6 fuzzed
- fixed bits: hi `40000000` under mask `f8000fc0`, lo `00000000` under mask `001c0000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; flags b7-10→h2..h5; index (any bit) → h0+l25+l27; compiler used pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11+l13+l31; compiler used r, pa, #
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; index (any bit) → h16+l4+l6+l29; compiler used pa, c
- a3: compiler used t16:
- a4: compiler used pa, 10, t16:
- a5: compiler used t16:
- a6: compiler used r, t16:, 32

### hw 08 / useasm 0x4e: `pck.u8.c10` (corpus)

- in 25 corpus shaders: add_l, sub_l, mul_l, div_l, min_l, max_l, dp4_l, pow_l, mod_l, mad_l, mix_l, neg_l …
- 30 distinct encodings, 6 fuzzed
- fixed bits: hi `40000fc0` under mask `f8000fc0`, lo `00000000` under mask `00180000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b7→h13+h14+h21, b8→h13+h14+h15+h21, b9-12→h12..h15, b13-14→h24..h25, b15→h25+h26, b17→h18
- f2: b8→l18
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; flags b7-10→h2..h5; compiler used i
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l16..l17; index (any bit) → h17+l11+l13+l31; compiler used pa
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b5-6→l14..l15; index (any bit) → h16+l4+l6+l29; compiler used pa, i
- a3: compiler used t16:
- a4: compiler used 10
- a6: compiler used r, 32

### hw 09 / useasm 0x4f: `and.test` (guess)

- in 97 corpus shaders: neg, abs, floor, fract, ceil, sqrt, rsq, exp2, log2, exp, log, sin …
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `48881001` under mask `ffdcf003`, lo `000c0000` under mask `0fffc000`
- f2: b0→h21
- test: b0-1→h10..h11, b2-3→h8..h9, b4→h7, b5-7→h4..h6
- a0: compiler used c
- a1: number b0-1→h2..h3; compiler used drc
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; compiler used #
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; compiler used #
- a6: compiler used 32

### hw 09 / useasm 0x52: `shl.test` (guess)

- in 1 corpus shaders: step_l
- 6 distinct encodings, 6 fuzzed
- fixed bits: hi `48881001` under mask `ffdcf003`, lo `000cc000` under mask `0fffc000`
- f2: b0→h21
- test: b0-1→h10..h11, b2-3→h8..h9, b4→h7, b5-7→h4..h6
- a0: compiler used c
- a1: number b0-1→h2..h3; compiler used drc
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; index (any bit) → h17+l11+l13+l31; compiler used r, pa
- a3: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; compiler used #

### hw 09 / useasm 0xa6: `fsub.test` (guess)

- in 2 corpus shaders: branch, if_z
- 2 distinct encodings, 2 fuzzed
- fixed bits: hi `48881001` under mask `ffd8f003`, lo `00000000` under mask `0fffc040`
- f2: b0→h21
- test: b0-1→h10..h11, b2-3→h8..h9, b4→h7, b5-7→h4..h6
- a0: compiler used c
- a1: number b0-1→h2..h3; compiler used drc
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b22→h18+l13; index (any bit) → h17+l11+l12+l13+l30+l31; compiler used pa, sa
- a3: number b0-5→l0..l5; bank [l28 l29 h16]: r=000, o=100, pa=010, sa=110, c=101, i=111, #=011; flags b22→h18; compiler used c

### hw 09 / useasm 0xa9: `fsub.neg.test` (guess)

- in 10 corpus shaders: step_l, discard, loop4, loop_dyn, if_gt, if_lt, if_eq, if_ge, if_ne, loop_v
- 16 distinct encodings, 2 fuzzed
- fixed bits: hi `48081001` under mask `ff58f003`, lo `00038000` under mask `0fffc000`
- f2: b0→h21
- test: b0-1→h10..h11, b2-3→h8..h9, b4→h7, b5-7→h4..h6
- a0: compiler used c
- a1: number b0-1→h2..h3; compiler used drc
- a2: number b0-5→l7..l12; bank [l13 l30 l31 h17]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; flags b22→l13; compiler used sa
- a3: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; flags b22→l6; compiler used sa

### hw 0f / useasm 0xa9: `fsub.neg.testmask` (guess)

- in 7 corpus shaders: step, step_m, step_l, lt, lt_m, lt_l, if_mul
- 19 distinct encodings, 4 fuzzed
- fixed bits: hi `78801000` under mask `ffd0104c`, lo `00038000` under mask `000fc000`
- f2: b0→h21
- test: b0-1→h10..h11, b2-3→h8..h9, b4→h7, b9→h4+h5, b10→h5
- a0: number b0-6→l21..l27; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; flags b16→h0+h1+h19+l20+l21+l23+l24, b22→l27; index (any bit) → h0+h1+l24+l25+l26+l27; compiler used r, pa
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b22→h18+l13; index (any bit) → h17+l11+l13+l31; compiler used pa, sa
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; flags b22→h18+l6; index (any bit) → h16+l4+l6+l29; compiler used pa, sa
- a3: compiler used #
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used #, 32

### hw 10 / useasm 0x57: `op0x57` (guess)

- in 1 corpus shaders: mov_l
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `80800004` under mask `fef47e04`, lo `00000000` under mask `0013c000`
- a0: number b0-5→l21..l26; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; flags b24→l27; index (any bit) → h0+l24+l26; compiler used pa
- a1: number b0-5→l7..l12; bank [l13 l30 l31 h17]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; index (any bit) → h17+l10+l12+l31; compiler used pa
- a2: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; index (any bit) → h16+l3+l5+l29; compiler used pa
- a3: compiler used t16:
- a4: number b0→h24, b1→h6+h8, b3→h6+h7; flags b15→h24; compiler used t16:
- a5: number b0→h15, b1→h3+h5, b3→h3+h4; flags b15→h15; compiler used t16:
- a6: number b0→l18, b2→l19; compiler used t16:

### hw 11 / useasm 0x59: `op0x59` (guess)

- in 9 corpus shaders: mul_l, div_l, pow_l, mod_l, mad_l, mix_l, neg_l, abs_l, satmul_l
- 14 distinct encodings, 6 fuzzed
- fixed bits: hi `88800000` under mask `fea40800`, lo `00100000` under mask `00100000`
- a0: number b0-5→l21..l26; bank [l27 h0 h1 h19]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0001/1001, x=1011, g=1101, i=0111/1111; flags b24→l27; compiler used pa, i
- a1: number b0-1→l14..l15; bank [l16 l17 l18 l19 h2]: r=00000, pa=00001, i=11110; compiler used i
- a2: number b0-5→l7..l12; bank [l13 l30 l31 h17]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; compiler used pa, i
- a3: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; compiler used pa, i
- a4: number b0→h7+h8+h24, b1→h6+h7, b2→h8, b3→h6+h7; flags b15→h24; compiler used t16:
- a5: number b0→h4+h5+h15, b1→h3+h4, b2→h5, b3→h3+h4; flags b15→h15; compiler used t16:
- a6: number b0→h20; compiler used t16:

### hw 11 / useasm 0xe1: `op0xe1` (guess)

- in 1 corpus shaders: norm3_l
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `89a01000` under mask `ffe4bff8`, lo `00100000` under mask `00102000`
- a0: number b0-5→l21..l26; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; flags b24→l27; index (any bit) → h0+l24+l26; compiler used pa
- a1: number b0-5→l14..l19; bank [l16 l17 l18 l19 h2]: r=00000, pa=00001, i=11110; compiler used pa
- a2: number b0-5→l7..l12; bank [l30 l31 h17]: r=000, o=100, pa=010, sa=110, c=101, i=111, #=011; compiler used c
- a3: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; index (any bit) → h16+l3; compiler used r
- a4: number b0→h20; flags b15→h20; compiler used t16:
- a5: flags b15→h14; compiler used t16:
- a6: compiler used t16:

### hw 12 / useasm 0x58: `op0x58` (guess)

- in 22 corpus shaders: add_l, sub_l, min_l, max_l, pow_l, mod_l, mad_l, dpx_l, wm_w, neg_l, abs_l, floor_l …
- 32 distinct encodings, 6 fuzzed
- fixed bits: hi `90000000` under mask `f8000124`, lo `00000000` under mask `001fc000`
- f1: b0→h23, b3→h22, b13-14→h25..h26, b17→h18
- a0: number b0-5→l21..l26; bank [l27 h0 h1 h19]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0001/1001, x=1011, g=1101, i=0111/1111; flags b7-9→h12..h14, b10→h11, b24→l27; compiler used pa, i
- a1: number b0-5→l7..l12; bank [l13 l30 l31 h17]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; compiler used i
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0110/1110, g=1101, c=0101, i=0111/1111, #=0011; compiler used i, #
- a3: number b0→h24, b1→h6, b3→h6+h7; flags b15→h24; compiler used t16:
- a4: number b0→h15, b1→h3, b3→h3+h4; flags b15→h15; compiler used t16:
- a5: number b0→h20, b2→h21; compiler used t16:
- a6: number b0→h9, b2→h10; compiler used t16:

### hw 14 / useasm 0x63: `op0x63` (guess)

- in 1 corpus shaders: varr
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `a0030000` under mask `f8138020`, lo `a0200000` under mask `f7a00000`
- f1: b0→h23, b3→h22, b5→h12, b6→h12+h13, b7→h12+h13+h14, b13-14→h25..h26, b17→h18
- f2: b10→h10, b12→h11, b15-16→h3..h4
- a0: number b1→l22; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, x=0011, g=1101, i=0111; flags b1→h24; compiler used x
- a1: number b0-6→l14..l20; bank [l16 l17 l18 l19 l20 h2]: r=000000, pa=000001, i=111110; compiler used r
- a2: number b0-6→l7..l13; compiler used #
- a3: number b0-6→l0..l6; flags b0→h21; compiler used #
- a4: number b0→h6+h7; compiler used t16:
- a5: number b0→h8+h9; compiler used t16:

### hw 18 / useasm 0x5a: `dp3.c10` (corpus)

- in 3 corpus shaders: dp3_l, dpx_l, norm3_l
- 3 distinct encodings, 3 fuzzed
- fixed bits: hi `c0000000` under mask `f930897c`, lo `00000000` under mask `001ec000`
- f1: b0→h23, b3→h22, b5→h12, b6→h12+h13, b7→h12+h13+h14, b13-14→h25..h26, b17→h18, b25→h9+h10
- a0: number b0-5→l21..l26; bank [l27 h0 h1 h19]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0001/1001, g=1101, i=0111/1111; flags b24→l27; index (any bit) → h0+h1+l24+l26; compiler used r, pa
- a1: number b0→h7; compiler used t16:
- a2: number b0→l16; compiler used t16:
- a3: number b0-5→l7..l12; bank [l13 l30 l31 h17]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; index (any bit) → h17+l10+l12+l31; compiler used pa
- a4: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; index (any bit) → h16+l3+l5+l29; compiler used pa
- a6: compiler used r

### hw 18 / useasm 0x5b: `dp4.c10` (corpus)

- in 1 corpus shaders: dp4_l
- 2 distinct encodings, 2 fuzzed
- fixed bits: hi `c1000600` under mask `f9308f7c`, lo `00000000` under mask `001ec000`
- f1: b0→h23, b3→h22, b5→h12, b6→h12+h13, b7→h12+h13+h14, b13-14→h25..h26, b17→h18
- a0: number b0-5→l21..l26; bank [l27 h0 h1 h19]: r=0000, o=0100, pa=0010, sa=0001, g=1101, i=0111; flags b24→l27; index (any bit) → h0+l24+l26; compiler used pa
- a1: number b0→h7; compiler used t16:
- a2: number b0→l16; compiler used t16:
- a3: number b0-5→l7..l12; bank [l13 l30 l31 h17]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; compiler used i
- a4: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; compiler used i
- a6: compiler used r

### hw 19 / useasm 0xf4: `op0xf4` (guess)

- in 3 corpus shaders: sin_l, cos_l, mad_l
- 3 distinct encodings, 3 fuzzed
- fixed bits: hi `c8000800` under mask `f8000be8`, lo `00100000` under mask `00100000`
- f1: b0→h23, b3→h22, b5→h12, b6→h12+h13, b7→h12+h13+h14, b13-14→h25..h26, b17→h18
- a0: number b0-5→l21..l26; bank [l27 h0 h1 h19]: r=0000/1000, o=0100/1100, pa=0010/1010, sa=0001/1001, g=1101, i=0111/1111; flags b24→l27; index (any bit) → h0+l24+l26; compiler used pa, i
- a1: number b0-5→l14..l19; bank [l16 l17 l18 l19 h2]: r=00000, pa=00001, i=11110; compiler used pa, i
- a2: number b0-5→l7..l12; bank [l13 l30 l31 h17]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; compiler used i
- a3: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=1000, o=1100, pa=1010, sa=1110, g=1101, c=0101, i=1111, #=0011; compiler used sa, i
- a4: number b0→h20, b1→h20+h21; flags b15→h10; compiler used t16:
- a5: flags b15→h24; compiler used t16:
- a6: flags b15→h15; compiler used t16:

### hw 1b / useasm 0x0: `fdual.fmad` (guess)

- in 5 corpus shaders: sin, cos, pow, mod, pow_m
- 22 distinct encodings, 6 fuzzed
- fixed bits: hi `d8800000` under mask `ff9c1000`, lo `00000000` under mask `00000000`
- a0: number b0-6→l21..l27; bank [l23 l24 l25 l26 l27 h0 h1]: r=0000000/0100000, o=0000010/0100010, pa=0000001/0100001, i=1111100; index (any bit) → h0+l25+l27; compiler used pa, i
- a1: number b0-6→l14..l20; bank [h2]: r=0, pa=1; flags b22→h22+l20; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0→h4, b22→h22+l13; index (any bit) → h17+l11+l13+l31; compiler used pa, c
- a3: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0→h3, b22→h22; compiler used c
- a4: compiler used #, 10
- a5: compiler used #
- a6: compiler used r, #

### hw 1b / useasm 0x1: `fdual.fadm` (guess)

- in 1 corpus shaders: mod
- 2 distinct encodings, 4 fuzzed
- fixed bits: hi `d8884300` under mask `ffbcf7e0`, lo `00000000` under mask `00000040`
- a0: number b0-6→l21..l27; bank [l23 l24 l25 l26 l27 h0 h1]: r=0000000, o=0000010, pa=0000001, i=1111100; index (any bit) → h0+h1+l25; compiler used r
- a1: number b0-6→l14..l20; bank [l16 l17 l18 l19 l20 h2]: r=000000/001000, pa=000001/001001, i=111110; flags b22→h22+l20; compiler used r, pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0→h4, b22→h22+l13; index (any bit) → h17+l11; compiler used r
- a3: number b0-5→l0..l5; bank [l28 l29 h16]: r=000, o=100, pa=010, sa=110, c=101, i=111, #=011; flags b0→h3, b22→h22; compiler used c
- a4: compiler used r, 10
- a6: compiler used r

### hw 1b / useasm 0xa3: `fdual.mov` (guess)

- in 1 corpus shaders: v_norm
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `d8a4a400` under mask `fffdfffc`, lo `10000000` under mask `301fe07f`
- a0: number b0-6→l21..l27; bank [l23 l24 l25 l26 l27 h0 h1]: r=0000000, o=0000010, pa=0000001, i=1111100; index (any bit) → h1+l25+l26; compiler used o
- a1: number b0-5→l7..l12; bank [l30 l31 h17]: r=000, o=100, pa=010, sa=110, c=101, i=111, #=011; compiler used c
- a2: compiler used pa
- a4: compiler used 32
- a6: compiler used pa

### hw 1b / useasm 0xa7: `fdual.op0xa7` (guess)

- in 1 corpus shaders: v_norm
- 1 distinct encodings, 2 fuzzed
- fixed bits: hi `dcac23c4` under mask `ffbcffe4`, lo `00008000` under mask `001fc000`
- a0: number b0-6→l21..l27; bank [l23 l24 l25 l26 l27 h0 h1]: r=0000000, o=0000010, pa=0000001, i=1111100; index (any bit) → h1+l25+l26; compiler used o
- a1: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0→h4, b22→h22+l13; index (any bit) → h17+l11+l13+l31; compiler used pa
- a2: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0→h3, b22→h22+l6; index (any bit) → h16+l4+l6+l29; compiler used pa
- a4: compiler used 32
- a6: compiler used pa

### hw 1b / useasm 0xc7: `fdual.op0xc7` (guess)

- in 1 corpus shaders: exp
- 1 distinct encodings, 2 fuzzed
- fixed bits: hi `dc8069c0` under mask `ffbcffe0`, lo `00000000` under mask `00000000`
- a0: number b0-1→l21..l22; bank [l23 l24 l25 l26 l27 h0 h1]: r=0000000, o=0000010, pa=0000001, i=1111100; compiler used i
- a1: flags b22→h22; compiler used i
- a2: number b0-6→l14..l20; bank [h2]: r=0, pa=1; flags b22→h22+l20; compiler used pa
- a3: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0→h4, b22→h22+l13; index (any bit) → h17+l11+l13+l31; compiler used pa
- a4: number b0-6→l0..l6; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; flags b0→h3, b22→h22+l6; index (any bit) → h16+l4+l6+l29; compiler used pa
- a6: compiler used r

### hw 1c / useasm 0x70: `smp` (corpus)

- in 2 corpus shaders: tex_dep, t_dep
- 2 distinct encodings, 2 fuzzed
- fixed bits: hi `e0010400` under mask `f8090f02`, lo `20000000` under mask `3000007f`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b9-10→h12..h13, b13-14→h24..h25, b15→h25+h26
- f3: b0-1→h5..h6, b2→h15, b3→h14
- a0: number b0-6→l21..l27; bank [h7]: r=0, pa=1; compiler used r
- a1: number b0-6→l14..l20; bank [l16 l17 l18 l19 l20 h2 h18]: r=0000000/0100000, o=0000001/0100001, pa=0000010/0100010, sa=0000011/0100011, i=1111100; flags b22→h3, b24→h4; compiler used pa, sa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; compiler used #
- a3: number b0→h0; compiler used 10
- a4: compiler used t16:
- a6: compiler used r

### hw 1c / useasm 0x73: `smp.bias` (corpus)

- in 2 corpus shaders: tex_bias, t_lod
- 2 distinct encodings, 2 fuzzed
- fixed bits: hi `e0000500` under mask `f8080f02`, lo `00000000` under mask `00000000`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b9-10→h12..h13, b13-14→h24..h25, b15→h25+h26
- f3: b0-1→h5..h6, b2→h15, b3→h14
- a0: number b0-6→l21..l27; bank [h7]: r=0, pa=1; compiler used r
- a1: number b0-6→l14..l20; bank [l16 l17 l18 l19 l20 h2 h18]: r=0000000, o=0000001, pa=0000010, sa=0000011, i=1111100; flags b22→h3, b24→h4; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; compiler used #
- a3: number b0-5→l0..l5; bank [l6 l28 l29 h16]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; compiler used c
- a4: number b0→h0; compiler used 10
- a5: compiler used t16:
- a6: compiler used r

### hw 1c / useasm 0x76: `smp.lod` (corpus)

- in 1 corpus shaders: vtex
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `e0000600` under mask `f8080f02`, lo `00000000` under mask `00000040`
- f1: b0→h23, b2→h20, b3→h22, b4→h12+h21, b5→h21, b6→h13+h21, b9-10→h12..h13, b13-14→h24..h25, b15→h25+h26
- f3: b0-1→h5..h6, b2→h15, b3→h14
- a0: number b0-6→l21..l27; bank [h7]: r=0, pa=1; compiler used r
- a1: number b0-6→l14..l20; bank [l16 l17 l18 l19 l20 h2 h18]: r=0000000, o=0000001, pa=0000010, sa=0000011, i=1111100; flags b22→h3, b24→h4; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; compiler used #
- a3: number b0-5→l0..l5; bank [l28 l29 h16]: r=000, o=100, pa=010, sa=110, c=101, i=111, #=011; compiler used c
- a4: number b0→h0; compiler used 10
- a5: compiler used t16:
- a6: compiler used r

### hw 1d / useasm 0x7d: `ld` (guess)

- in 97 corpus shaders: neg, abs, floor, fract, ceil, sqrt, rsq, exp2, log2, exp, log, sin …
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `e8010000` under mask `f8410c30`, lo `20000000` under mask `3000007f`
- f1: b0→h23, b2→h20, b5→h12, b6→h12+h13, b7→h12+h13+h14, b8→h12+h13+h14+h15, b13→h24, b14→h26, b15→h25, b27-28→h8..h9, b29→h6, b31→h21
- f2: b2→h1, b3→h19, b9→h3
- f3: b9→h1+h19
- a0: number b0-6→l21..l27; bank [h7]: r=0, pa=1; compiler used pa
- a1: number b0-6→l14..l20; bank [l16 l17 l18 l19 l20 h2 h18]: r=0000000, o=0000001, pa=0000010, sa=0000011, i=1111100; compiler used pa
- a2: number b0-6→l7..l13; bank [l13 l30 l31 h17]: r=0000, o=0100, pa=0010, sa=0110, g=1101, c=0101, i=0111, #=0011; compiler used #
- a3: compiler used #
- a4: number b0→h0; compiler used 10
- a6: compiler used 32

### hw 1f / useasm 0x96: `br` (guess)

- in 3 corpus shaders: loop4, loop_dyn, loop_v
- 2 distinct encodings, 2 fuzzed
- fixed bits: hi `f8000040` under mask `f87fe1ff`, lo `00000000` under mask `fffffef1`
- f1: b3→h11+l3+l8, b9→l3+l8, b13→h24+l3+l8, b14→h25+h26+l3+l8, b15→h25+h26+l3+l8, b18→h23+l3+l8, b19→h9+l3+l8, b22→h10+l3+l8
- f2: b6→l3+l8, b7→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b24→l3+l8, b27→l3+l8
- f3: b4→h12+h23+l3+l8, b17→l3+l8, b18→l3+l8, b21→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8
- test: b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8
- a0: number b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8; bank [l1 l2 l3 l8]: 11=0000, t18:=0011, t21:=0100/1100; flags b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8; index (any bit) → l3+l8; compiler used 11
- a1: number b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8; bank [l3 l8]: r=11, o=11, 10=11, 11=11, drc=11, lbl=11, 14=11, t15:=11, t16:=11, 17=11, t18:=11, 19=11, pa=11, 20=11, t21:=11, 22=11, 23=11, 24=11, 25=11, 26=11, 27=11, 28=11, 29=11, sa=11, 30=11, 31=11, x=11, g=11, c=11, i=11, #=11, p=11; flags b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8
- a2: bank [l3 l8]: r=00, o=11, 10=11, 11=11, drc=11, lbl=11, 14=11, t15:=11, t16:=11, 17=11, t18:=11, 19=11, pa=11, 20=11, t21:=11, 22=11, 23=11, 24=11, 25=11, 26=11, 27=11, 28=11, 29=11, sa=11, 30=11, 31=11, x=11, g=11, c=11, i=11, #=11, p=11; flags b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8; compiler used r
- a3: number b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8; bank [l3 l8]: r=00, o=11, 10=11, 11=11, drc=11, lbl=11, 14=11, t15:=11, t16:=11, 17=11, t18:=11, 19=11, pa=11, 20=11, t21:=11, 22=11, 23=11, 24=11, 25=11, 26=11, 27=11, 28=11, 29=11, sa=11, 30=11, 31=11, x=11, g=11, c=11, i=11, #=11, p=11; compiler used r
- a4: bank [l3 l8]: r=11, o=11, 10=11, 11=11, drc=11, lbl=11, 14=11, t15:=11, t16:=11, 17=11, t18:=11, 19=11, pa=11, 20=11, t21:=11, 22=11, 23=11, 24=11, 25=11, 26=11, 27=11, 28=11, 29=11, sa=11, 30=11, 31=11, x=11, g=11, c=11, i=11, #=11, p=11
- a5: bank [l3 l8]: r=11, o=11, 10=11, 11=11, drc=11, lbl=11, 14=11, t15:=11, t16:=11, 17=11, t18:=11, 19=11, pa=11, 20=11, t21:=11, 22=11, 23=11, 24=11, 25=11, 26=11, 27=11, 28=11, 29=11, sa=11, 30=11, 31=11, x=11, g=11, c=11, i=11, #=11, p=11
- a6: number b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8; bank [l3 l8]: r=00/11, o=11, 10=11, 11=11, drc=11, lbl=11, 14=11, t15:=11, t16:=11, 17=11, t18:=11, 19=11, pa=11, 20=11, t21:=11, 22=11, 23=11, 24=11, 25=11, 26=11, 27=11, 28=11, 29=11, sa=00/11, 30=11, 31=11, x=11, g=11, c=11, i=11, #=11, p=11; flags b0→l3+l8, b1→l3+l8, b2→l3+l8, b3→l3+l8, b4→l3+l8, b5→l3+l8, b6→l3+l8, b7→l3+l8, b8→l3+l8, b9→l3+l8, b10→l3+l8, b11→l3+l8, b12→l3+l8, b13→l3+l8, b14→l3+l8, b15→l3+l8, b16→l3+l8, b17→l3+l8, b18→l3+l8, b19→l3+l8, b20→l3+l8, b21→l3+l8, b22→l3+l8, b23→l3+l8, b24→l3+l8, b25→l3+l8, b26→l3+l8, b27→l3+l8, b28→l3+l8, b29→l3+l8, b30→l3+l8, b31→l3+l8; index (any bit) → l3+l8; compiler used r, sa

### hw 1f / useasm 0x9a: `wdf` (guess)

- in 97 corpus shaders: neg, abs, floor, fract, ceil, sqrt, rsq, exp2, log2, exp, log, sin …
- 2 distinct encodings, 2 fuzzed
- fixed bits: hi `f9200000` under mask `fffff7fe`, lo `00000000` under mask `ffffffff`
- f1: b3→h11
- a0: number b0→h0; compiler used 10
- a1: compiler used pa
- a2: compiler used #
- a3: compiler used #
- a4: compiler used 10
- a6: compiler used 32

### hw 1f / useasm 0xbe: `smoe` (guess)

- in 49 corpus shaders: neg, abs, floor, fract, ceil, sqrt, rsq, exp2, log2, log, sin, cos …
- 34 distinct encodings, 6 fuzzed
- fixed bits: hi `fa100000` under mask `fff0fff0`, lo `00000000` under mask `00000000`
- f1: b3→h18
- a0: number b0-2→l24..l26, b3→l26+l27, b4→l27+l28, b5→l29, b6→l28+l30, b7→l29, b9-10→l30..l31; bank [l0 l7 l8 l11 l13 l16 l24 l27 l28 l29 l30 l31 h3 h16 h17 h19]: r=1001100001000101/1100000001000101, o=1001100001100101/1100000001100101, t15:=0000010110001000/1000010110001000/1010010110001000, pa=1001100001010101/1100000001010101, sa=1001100001110101/1100000001110101, g=1100100001100111, c=1100000001100111, i=1001100001110111/1100000001110111, #=0000010010101000/1000010010101000/1010010010101000/1010011000000000; compiler used #, t15:
- a1: number b0-2→l16..l18, b3→l18+l19, b4→l19+l20, b5→l21, b6→l20+l22, b7→l21, b9-10→l22..l23; bank [l20 l22]: t15:=10, #=01; compiler used #, t15:
- a2: number b0-6→l8..l14; compiler used #
- a3: number b0-6→l0..l6; compiler used #
- a4: number b0→h3, b1→h3, b2→h3, b3→h3, b4→h3, b5→h3, b6→h3, b7→h3, b8→h3, b9→h3, b10→h3, b11→h3, b12→h3, b13→h3, b14→h3, b15→h3, b16→h3, b17→h3, b18→h3, b19→h3, b20→h3, b21→h3, b22→h3, b23→h3, b24→h3, b25→h3, b26→h3, b27→h3, b28→h3, b29→h3, b30→h3, b31→h3; compiler used #
- a5: number b0→h2, b1→h2, b2→h2, b3→h2, b4→h2, b5→h2, b6→h2, b7→h2, b8→h2, b9→h2, b10→h2, b11→h2, b12→h2, b13→h2, b14→h2, b15→h2, b16→h2, b17→h2, b18→h2, b19→h2, b20→h2, b21→h2, b22→h2, b23→h2, b24→h2, b25→h2, b26→h2, b27→h2, b28→h2, b29→h2, b30→h2, b31→h2; compiler used #
- a6: number b0→h1, b1→h1, b2→h1, b3→h1, b4→h1, b5→h1, b6→h1, b7→h1, b8→h1, b9→h1, b10→h1, b11→h1, b12→h1, b13→h1, b14→h1, b15→h1, b16→h1, b17→h1, b18→h1, b19→h1, b20→h1, b21→h1, b22→h1, b23→h1, b24→h1, b25→h1, b26→h1, b27→h1, b28→h1, b29→h1, b30→h1, b31→h1; compiler used #

### hw 1f / useasm 0xc1: `op0xc1` (guess)

- in 2 corpus shaders: sin_l, cos_l
- 1 distinct encodings, 1 fuzzed
- fixed bits: hi `fd100000` under mask `fffbffff`, lo `00000000` under mask `fffffefe`
- f1: b3→h18
- a0: number b0→l0; compiler used #
- a1: number b0→l8; compiler used #
- a2: compiler used 48
- a4: compiler used 32
- a6: compiler used r

### hw 1f / useasm 0xf1: `op0xf1` (guess)

- in 97 corpus shaders: neg, abs, floor, fract, ceil, sqrt, rsq, exp2, log2, exp, log, sin …
- 2 distinct encodings, 2 fuzzed
- fixed bits: hi `f8000140` under mask `ff7bf7ff`, lo `00000000` under mask `fffffffc`
- f1: b2→h23, b3→h11, b17→h18
- f2: b26→l0
- f3: b5→l1
- a0: compiler used r, pa
- a1: compiler used r, sa
- a2: compiler used r, sa
- a3: compiler used o, c
- a6: compiler used 32

