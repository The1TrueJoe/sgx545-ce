# SGX545 USSE instruction set (Cedarview DDK 1.7)

The 64-bit USSE instructions the DDK's shader compiler emits for the SGX545,
decoded from the compiler's own assembler. `dis.py` disassembles USP shader
binaries with it, `asm.py` assembles the same text, and both run on the
tables in `hw.py` + `hw_*.py`.

## How each fact was established

Every field below carries one of these tags:

- **encoder**: read from libusc's USE assembler (useasm.c, compiled into
  `libusc.so.1.7.862890`; `UseAssembleInstruction` at libusc + 0x109f50
  dispatches to one encoder per instruction class) in a Ghidra decompile, and
  confirmed by running that same encoder: `oracle.py` executes it in an i386
  emulator (unicorn), bit-exact against 5045 instructions logged from real
  compiles on the EA3. Each `check_<area>.py` sweeps the encoder over every
  field value of its forms and checks that `hw.py` decodes the result with
  no unexplained bit and re-encodes it from text. So an encoder field's
  *position and values* are facts; its *meaning* is only as good as the
  USEASM input that drives it (named below where libusc names it: asserts,
  flag names, its own pseudo-ops).
- **corpus**: what the GLSL compiler emits for a one-feature shader
  (`gen.py`, `gen2.py`, `gen3.py`; corpora `corpus/c1..c3`), or what
  toggling that feature changes.
- **hardware**: drawn on the EA3 (re/NOTES.md, the replayer).
- **writer / reader / driver**: read from libusc's USP writer, the GLES
  driver's USP loader, or the rest of libGLESv2 (finalizer, draw-time
  program builder, linker) and libglslcompiler, in Ghidra decompiles
  (USP sections only).
- **guess**: a name or meaning that fits but isn't backed by the above.

Function names are Ghidra's with its 0x10000 image base: `FUN_001147f0` is
libusc + 0x1047f0. Bit names: `l<n>` is bit n of the low word, `h<n>` of the
high word; an instruction is stored low word first.

## Tools

| file | what it does |
|---|---|
| `oracle.py` | libusc's encoder in an emulator: `Oracle().encode(USE_INST)` |
| `uinst.py` | build USEASM instructions (`U(op, A(type, n, flags, index), ..., f1=, f2=, test=)`), encode with the oracle |
| `hw.py`, `hw_*.py` | the instruction forms (framework + one module per group of classes); `decode`, `encode`, `fmt`, `parse` |
| `dis.py` | disassemble USP binaries (`python3 dis.py corpus/c3`) or hex words (`-x hi lo`) |
| `asm.py` | assemble dis.py's text; `--check` runs `hwcheck.py`. Also accepts the older fields.json syntax (`asm_fz.py`) for existing callers |
| `hwcheck.py` | every corpus word: has a form, no unexplained bit, text round-trips |
| `check_float.py`, `check_pack.py`, `check_int.py`, `check_mem.py` | oracle sweeps per area (run from the scratchpad with `LIBUSC=` and the venv that has unicorn) |
| `usp.py`, `smpblock.py` | the USP container and its texture-sample blocks |
| `gen3.py` | the texture / flow / int / index / vertex corpus (`corpus/c3`, USEASM log `corpus/encfuzz_c3.log.gz`) |
| `encfuzz.c`, `fz.py`, `mkfields.py`, `mkisa.py`, `fields.json`, `dis_fz.py`, `asm_fz.py`, `ISA_fz.md` | the first, bit-flip-fuzzing pass (superseded; kept for history) |

Status: all 4154 instructions (1570 distinct) of the three corpora decode
with no unexplained bit and round-trip (`python3 hwcheck.py`), and the four
oracle sweeps pass (about 20,000 encoder outputs).

## Instruction classes

h27-31 is the hardware opcode. One encoder per class:

| hw | class | forms | encoder | section |
|---|---|---|---|---|
| 00 | float 3-src | fmad fadm fmsa fsubflr | FUN_001147f0 | Float |
| 01 | float scalar | frcp frsq flog2 fexp2 fsqrt fsin fcos | FUN_001147f0 | Float |
| 02 | dot products | fdp fdpc fddp fddpc | FUN_001147f0, FUN_001159f0, FUN_00115d10, FUN_00115540 | Float |
| 03 | min/max | fmaxmin fminmax | FUN_001147f0 | Float |
| 04 | derivatives | fdsx fdsy | FUN_001147f0 | Float |
| 05 | moves | mov movc op0xf3 | FUN_00112370, FUN_0010ba40, FUN_00119b00 | Moves |
| 06 | f16 | fmad16 | FUN_001147f0 | Float |
| 07 | efo | efo | FUN_00116100 | Integer |
| 08 | pack | pck.<dst>.<src> | FUN_001165c0 | Pack |
| 09, 0f | test | test testmask | FUN_00112770 | Pack |
| 0a-0e | bitwise | and or xor shl rol shr asr rlp | FUN_00117270 | Pack |
| 10-1a | integer / fixed point | sop2 sop3 sopwm ima8 ima16 imae fpma dot3/dot4 ... | see Integer | Integer |
| 1b | dual issue | fdual | FUN_00113620 | Dual issue |
| 1c | sample | smp | FUN_0010a3f0 | Memory |
| 1d, 1e | memory | ld eld st | FUN_0010b190, FUN_0010ace0 | Memory |
| 1f | special | branches, MOE setup, wdf, nop, limm, ... | dispatcher + several | Memory |

## Registers

| prefix | file | encoding | notes |
|---|---|---|---|
| r | temporaries | 7-bit number | r124-r127 are the internal registers i0-i3: the encoder writes i<n> as r(124+n) in any slot that can't name the internal bank (**encoder**); under format select, numbers are 6 bits |
| pa | primary attributes | 7-bit | fragment: iterated varyings and non-dependent texture results; vertex: the vertex attributes; the fragment colour is assembled here (**corpus**). The budget is 0x80 minus USP header +0x3c (notes from the finalizer) |
| sa | secondary attributes | 7-bit | uniforms, texture state, results of the secondary update program (**corpus**) |
| o | outputs | 7-bit | vertex outputs; the finalised fragment colour (**hardware**) |
| c | hardware constants | number < 64 on the c/g bank | table below (**encoder**: libusc's regpack.c) |
| g | special registers | number + 0x40 on the c/g bank | g0-g45 (**encoder**: range); the corpus uses g17 as the per-instance stride when addressing temp arrays |
| i | internal | i0-i3 | bank 3 + ext in dst/src1/src2; fdual's secondary op works in i0-i2 |
| idx | index registers | idx1, idx2 (USEASM type 4, 1-3) | set by an integer op (ima16/imae into idx), then `bank[idxN+off]` (**encoder**, **corpus**) |
| p | predicates | p0-p3 | written by `test`, read by every instruction's predicate field |
| # | immediate | 7 bits in a source slot | bigger constants: `limm` (special class) or sa |

Indexed operands (**encoder**): any of r, o, pa, sa as `bank[idxN+off]`; the
7-bit number field holds bank(2) | index register(1) | offset(4) (offset 3
bits under format select). The destination's indexed bank is bank 3; a
source's is bank 0 + extension. src0 can't be indexed.

### Hardware constants (c<n>)

libusc's table of the hardware's special constants
(`USEASM_NUM_HW_SPECIAL_CONSTS`, regpack.c; **encoder**, values as libusc
believes them; c48 = 0.0, c52 = 1.0, c4 = 2.0, c14 = 0.5, c45 = F16 (1, 1)
also **hardware**/**corpus**):

| c | value | c | value | c | value |
|---|---|---|---|---|---|
| 0-2 | 0 | 3 | 1.0 | 4-13 | 2, 4, 8, ... 1024 |
| 14-27 | 0.5, 0.25, ... 2^-14 | 28 | e | 29 | 1/e |
| 30 | √2 | 31 | 1/√2 | 32 | π/4 |
| 33 | π/2 | 34 | π | 35 | 1/π |
| 36 | 2/π | 37 | 4/π | 38 | 2π |
| 39 | 2^-16 | 40-44 | sin/cos series coefficients (1.55e-6, 2.17e-5, 2.60e-4, 2.60e-3, 0.0208) | 45 | F16 pair (1.0, 1.0) (EURASIA_USE_SPECIAL_CONSTANT_FLOAT16ONE) |
| 46 | 0.5 | 47 | 1.0 | 48-51 | 0.0 (four, for vec4 repeats) |
| 52-55 | 1.0 (four) | 56 | 0xffffffff (EURASIA_USE_SPECIAL_CONSTANT_INT32ONE) | | |

## Float, moves and dual issue (hw 00-06, 1b)

### Common fields (all ALU classes)

| bits | field | meaning | evidence |
|---|---|---|---|
| h27-31 | opcode | hardware class | encoder |
| h24-26 | pred | 0 always, 1 p0, 2 p1, 3 p2, 4 p3, 5 !p0, 6 !p1, 7 pN (per-instance predicate) | encoder (FUN_00106f30 maps USEASM 0-7 to these codes); the pN name is a **guess** |
| h24-25 | pred (short form, fdual and some special ops) | 0 always, 1 p0, 2 p1, 3 !p0 | encoder |
| h23 | skipinv | skip invalid instances (libusc INST_SKIPINV; set on nearly every compiled instruction) | encoder; name from libusc's flag name |
| h20 | syncs | sync start (libusc INST_SYNCSTART) | encoder; name from libusc's flag name |
| h11 | nosched | no scheduling (libusc INST_NOSCHED, named in an asm.c assert) | encoder |
| h18 | end | last instruction of the program | encoder; hardware (the finalised probe programs end with it) |

### Operands (generic encoder FUN_00107830)

| slot | number | bank | ext | values |
|---|---|---|---|---|
| dst | l21-27 | h0-1 | h19 | r, o, pa, indexed (bank 3); with ext: sa, g, idx, i | 
| src0 | l14-20 | h2 | (none in float ops) | r, pa (o and sa need an ext bit only some classes have) |
| src1 | l7-13 | l30-31 | h17 | r, o, pa, sa; with ext: indexed, c/g, #imm, i |
| src2 | l0-6 | l28-29 | h16 | same as src1 |

All **encoder**. Indexed: number = bank(2) idx(1) offset(4): `pa[idx1+3]`; under
format select the offset has 3 bits. g (special registers) = number | 0x40
on the c/g bank; numbers 0-0x2d. Immediates 0-127. Internal registers in a
slot that can't hold them (src0; dst/src without ext) are r124+n: hardware
aliases i0-i3 onto r124-r127 (the encoder relies on it).

### Float classes (FUN_001147f0, hw 0-4, 6)

| class | sub-op | forms |
|---|---|---|
| hw0 | h9-10 | 0 fmad (s0*s1+s2), 1 fadm, 2 fmsa, 3 fsubflr (USEASM 0xcf "ffrc" = fsubflr(a, a) = fract) |
| hw1 | h9-10 + h16 | 0 frcp, 1 frsq, 2 flog2, 3 fexp2, 4 fsqrt, 5 fsin, 6 fcos |
| hw2 | h9-10 | 0 fdp (l14 clear) / fdpc (l14 set), 1 fddp, 2 fddpc |
| hw3 | h9 | 0 fmaxmin, 1 fminmax (three sources) |
| hw4 | h9 | 0 fdsx, 1 fdsy |
| hw6 | - | fmad16 |

All sub-op values **encoder**; hw1 names **corpus** (one-op shaders); hw0
fmad/fsubflr **corpus**, fadm/fmsa names **guess** (IMG enum order).
fminmax/fmaxmin: libusc writes fmin(a, b) as hw3 sub 1 on (a, a, b) and
fmax(a, b) as sub 0 on (a, b, a) (**encoder**: USEASM 0xc/0xd are rewritten
to 0x11/0x12); so sub 1 = min(max(s0, s1), s2), sub 0 = max(min(s0, s2), s1)
fit both (**guess** beyond those two uses).

Modifiers, 3-source ops (**encoder**): h7 = |src0|; src0 negate has no bit
(libusc folds it into src1's negate for fmad/fmad16/fddp and into src1 and
src2 for fadm; fmsa rejects it); h5/h6 = -src1 / |src1|; h3/h4 = -src2 /
|src2|. Two-source ops: src1 h5/h6, src2 h3/h4.

Repeat (FUN_00107080, SGX545 path, **encoder**): hw0/2/3/4: h12-13 =
repeat count - 1 (a write mask is turned into a count; at most 4). h21 = moe
(operands step by the MOE increments; libusc INST_MOE). With h21 clear,
h15/h14/h8 say whether the first/second/third source steps (USEASM flags2
bits 29/30/31; **corpus**: bit 30 appears with a register second source,
bit 31 with a register third source).
hw1 (scalar): h21 = rcnt; set: h12-15 = count - 1 (1-16); clear: h12-15 =
write mask.

h22 = fmt (format select, **encoder**): set when any source has USEASM
argument flag 22; every source number then has 6 bits and number bit 6
marks that source as F16 (`pa2.f16`). The destination is unaffected.

hw1 source format (**encoder**): h7 = F16 source (h3 picks the high half);
h8 = packed source with h3-4 picking one of four channels (USEASM argument
flag 24; c10/u8 **guess**). h2 = USEASM flags2 bit 22 (scalar ops only;
meaning **unknown**).

fmad16 (**encoder**): per-source swizzle (USEASM argument flags 25-26):
src0 h9-10, src1 h13 + h18, src2 h21-22 (so fmad16 has no end bit); repeat
1-2 in h12 (needs explicit increments h15/h14/h8). The four swizzle values
are libusc's FMAD16_SWIZZLE_{LOWHIGH?, LOWLOW, HIGHHIGH, CVTFROMF32} (names in
asserts; which value is which is a **guess**).

fdpc (**encoder**): fdp + clip-plane result cp0-7 (USEASM register type
0xd) in l15-17, l14 set. Needs a multi-channel repeat or mask.
fddp/fddpc (**encoder**): two dot products; the second result goes to i0/i1
(h19), so the destination has no extension bit. fddpc: clip planes in h3-5
and h6-8, no source modifiers.

### Moves (hw 5)

| h8-10 | form |
|---|---|
| 0 | mov d, s (src1 slot; src2 is the unused-source pattern: bank 2 + ext = #0, written by useasm's EncodeUnusedSource) |
| 1-5 | movc.<cond>.<type> d, s0, s1, s2: d = test(s0) ? s1 : s2 |
| 6, 7 | USEASM 0xf3 (target feature 0x2000; never emitted by libusc's compiler): writes an output; 7 has a src0, 6 none |

Repeat: h21 rcnt, h12-15 count-1 or write mask (**encoder**).
movc type (**encoder** for the bits; names from libusc's IMOVC variants in
asm.c): 1 u8, 2 i16 (**guess**: the fifth variant), 3 i32 (IMOVC_I32), 4
f32 (IMOVC), 5 c10. Condition h7 + h22 (**encoder**): 0 `z` (s0 == 0), 1
`nz`, 2 `ltz` (< 0), 3 `gez` (>= 0); these are the only USEASM test specs
movc accepts. USEASM test spec: bits 0-1 sign test (1 sign set, 2 sign
clear), bits 2-3 zero test (1 zero, 2 nonzero), bit 4 AND instead of OR
(libusc's own compare pseudo-ops pin the sign values; the "Pack/unpack, test, testmask, bitwise" section).
libusc's test types (asm.c g_auEncodeTest) are, in order: always, >0, >=0,
==0, <0, <=0, !=0, sign clear, sign set (specs 0x10, 0x1a, 0x6, 0x14, 0x19,
0x5, 0x18, 0x12, 0x11).

USEASM mov with a register-type-9 operand encodes on hw 1f (`f80000c0`:
type 9 <- src1; `f8000100`: dst <- type 9); see the "smp, ld/st, and the special class" section.

### Dual issue (hw 1b, FUN_00113620, **encoder**)

USEASM flags1 bit 25 pairs an instruction with the next (target feature
0x100). The primary op uses dst/src0/src1/src2 (dst has no ext bit); the
secondary computes into internal registers.

| bits | field | values |
|---|---|---|
| h24-25 | pred | short predicate |
| h18, h19, h26 | pri | 0 fmad, 1 mov, 2 fadm, 3 fmsa, 4 fddp, 5 fminmax, 6 USEASM 0x65, 7 USEASM 0xa7 |
| h6-9 | sec | 0 fmad, 1 fmad16, 2 fmsa, 3 fadm/fsubflr/ffrc, 4 fmaxmin, 5 frcp, 6 frsq, 7 fexp2, 8 flog2, 9 fsin, 10 fcos, 11 fadd, 12 fsub, 13 fmul, 14 USEASM 0x65, 15 mov |
| h5, h13 | sd | secondary destination (and its first source): 0 i0, 1 i1, 2 i2, 3 i0 with the first source taken from the src0 slot |
| h14-15 | sb | secondary second source: i0, i1, i2, or 3 = the primary's src1 |
| h10, h21 | sc | secondary third source: i0, i1, i2, or 3 = the src2 slot |
| h12 | rpt | 1-2 |
| h22 | fmt | format select as in the float classes |
| h4, h3 | neg1, neg2 | negate the src1 / src2 slot |

A one-source secondary op reads sb; a two-source one sb and sc. USEASM 0xa7
exists only as a dual-issue primary (libusc internal op 0x8e; the corpus
uses it once, in `v_norm`); 0x65 is described in the "Integer, fixed-point and multi-unit ops" section.

Checked: `check_float.py` (3217 oracle encodings over every operand type,
modifier, repeat/mask/increment, predicate, flag, swizzle, movc type and
condition and fdual pairing) and `hwcheck.py` (all corpus words of these
classes): no unexplained bit, every word round-trips.

## Pack/unpack, test, testmask, bitwise (hw 08, 09, 0a-0e, 0f)

Forms: `hw_pack.py`. Checks: `check_pack.py` (oracle sweep, 1579 encodings,
0 failures) and `hwcheck.py` (all 281 pack, 107 test, 34 testmask corpus words
decode with no unexplained bits and round-trip).

Encoders (Ghidra addresses = libusc vaddr + 0x10000):

| function | what | USEASM ops |
|---|---|---|
| FUN_001165c0 | pack/unpack, hw 08 | 0x14-0x4e, 0x16f, 0x170 |
| FUN_00112770 | test (hw 09) / testmask (hw 0f) | any ALU op with flags1 b30 (TEST); the dispatcher FUN_00119f50 sends it there before anything else |
| FUN_00117270 | bitwise, hw 0a-0e | 0x4f-0x56 |

Operand slots come from the generic encoder FUN_00107830: src1 via
FUN_001080b0 (l7-13, bank l30-31, ext h17), src2 via FUN_00108040 (l0-6, bank
l28-29, ext h16), dest via FUN_0010b9d0 (l21-27, bank h0-1, ext h19). None of
these classes has a src0 slot. Predicate h24-26 via FUN_00106f30 (PRED3).

Evidence tags: **encoder** = read from the decompile and confirmed with the
oracle; **corpus** = what GLSL produces it; **guess** = name/meaning not backed.

### Shared ALU control bits (all three encoders)

| bits | field | USEASM source | tag |
|---|---|---|---|
| h24-26 | pred | flags1 b13-16 | encoder |
| h23 | skipinv | flags1 b0 | encoder |
| h22 | nosched | flags1 b3 (h22 here, not h11 as in the float classes) | encoder |
| h20 | syncs | flags1 b2 | encoder |
| h18 | end (pck, bitwise only) | flags1 b17 (test rejects it) | encoder |
| h21 | rcnt (pck, bitwise) | set when flags1 repeat count n>0 | encoder |
| h12-15 | rpt = n (stored n-1) when rcnt; else mask = flags1 b9-12 verbatim | flags1 b4-8 / b9-12 | encoder; "mask" = per-iteration enable mask is a **guess** (compiler always writes 1) |

### hw 08: `pck.<dst>.<src>` (FUN_001165c0)

Format codes (both fields): 0 u8, 1 s8, 2 o8, 3 u16, 4 s16, 5 f16, 6 f32,
7 c10. The code -> name map is **encoder** (it follows the USEASM op enum,
whose order PCK<dst><src> is fixed by the corpus: 0x20 = F32->U8 colour
writes, 0x4e = C10 lowp, 0x14 = F32->F16 for mediump); "o8" as an
offset/biased 8-bit format is a **guess**.

| bits | field | values | tag |
|---|---|---|---|
| h27-31 | opcode | 8 | encoder |
| h9-11 | source format | as above | encoder |
| h6-8 | destination format | as above | encoder |
| h2-5 | dmask | destination byte write mask, USEASM arg0 flags b7-10. Encoder limits: dst u16/s16/f16 only 3, 0xc, 0xf; f32 only 0xf; 8-bit and c10 any. A partial mask needs a non-indexed dest, and on an `i` dest only c10 takes one | encoder; corpus (`pck.u8.f32 … dmask=6/9` pairs build RGBA8 two bytes at a time) |
| l16-17 | src1 lane (`.l<n>`) | USEASM arg1 flags b5-6 | encoder; meaning by source format is corpus: 8-bit sources pick a byte (`pck.u8.u8 pa9, pa9.l3`), 16-bit sources use 0/2 = low/high half (`pck.f16.f16 pa9, r0, pa9.l2`), c10 picks the 10-bit channel (`pck.f16.c10 pa11, i2.l2, i2.l1`) |
| l14-15 | src2 lane | USEASM arg2 flags b5-6 | encoder, as above |
| l18 | `scale` (pairs with one float side): flags2 b8. `rint` (float->float pairs): flags1 b24. Int->int pairs: encoder never sets it | encoder for the sources. scale = normalise (corpus: every F32->U8 colour write has it). rint: libusc's own trunc (intermediate op 0x96, the fast path of icvt_i32.c's TRC, `psTrcSrc` assert) is `pck.f32.f32 [rint rnd=zero]` -> round to integral with `rnd` (**encoder**+compiler code; the name is ours) |
| l19-20 | rnd | arg3 must be type 0x10: USEASM 0x3a -> 0, 0x39 -> 1, 0x3b -> 2, 0 -> 3. Required for float->float pairs; with `scale` on float<->int pairs; otherwise not encoded | encoder for the mapping. 0 = nearest (compiler default, used for every corpus conversion), 3 = toward zero (the trunc above) are compiler-backed; 1 = down, 2 = up is a **guess** (x86 RC order) |
| l21-27, h0-1, h19 | dst | r o pa sa g i ix | encoder |
| l7-13, l30-31, h17 | src1 | r o pa sa c g # i ix | encoder |
| l0-6, l28-29, h16 | src2 | same | encoder |

Notes:
- Two sources: the compiler packs two values per instruction (corpus:
  `pck.u8.f32 pa9, pa9, pa13 [dmask=6]` then `... [dmask=9]` builds RGBA8),
  and a one-value conversion passes #0 as src2. Which source lands in which
  enabled lane (src1 low, src2 high) is a **guess**.
- Immediates are 7 bits. The encoder range-checks them as signed for USEASM
  ops that unpack from s8/o8/c10/s16 (local_44 = 1: 0x35-0x37, 0x3a, 0x3c-0x3e,
  0x41, 0x43-0x45, 0x48, 0x4a-0x4c) and unsigned otherwise; the bits are the
  same, so the text shows the raw 7-bit value.
- USEASM ops -> form (several USEASM ops share a format pair; the 0x34-0x4c
  "unpack" half only differs from the pack half in that immediate check):

0x14 `pck.f16.f32` (corpus), 0x15 `pck.f16.f16` (corpus), 0x16 `pck.f16.u16`,
0x17 `pck.f16.s16`, 0x18 `pck.u16.f32`, 0x19 `pck.u16.f16`, 0x1a `pck.u16.u16`,
0x1b `pck.u16.s16`, 0x1c `pck.s16.f32` (corpus), 0x1d `pck.s16.f16`,
0x1e `pck.s16.u16`, 0x1f `pck.s16.s16`, 0x20 `pck.u8.f32` (corpus),
0x21 `pck.u8.f16` (corpus), 0x22 `pck.u8.u16`, 0x23 `pck.u8.s16`,
0x24 `pck.s8.f32`, 0x25 `pck.s8.f16`, 0x26 `pck.s8.u16`, 0x27 `pck.s8.s16`,
0x28 `pck.o8.f32`, 0x29 `pck.o8.f16`, 0x2a `pck.o8.u16`, 0x2b `pck.o8.s16`,
0x2c `pck.c10.f32` (corpus), 0x2d `pck.c10.f16`, 0x2e `pck.c10.u16`,
0x2f `pck.c10.s16`, 0x30 `pck.f32.f32`, 0x31 `pck.f32.f16` (corpus),
0x32 `pck.f32.u16`, 0x33 `pck.f32.s16`, 0x34 `pck.f32.u8`, 0x35 `pck.f32.s8`,
0x36 `pck.f32.o8`, 0x37 `pck.f32.c10`, 0x38 `pck.f16.f16`, 0x39 `pck.f16.u16`,
0x3a `pck.f16.s16`, 0x3b `pck.f16.u8`, 0x3c `pck.f16.s8`, 0x3d `pck.f16.o8`,
0x3e `pck.f16.c10` (corpus), 0x3f `pck.u16.f16`, 0x40 `pck.u16.u16`,
0x41 `pck.u16.s16`, 0x42 `pck.u16.u8`, 0x43 `pck.u16.s8`, 0x44 `pck.u16.o8`,
0x45 `pck.u16.c10`, 0x46 `pck.s16.f16`, 0x47 `pck.s16.u16`, 0x48 `pck.s16.s16`,
0x49 `pck.s16.u8`, 0x4a `pck.s16.s8`, 0x4b `pck.s16.o8`, 0x4c `pck.s16.c10`,
0x4d `pck.u8.u8` (corpus), 0x4e `pck.c10.c10` (corpus), 0x16f `pck.u8.s8`,
0x170 `pck.s8.u8`. (ISA.md's `op0x14` etc. are these; its 0xef also lands here
via the dispatcher's rewrite to 0x40.)

### hw 09 `test`, hw 0f `testmask` (FUN_00112770)

Any ALU USEASM op with flags1 b30 goes here; dw4 (the test spec) picks the
condition and test vs testmask. Text:

    test.<alu>.<cond> pN, [dst,] src1, src2      [chan= mask= fmt f2_0 ...]
    testmask.<alu>.<cond>.<type> dst, src1, src2

| bits | field | values | tag |
|---|---|---|---|
| h27-31 | opcode | 9 = test (spec b9-11 = 0), 0xf = testmask (spec b9-11 != 0) | encoder |
| l14-19 | alu | ALU op = the op the USEASM instruction names; l18-19 = group (0 float, 1 int16/32, 2 8-bit/fixed, 3 bitwise), l14-17 the op in the group. Table below | encoder |
| h10-11 | cond: sign test | spec b0-1: 0 none, 1 sign bit set, 2 sign bit clear, 3 never emitted | encoder; meaning corpus |
| h8-9 | cond: zero test | spec b2-3: 0 none, 1 zero, 2 nonzero, 3 never emitted | encoder; meaning corpus |
| h7 | cond: combine | spec b4: 0 OR, 1 AND | encoder; meaning corpus |
| h4-6 | chan (test only) | spec b5-8 (0-7, 8+ rejected); which channel's result is tested | encoder; values 0-3 = channel is a **guess**, 4-7 (and/or over channels?) **guess**; corpus uses 0, and 2 for lowp c10 compares |
| h2-3 | pdst (test only) | USEASM arg1, type 0xc, p0-p3 | encoder |
| h4-5 | type (testmask only) | spec b9-11: 1 -> 0 `byte`, 2 -> 1 `word`, 3 -> 2 `dword`; others rejected | encoder; 3 = USEASM_TEST_MASK_DWORD (libusc assert string) and the only one the corpus uses; byte/word are **guess** |
| h12-15 | mask | flags1 write mask verbatim (fdp: USEASM 0xc2 -> 7, 0xc3 -> 0xf). testmask also takes a repeat count n -> mask (1<<n)-1 (n <= 4); test rejects a repeat. A test with mask != 1 must write p0 | encoder; corpus: testmask mask=15 for bvec4 compares |
| l20 | dest written | clear when USEASM arg0 has flag b16; then the dest field holds h0 + h19 (`nodst` = 0x280) | encoder; corpus: every `test` has no dest |
| h18 | fmt | set when a source has arg flag b22 (float group, F16) or b24 (group 2 ops 0xf7-0xf9, C10; the dest flag counts there too). Sources and dest then have 6-bit numbers + bit 6 = per-operand format, printed `.f16` / `.c10` | encoder; corpus (`test.fsub.ge p1, sa20.f16, sa24.f16`, `test.fpsub8.lt p0, pa4.c10, pa5.c10`) |
| h21 | f2_0 | USEASM flags2 b0 | encoder; corpus sets it only on the counter test of fixed-count loops (loop4, lp_s4/s32/s100, lp_nest, tx_dep_loop): meaning unknown |
| h22, h23, h20, h24-26 | nosched, skipinv, syncs, pred | as above | encoder |
| dst / src1 / src2 | dst l21-27 h0-1 h19 (USEASM arg0), src1 = arg1 (testmask) / arg2 (test), src2 = next arg. One-source ops (frcp ... fexp2, fadd from 0xa4) get src2 = #0; fsubflr from 0x3 repeats src1 | encoder |

No negate/abs bits: the only source flags accepted are the format bits; a
subtract is the `fsub` ALU op (USEASM 0xa9 = 0xa6 with src2's negate flag).

#### ALU op field (l14-19)

| value | group | sub | mnemonic | USEASM ops | tag |
|---|---|---|---|---|---|
| 0x00 | 0 | 0 | `fadd` | 0xa6 (a+b), 0xa4 (fmov: a + #0) | encoder (dispatcher rewrites 0xa4/0xa5/0xa6/0xa9 to fmad with 1.0/0.0, giving the names) |
| 0x03 | 0 | 3 | `fsubflr` | 0x3, 0xcf (ffrc) | encoder |
| 0x04-0x07 | 0 | 4-7 | `frcp frsq flog2 fexp2` | 0x4-0x7 | encoder |
| 0x08 | 0 | 8 | `fdp` | 0xb, 0xc2, 0xc3 | encoder |
| 0x09, 0x0a | 0 | 9, 10 | `fmin fmax` | 0xc, 0xd | encoder |
| 0x0b, 0x0c | 0 | 11, 12 | `fdsx fdsy` | 0xe, 0xf | encoder |
| 0x0d | 0 | 13 | `fmul` | 0xa5 | encoder |
| 0x0e | 0 | 14 | `fsub` | 0xa9; also 0xaa-0xaf (below) | encoder, corpus |
| 0x16-0x1b | 1 | 6-11 | `iadd16 isub16 imul16 iaddu16 isubu16 imulu16` | 0xe4-0xe9 | encoder: outside TEST these become ima16 a*1+b, a*1-b, a*b+0 with flags2 0x1000 (0xe4-0xe6) or 0x800 (0xe7-0xe9); signed/unsigned from the signed-immediate check is a **guess** |
| 0x1c, 0x1d | 1 | 12, 13 | `iadd32 iaddu32` | 0xea, 0xf0 | encoder (imae a*1+b); names **guess** |
| 0x20-0x26 | 2 | 0-6 | `i8op0`..`i8op6` | 0xeb 0xec 0xf5 0xf6 0xed 0xf7 0xee | encoder; 0xeb/0xec/0xf5/0xf6 become ima8 (add / swapped-negated sub, 0xf5/0xf6 with flags2 0x400); 0xed/0xee are TEST-only. Names unknown |
| 0x27 | 2 | 7 | `fpadd8` | 0xf8 | **guess** (TEST-only, C10-capable; libusc has an IFPADD8) |
| 0x28 | 2 | 8 | `fpsub8` | 0xf9 | corpus: every lowp compare (fl_*_l) is 0xf9 with C10 sources; name **guess** after libusc's IFPSUB8 |
| 0x30-0x35, 0x37 | 3 | 0-5, 7 | `and or xor shl shr rol asr` | 0x4f 0x50 0x51 0x52 0x53 0x55 0x54 | encoder; corpus (`test.and.ne p0, #0, #0` clears p0; `test.shl.pos/neg p2, x, #31` in step_l) |

Other values print as `alu<hex>`; the encoder never produces them.

#### Conditions, and what GLSL produces

`cond` is printed by name for the six comparisons and the two plain sign
tests, else as `s<sign>z<zero><a|o>` (e.g. `s1z0o`). The test applies to the
ALU result, so for `fsub a, b` it compares a with b.

| spec / cond | sign | zero | comb | name | meaning on a-b | corpus |
|---|---|---|---|---|---|---|
| 0x19 | set | nonzero | AND | `lt` | a < b | fl_lt, ic_lt, if_lt, loop counters; `testmask` in lt, ic_ltv |
| 0x05 | set | zero | OR | `le` | a <= b | fl_le, ic_le, dc_gt (discard if a > b writes p1 = a <= b) |
| 0x14 | none | zero | AND | `eq` | a == b | fl_eq, ic_eq, if_eq, ic_eqv (testmask) |
| 0x18 | none | nonzero | AND | `ne` | a != b | fl_ne, ic_ne, dc_eq; `fadd.ne x, c48` = x != 0 (fl_ne0, cv_ftob, bool tests) |
| 0x1a | clear | nonzero | AND | `gt` | a > b | fl_gt, ic_gt, if_gt, bo_* (`greaterThan` via testmask) |
| 0x06 | clear | zero | OR | `ge` | a >= b | fl_ge, ic_ge, step (testmask) |
| 0x11 / 0x12 | set / clear | none | AND | `neg` / `pos` | sign bit | step_l (`shl` by 31 then sign test) |

So "sign clear" includes +0 (gt needs the extra nonzero), and "none" passes
under AND. libusc's own compare pseudo-ops confirm the table: USEASM 0xaa gt,
0xab ge, 0xac eq, 0xad lt, 0xae le, 0xaf ne are rewritten by FUN_00119f50 to
TEST fsub with spec 0x1a, 6, 0x14, 0x19, 5, 0x18 (and arg0 flag b16, no
dest); 0xb8/0xb9 to TEST fadd x, #0 with eq/ne. `if`/`discard`: the compiler
writes p0 (branch) or p1 (discard, with the inverted condition: keep the
pixel when the test passes), e.g. dc_lt0 `test.fadd.ge p1, pa4, c48`,
dc_always `test.fadd.ne p1, c48, c48` (always false).

#### test vs testmask

`test` writes predicate pdst from one channel (chan) of the ALU result, or
with mask > 1 one predicate per channel starting at p0 (the encoder forces
pdst = p0 then; guess that channel n writes pn). The ALU result itself is
also written when there is a dest (l20).

`testmask` has no predicate: it writes into dst, per enabled channel (mask), a
mask of the given width that is all ones when the test passes and zero
otherwise (corpus: every testmask is followed by `movc.nz.i32 d, d, c52, c48`
= select 1.0/0.0, e.g. step, lt, bo_xor, ic_eqv, and vo_if's `movc.z.i32`
select). The all-ones/zero reading is a **guess** consistent with movc.nz.

### hw 0a-0e: bitwise (FUN_00117270)

| hw | h3 | mnemonic | USEASM |
|---|---|---|---|
| 0a | 0 / 1 | `and` / `or` | 0x4f / 0x50 |
| 0b | 0 | `xor` | 0x51 |
| 0c | 0 / 1 | `shl` / `rol` | 0x52 / 0x55 |
| 0d | 0 / 1 | `shr` / `asr` | 0x53 / 0x54 |
| 0e | - | `rlp` | 0x56 |

All encoder; no corpus instance (the compiler only uses these ALU ops under
TEST). Mnemonic names follow the USEASM op order and the TEST ALU codes
(**guess** for the exact semantics of rol/asr/rlp).

| bits | field | values | tag |
|---|---|---|---|
| h27-31, h3 | opcode, sub-op | table above | encoder |
| h11 | inv (`.inv`) | src2 inverted: USEASM arg2 flag 0x800 (src1 takes no flags) | encoder |
| h2 | partial | USEASM flags1 b20; also stops the immediate compression | encoder; "16-bit/partial operation" meaning **guess** |
| h6-10 | rot | immediate rotate-left count | encoder |
| l0-6, l14-20, h4-5 | src2 immediate | when src2's bank is `#` (l28-29 = 2, h16): a 16-bit value, bits 0-6 / 7-13 / 14-15 | encoder |
| h21, h12-15 | rcnt, rpt/mask | as above | encoder |
| h18, h20, h22, h23, h24-26 | end, syncs, nosched, skipinv, pred | as above | encoder |
| dst / src1 / src2 | arg0 / arg1 / arg2, generic slots | encoder |

Immediate compression (encoder, oracle-checked for 32 values): a 32-bit
USEASM immediate v (not rlp, not partial) is stored as (n, rot, inv) with
v = rol(n, rot), n < 0x10000, smallest rot; if no rotation fits, the same
search runs on ~v and inv is toggled (so `and r, r, #0xfffffffe` is
`and.inv … #1`). Not representable -> encoder error. With partial, n = v & 0xffff,
no rotate. The text shows n, rot and inv separately (lossless):
`and r5, r8, #0xff00  [rot=16]` = r8 & 0xff000000.

`rlp dst, src1, pN` (hw 0e): USEASM arg2 must be a predicate (type 0xc, p0-p3)
-> l14-15; src2's slot is encoded as unused (#0: l28-29 = 2, h16 = 1; field
`unused2` = 0x300); h3, h4-11 never set. Rotate-left-through-predicate is a
**guess** from the name.

### Open questions

- f2_0 (test h21, USEASM flags2 b0): only on fixed-trip-count loop counter
  tests. Meaning unknown.
- partial (bitwise h2, USEASM flags1 b20): compiler never sets it.
- test chan values 4-7, testmask type 0/1 (byte/word) and value 3, the
  sign/zero test value 3: encoder emits 4-7 / 0-2 but nothing names them;
  3 is never emitted.
- group-2 ALU ops (0x20-0x26) names; whether 0xf8/0xf9 are FPADD8/FPSUB8.
- rnd 1/2 = down/up is a guess; the pck `scale`/`rnd` bits on int->int pairs
  are decoded but the encoder never sets them.
- Immediate sign for pck sources is an assembler range check only.

## Integer, fixed-point and multi-unit ops: hw 07, 10-19, 1a

Forms: `hw_int.py`. Checks:
- `check_int.py`: an oracle sweep of 1602 cases, every field value of every form, 0 failures.
- `hwcheck.py`: all corpus words with hw 07 (104 distinct), 10, 11, 12, 14, 15, 18 and 19 decode with no unexplained bits and round-trip.

A crossover fuzz of the check cases also passed: 30000 encodes, 0 unexplained bits, 0 round-trip failures.

Evidence tags:
- **encoder**: read from the decompile and confirmed with the oracle.
- **corpus**: a GLSL shader in c1/c2/c3 produces it.
- **usc**: libusc's own compiler (usc.c, `tools/intern/usc2/asm.c`) names it or builds it that way.
- **guess**: not backed.

Function names are Ghidra addresses (libusc vaddr + 0x10000).

### USEASM opcode -> form

| USEASM | encoder | hw | form |
|---|---|---|---|
| 0x13 | FUN_00116100 | 07 | efo |
| 0xb0-0xb6 | dispatcher rewrites to 0x13 + a fixed control word | 07 | rejected on SGX545 (FUN_0011e020 = word0 & 0x400 is set) |
| 0x57 + coissued 0xe3 | FUN_0010bea0 | 10 | sop2 |
| 0x59 + 0xde / 0xdf | FUN_001178e0 | 11 (h21=0, h10=0) | sop3 |
| 0x59 + 0xe0 | FUN_001178e0 | 11 (h21=0, h10=1) | sop3l |
| 0xe1 + 0xde | FUN_0010db30 | 11 (h21=1, h10=0) | lrp1 |
| 0xe2 + 0xe0 | FUN_0010d120 | 11 (h21=1, h10=1) | sop3x |
| 0x58; 0xfa | FUN_0010c9e0 | 12 | sopwm (0xfa = 0x58 with c1=s1a, c2=1-s1a, add, add) |
| 0x62; 0xeb 0xec 0xf5 0xf6 | FUN_0010f1a0 | 13 | ima8 (0xeb: a*#1+b; 0xec: -b*#1+a; 0xf5/0xf6 the same plus flags2 b10) |
| 0xf4 | FUN_0010f1a0 | 19 | fpma |
| 0x63; 0xe4-0xe9 | FUN_0010f960 | 14 | ima16 (0xe4/0xe7 a*#1+b, 0xe5/0xe8 a*#1-b, 0xe6/0xe9 a*b+#0; 0xe4-0xe6 signed via flags2 0x1000, 0xe7-0xe9 0x800) |
| 0x64; 0xea 0xf0 | FUN_00110060 | 15 | imae (0xea/0xf0 = a*#1+b with src2 u32, signed/unsigned) |
| 0x5a-0x61 | FUN_0010e460 | 18 | dot3 / dot4 / dot3s / dot4s |
| 0x67 0x68 0x69 0x6a 0x6b 0x6e | see hw 16 below | 16 | adif adifsum ssum16 ssum16i bilin op16s1 |
| 0x6c; 0x6d (inline) | FUN_00111850; FUN_00119f50 | 17 | firv; firh |
| 0x65; 0x167/0x168 | FUN_00118650 | 1a | ima32 (0x167/0x168 = a*#1 + -b, signed/unsigned) |
| 0x66 (inline) | FUN_00119f50 | 1a | op1adrc |

The coissued second instruction is a separate USE_INST. It hangs off dw61 (+0xf4) and is flagged by flags1 b25. The two encode into one 64-bit word.

The sop2, sop3, lrp1, sop3x and dot "s" forms require the coissue: the encoder rejects them without it. For dot it is optional.

Libusc never emits 0x6a, 0x6d, 0x6e or 0x66 for SGX545: the feature checks reject them. They were swept with the emulated target patched (feature word0 |= 2, core flags &= ~0x14000), and are tagged **encoder (patched)**.

### Fields shared by hw 10-19 and 1a (encoder)

| bits | field | source |
|---|---|---|
| h27-31 | opcode | constant |
| h25-26 | pred, short form (none, p0, p1, !p0) | FUN_00106f30(flags1, 1) << 25; USEASM preds 4-7 rejected |
| h23 | skipinv | flags1 b0 |
| h22 | nosched | flags1 b3 << 19 |
| h18 | end | flags1 b17 << 1 |
| h12-14 | rpt - 1 (1..8) | flags1 b4-8; sop3 / sopwm have no repeat |

flags1 b2 (sync) is rejected. With flags2 b1 the dispatcher emits nothing, for every class.

The sop3 and sopwm encoders insist on flags1 mask = 0x200 (one channel); the others ignore the mask.

Operand slots come from the generic encoder FUN_00107830:
- dst: l21-27, bank h0-1, ext h19 (BANK_DST).
- src0: l14-20, bank h2. FUN_00109550 with ext disallowed takes r/pa only; an internal register becomes r124+n.
- src1: l7-13, bank l30-31, ext h17 (BANK_SRC: r o pa sa, ix, c/g, #, i).
- src2: l0-6, bank l28-29, ext h16.
- An index operand number is [bank 2][idx 1][offset 4].

**Operand bit 6.** In sop2/sop3/sopwm/dot/ima8/fpma, USEASM flags2 b24 makes every number 6 bits. Bit 6 is then the per-operand format flag (USEASM arg flag b24, C10 vs U8).

In efo the same happens for F16 sources (arg flag b22). There is no instruction bit that records this, so the decoder prints the raw 7-bit number. For example, the corpus's `pa64` and `i64` are pa0 and i0 with the format flag set.

All corpus sop/dot/fpma words come from programs with flags2 b24. So a backend that wants r64-r127 in these classes cannot also use per-operand C10/F16. Whether the hardware takes a global mode from somewhere (the program header?) is **unknown**.

INTSRCSEL (USEASM register type 0x10) values met here. The numbers are **encoder**. The names marked * are named in libusc asserts; the rest are **guess**.

| value | name | value | name |
|---|---|---|---|
| 0 | zero* | 1 | one (= complement of zero) |
| 2 | asat (src alpha saturate) | 3 | complement of 2 |
| 4 | src0* | 5 | src1 |
| 6 | src2 | 7 | src0alpha* |
| 8 | src1alpha | 9 | src2alpha* |
| 0xa-0xd | cx1..cx8 (dot colour scale) | 0xe-0x11 | ax1..ax8 (dot alpha scale) |
| 0x12 | add* | 0x13 | sub |
| 0x14 | neg | 0x15 | none |
| 0x16 | min | 0x17 | max |
| 0x18 / 0x19 | k18 / k19 (sop2 extra c1/c2 select) | 0x1a | complement |
| 0x1e / 0x1f / 0x20 | u8 / s8 / o8 | 0x27 / 0x28 / 0x29 | u16 / s16* / u32* |
| 0x2a / 0x2b | carry in / carry out | 0x2c | 16-bit format (ima16) |

Arg flag b15 (0x8000) is the complement of a select. The encoder folds it in as select ^ complement, so ONE = complement of ZERO.

In the forms, a select field's spec ends with its complement bit. Complemented values print as `1-x` (`one`/`zero` for 1-zero/1-one).

### efo (hw 07, USEASM 0x13, FUN_00116100)

The encoder ORs USEASM arg4 (an immediate) straight into the high word. usc's asm.c builds that immediate from psEfo (usc.c ~1926-2400; SGX545 takes the `bNewEfoFeature` branch). The field meanings below are asm.c's.

Units:
- M0, M1: multipliers.
- A0, A1: adders.
- I0, I1: the internal registers they feed back into.

| bits | field | values | tag |
|---|---|---|---|
| h24-25 | pred (short form) | | encoder |
| h23 | skipinv | flags1 b0 | encoder |
| h11 | nosched | flags1 b3 << 8 | encoder |
| h12-13 | rpt-1 (1..4) | | encoder |
| h20-21 | d: what the dest gets | 0 i0, 1 i1, 2 a0, 3 a1 | usc + corpus |
| h18-19 | i: internal writes | 0 i0=a0 i1=a1; 1 i0=a1 i1=a0; 2 i0=m0 i1=m1; 3 i0=a0 i1=m1 | usc |
| h16-17 | a: adder inputs | 0 a0=m0+m1 a1=i1+i0; 1 a0=m0+s0 a1=i1+i0; 2 a0=m0+i0 a1=i1+m1; 3 a0=s0+s2 a1=s1+s2 | usc |
| h14-15 | m: multiplier inputs | 0 m0=s0*s2 m1=s1*s2; 1 m0=s2*s1 m1=s2*s2; 2 m0=s1*s0 m1=s2*(s2?); 3 m0=s1*i0 m1=s2*i1 | usc (mode 2 m1 only asserted to involve s2) |
| h10 / h9 | wi0 / wi1: write I0 / I1 | | usc |
| h26 | a0rneg: negate A0's right input | | usc assert `bA0RightNeg`; corpus `mix`: m0=-x*a, a0=m0-(-x) = x(1-a) |
| h22 | a0lneg | usc copies an add's other source negate here | **guess** (name) |
| h8 | f3_10 | USEASM flags3 b10 | encoder; meaning unknown |
| h5 h4 h3 | src0/src1/src2 negate | arg flag b0 | encoder |

Operands:
- dst: l21-27, bank h0-1 (r o pa ix), no ext.
- src0: l14-20, bank h2, ext h6 (r pa o sa).
- src1: l7-13, bank l30-31, no ext (r o pa sa).
- src2: l0-6, bank l28-29, ext h7 (all kinds).
- Internal registers in the dest, src0 and src1 slots become r124+n.

The pre-SGX545 EFO uses h3-8 as three 2-bit source modifiers; this core uses the h5/h4/h3 + ext layout. The usc control-word mapping is two-variant as well.

Corpus control words and their GLSL:

| ctl | decoded | shaders |
|---|---|---|
| 0x2b0600 | d=a0, i=m0/m1, a=s0+s2/s1+s2, m=s0*s2/s1*s2 | exp log cos norm3 kmul_*: two mads at once (dst = s0+s2, I0/I1 = the products) |
| 0x230600 | d=a0, i=a0/a1, a=s0+s2/s1+s2 | ftoi and int compares: x+1 for two lanes (`efo pa2, pa8, pa9, c52` then movc.ltz); bo_not: 1-x |
| 0x180600, 0x20600, 0x220200 | m products into I, then a0=m0+i0, a1=i1+m1 | v_mvp: dot-product accumulation across 3 efos |
| 0x42d0600 / 0x43d0600 / 0x14600 | a0rneg | mix, fl_vsel: x(1-a) + y*a |
| 0x200000 | d=a0, a0=m0+m1, no I writes | ix_loop, vix_loop |
| 0x2b8600 | m mode 2 | rv_aa |

### sop2 (hw 10, USEASM 0x57 + 0xe3, FUN_0010bea0)

The encoder requires the coissue. `dst.rgb = (src1 * c1) cop (src2 * c2)` and `dst.a = (src1.a * a1) aop (src2.a * a2)` (Eurasia SOP2, **guess**). Operands are dst, src1 and src2; there is no src0.

| bits | field | source | tag |
|---|---|---|---|
| h6-8, h24 | c1: 0 zero 1 s1 2 s2 3 s1a 4 s2a 5 asat 6 k18; h24 complement | arg4 | encoder |
| h3-5, h15 | c2: same, 6 = k19 | arg5 | encoder |
| l18-19 | cop: add sub min max | arg6 | encoder |
| l20 | cs1c: complement src1 (colour) | arg3 0x1a (else none 0x15) | encoder; usc bComplementCSrc1 |
| h20-21, h11 | a1: zero s1a s2a k18 + complement | 0xe3 arg1 | encoder |
| h9-10, h2 | a2: zero s1a s2a k19 + complement | 0xe3 arg2 | encoder |
| l16-17 | aop | 0xe3 arg3 | encoder |
| l15 | as1c: complement src1 (alpha) | 0xe3 arg0 0x1a | encoder; usc bComplementASrc1 |
| l14 | aneg: negate alpha result | 0xe3 arg4 0x14 | encoder |

The only corpus use is `mov_l`/`fl_gt_l`: `sop2 d, a, b [c1=one c2=zero a1=zero a2=one]`, a lowp select or move.

### sopwm (hw 12, USEASM 0x58 / 0xfa, FUN_0010c9e0)

`dst = (src1 * c1) cop (src2 * c2)` per byte, with a write mask. There is no coissue and no repeat; flags1 mask must be 0x200.

| bits | field | source | tag |
|---|---|---|---|
| h12,h13,h14,h11 | mask, in USEASM order x y z w (stored rotated); default 15 | dest arg flags b7-10 with flag b20; a partial mask needs a non-indexed dest | encoder; usc puts auDestMask << 7 there |
| h6-8, h24 | c1: 0 zero 1 asat 2 s1 3 s1a 6 s2 7 s2a (4, 5 unused) + complement | arg3 | encoder |
| h3-5, h15 | c2: same | arg4 | encoder |
| h20-21 | cop add sub min max | arg5 | encoder |
| h9-10 | aop | arg6 | encoder |

h2 and l14-20 are unused. Corpus: lowp add/sub/min/max (`sopwm d, a, b [c1=one c2=one cop=sub aop=sub]`) and lowp mix (0xfa).

### sop3 family (hw 11)

All forms take dst, src0 (r/pa), src1 and src2. Selects for c1/c2 (h6-8 + h24, h3-5 + h15) are 0 zero, 1 asat, 2 s1, 3 s1a, 4 s0, 5 s0a, 6 s2, 7 s2a (**encoder**). h21 picks the colour op and h10 the coissued alpha op.

**sop3** (h21=0, h10=0; USEASM 0x59 + 0xde asop / 0xdf arsop):

| bits | field | tag |
|---|---|---|
| h20 | cop add/sub (arg6 0x12/0x13) | encoder |
| h12-13, h14 | a: alpha select zero s0a s1a s2a + complement (0xde/0xdf arg0) | encoder |
| h9 | aop add/sub (0xde/0xdf arg1) | encoder |
| h11 | arev: 0xdf instead of 0xde | encoder; usc uses 0xdf when bNegateAResult. In asop the other alpha select is c2's alpha; arsop swaps the roles (b - a) |

**sop3l** (h21=0, h10=1; 0x59 + 0xe0 alrp):

| bits | field | tag |
|---|---|---|
| h20 | cop | encoder |
| h13, h9 | a1: zero / s1a + complement | encoder |
| h12, h14 | a2: zero / s2a + complement | encoder |
| h11 | cneg: colour result negate (arg7 0x14); only allowed with alrp | encoder |

**lrp1** (h21=1, h10=0; USEASM 0xe1 + 0xde):

| bits | field | tag |
|---|---|---|
| h13, h20 | l1: zero / s1 + complement | encoder |
| h12, h14 | l2: zero / s2 + complement | encoder |
| h11 | cs: s0 / s0a | encoder; usc assert `psLrp1->uCS == SRC0ALPHA` |
| c1/c2 fields | the coissued asop's two selects | encoder |
| h9 | aop | encoder |

Colour = lerp(l1, l2, cs) is a **guess** from the name.

**sop3x** (h21=1, h10=1; USEASM 0xe2 + 0xe0): c1 and c2 as above, plus the alrp alpha (a1, a2 as in sop3l).

h11 and h20 (field p) encode the pair arg6/arg7: 0 `s1.s2`, 1 `s1.1-s2`, 2 `one.s2`, 3 `1-s1.s2`. The pair 1-s1, 1-s2 is rejected (**encoder**). The name and the meaning are **guess**; usc's asm.c never emits 0xe2 explicitly.

Corpus: lowp `mul_l`/`div_l`/`pow_l`/`mad_l` are `sop3l d, x, y, z [c1=s0 c2=zero a2=s2a]`. `mod_l` uses `sop3 [cop=sub c1=one c2=s0 a=one aop=sub]`.

### dot3 / dot4 (hw 18, USEASM 0x5a-0x61, FUN_0010e460)

| bits | field | source | tag |
|---|---|---|---|
| h24 | 4-vector (dot4) | 0x5b 0x5d 0x5f 0x61 | encoder; usc uVecLength 3/4 |
| h6 | f6 | 0x5e-0x61 | encoder; meaning unknown |
| h7-8 | cs: colour scale x1 x2 x4 x8 | arg1 INTSRCSEL 0xa-0xd | encoder |
| h11 / h15 | src1 / src2 complement | arg3/arg4 flag b15 | encoder |
| h5 | src0 present: coissue 0xda/0xdb (forms dot3s/dot4s) | | encoder |
| h4 | f4: 0xdb or 0xdd rather than 0xda/0xdc | | encoder |
| h20-21 (+h2 when h5=0) | a1: zero s1a s2a, 3 = x3 (+ complement) | coissue arg1; 0x5c 0x5d 0x60 0x61 force 3 | encoder |
| h9-10, h3 | a2: zero s1a s2a, 3 = x3 + complement | coissue arg2 (0xda/0xdb) or arg3 (0xdc/0xdd); without a coissue 3 | encoder |
| l16-17 (h5=0) | as: alpha scale | arg2 0xe-0x11 (no coissue) or 0xdc/0xdd arg0 | encoder |
| l15 (h5=0) | s1ac: complement of 0xdc/0xdd's fixed src1alpha (arg2) | | encoder |
| l14-20 (h5=1) | src0 (r/pa) | 0xda/0xdb arg0 | encoder |

0x5b and 0x5f reject a coissue. Corpus: dp3_l/dp4_l/norm3_l use `dot3/dot4 d, a, b [a1=zero a2=x3]`, the lowp dot product with no alpha op.

### ima8 (hw 13, USEASM 0x62) / fpma (hw 19, 0xf4), FUN_0010f1a0

`dst = c0 * c1 + c2` per channel and `alpha = a0 * s1a + s2a`. Operands: dst, src0 (r/pa, negate h3), src1, src2.

| bits | field | source | tag |
|---|---|---|---|
| h20-21, h10 | c0: s0 s1 s0a s1a + complement | arg4 | encoder |
| h5, h24 | c1: s1 / s1a + complement | arg5 | encoder |
| h4, h15 | c2: s2 / s2a + complement | arg6 | encoder |
| h9, h6 | a0: s0a / s1a + complement | arg7 | encoder |
| h7 / h8 | a1c / a2c: complement s1a / s2a (arg8 must be s1a, arg9 s2a) | | encoder |
| h11 | sat | flags2 b10 | encoder; name **guess** (usc psFpma field after bNegateSource0) |

Corpus: lowp `mad_l`/`sin_l` use `fpma`.

### ima16 (hw 14, USEASM 0x63; 0xe4-0xe9), FUN_0010f960

`dst = src0 * src1 + src2`, 16-bit. src0 is r/pa only.

| bits | field | source | tag |
|---|---|---|---|
| h11 | sgn (.sgn) | flags2 b12 (b11 = unsigned; both rejected) | encoder; usc |
| h10 | sat | flags2 b10 | encoder; name **guess** |
| h3-4 | f2_15 | flags2 b15-18, 0..3 | encoder; meaning unknown |
| h24 | dflag1 | dest arg flag b1 | encoder; meaning unknown |
| h6-7 / h8-9 | f1 / f2: src1 / src2 format: 0 16-bit (INTSRCSEL 0x2c), 1 u8 (0x1e), 2 s8 (0x1f), 3 o8 (0x20) | arg4 / arg5 | encoder; names **guess** (Eurasia SFORM) |
| h20 / h5 | s1hi / s2hi: byte select for 8-bit formats | arg2/arg3 flag b13 (b12 = low; one is required for a register) | encoder |
| h21 | src2 negate | arg3 flag b0 | encoder |

Immediates are 7-bit signed unless the format is u8. usc uses u8 exactly when the source is an immediate (usc.c case 0x34). Corpus: indexing, e.g. `ima16.sgn idx1, r0, #4, #0 [f1=u8 f2=u8]` scales an array index into an index register.

### imae (hw 15, USEASM 0x64; 0xea 0xf0), FUN_00110060

`dst = src0 * src1 + src2`, 16x16 + 32 with carry. src0 is r/pa only.

| bits | field | source | tag |
|---|---|---|---|
| h11 | sgn | flags2 b12 (usc: b11 when unsigned) | encoder; usc |
| h10 | sat | flags2 b10 | encoder; name **guess** |
| h3-5 | f2_15 | flags2 b15-18, 0..7 | encoder; unknown |
| h6-7 | t2: src2 type u16 / s16 / u32 | arg4 0x27/0x28/0x29 | encoder; usc assert names S16, U32 |
| h24 / h21 / h20 | s0hi / s1hi / s2hi: high 16 bits of src0/1/2 (arg flag b13; b12 = low; one of them, or an immediate, is required; u32 src2 takes neither) | | encoder |
| h8 | cout (arg5 0x2b) | | encoder; name **guess** |
| h9 | cin (arg6 0x2a) | | encoder; name **guess** |
| h15 | ci: carry internal register i0/i1 (arg7, when cin or cout) | | encoder |

src1 immediates are signed when sgn is set. src2 immediates are signed for s16 and u32 (usc sign-extends 16-bit constants for s16/u32: usc.c ~69289). Corpus: dynamic array addressing, `imae.sgn pa0, pa1, #16, sa9 [t2=u32]`.

### hw 16 / 17 / 1a (no corpus words; encoder or encoder (patched))

The common fields are as above. firv and op16s1 have only h12-13 for the repeat (1..4); firh accepts 1..6. Only ima32 is named by libusc (`psIn->u.psIma32`); the other names are Eurasia-family **guesses**.

**adif / adifsum** (16, h20-21=0, FUN_00110800, USEASM 0x67/0x68):
- h6 = 1 makes it adifsum, with src0 (r/pa).
- h15 = s0f13 (src0 arg flag b13, unknown).
- Probably |src1 - src2| (+ src0) (**guess**).

**op16s1** (16, h20-21=1, 0x6e, FUN_00118bf0, patched):
- f2_15 h3-6 (flags2 b15-18).
- s8 h7 (arg8 0x1f).
- x38 h8.
- x1c h9.
- x39 h10-11 (0x39-0x3b).
- i h15 (arg6 i0/i1).
- The operation is unknown.

**bilin** (16, h20-21=2, 0x6b, FUN_001111f0):
- fmt h8-9 u8/s8/o8 (0x1e-0x20; s8/o8 make immediates signed).
- ileave h24 (0x2d vs 0x2e).
- src23 h11 (0x30 vs 0x2f).
- dst23 h10 (0x32 vs 0x31).
- rnd h15 (0x33).
- The names are **guess**.

**ssum16** (16, h20-21=3, 0x69, FUN_00110be0):
- src0 has no bank bits: it is read in src1's bank (the encoder requires the same r/o/pa/sa type), and src1 has no ext.
- fmt h8-9.
- x1b h24.
- x1c h7.
- x1d h10.
- nf16 h15: set unless dest flag b16.

**ssum16i** (h8-9 = 3, 0x6a, FUN_001192a0, patched): src0 is an internal register (raw number); there is no src1.

**firv** (17, h2=0, 0x6c, FUN_00111850):
- src0 SameBank.
- fmt h8-9.
- a5 h6-7 (immediate 0-2).
- coef h3-5 (register type 0x11, 0-7).
- shift: 5-bit two's complement in h10-11, h14-15, h24 (printed raw 0-31).
- a8 h20-21 (0-3).

**firh** (h2=1, 0x6d inline, patched): the dest is an internal register (raw number). Shift bits 2-3 move to h0-1. h19 = flags2 b27.

**ima32** (1a, h20=0, 0x65, FUN_00118650): `dst = src0*src1 + src2`, 32-bit.
- sgn h9 (flags2 0x1000 vs 0x800; it also makes immediates signed).
- hi h4-6: the high 32 bits go to i0-i2 (arg1; usc psHighWordDest).
- cin h10-11: carry-in from i0-i2 (arg5 0x2a + arg6).
- negp h8 = src0 neg xor src1 neg.
- src2 neg h7.
- src0 r/pa/o/sa via ext h15.
- USEASM 0x167/0x168 are rewritten to `a*#1 + -b`: isub32 signed/unsigned.

**op1adrc** (1a, h20=1, 0x66 inline, patched):
- dest r/pa (h7), raw number.
- drc h0 (register type 10, DRC? 0-1).
- Sources src1, src2. The operation is unknown; the DRC operand suggests a long-latency op.

### What GLSL integer code compiles to (c3 corpus, ef_c3.txt)

libusc has no integer path for GLSL ES 1.0 `int`/`bool`. They are floats:
- mediump ints, the fragment default: F16 `fmad16`.
- highp ints (vertex shaders, or highp in in_*/ic_*): F32 `fmad`.
- bools: 1.0/0.0.

The hardware integer units appear only for array indexing.

| GLSL | emitted (corpus) |
|---|---|
| int(x), truncate toward zero | `fsubflr t, x, x` (fract), then `fmad16 f, t, -c45, x` (x - fract = floor), then `test.fadd.lt p0, ..., c48`, then `fmad16 u, f, c45, c52` (floor + 1), then `(p0) mov f, u`. highp: `fmad`, and the +1 for two lanes at once is an `efo` (ctl 0x230600) followed by `movc.ltz.f32 d, x, i0, x` (cv_ftoi, ic_*) |
| i + j / i - j / -i | `fmad16 d, i, c45, j` / `fmad16 d, i, -c45, j` / `fmad16 d, i, -c45, c48` (c45 = 1.0 f16, c48 = 0, c52 = 1.0 f32) |
| i * j, i * k | `fmad16 d, i, j, c48`; a constant comes from a secondary-attribute uniform (sa9) |
| i / j | `frcp r, j` (the secondary program for a constant divisor), `fmad16 q, i, r, c48`, then the truncation sequence on q |
| i % 7 (in_mod) | `i - trunc(i * (1/7)) * 7`: fmad16 with sa10 = 1/7, truncation, `fmad16 d, q, -sa9, i` |
| float(i) | nothing (already a float); an int uniform arriving as F16 gets `pck.f32.f16` |
| i < j etc., scalar | truncate both, then `test.fsub.<lt/le/gt/ge/eq/ne> p0, a, b` + predicated mov or branch |
| equal()/lessThan() vector | `testmask.fsub.<cond>.dword m, a, b` + `movc.nz.i32 d, m, c52, c48` (1.0/0.0) |
| && / \|\| | `test.fadd.ne p0, x, c48` per bool + predicated select or branch (bo_and, bo_or, bo_bu) |
| ^^ | `testmask.fsub.ne.dword` on the two 0/1 values + `movc.nz.i32` |
| not() | `efo d, -x, -y, c52` (ctl 0x230600: i0 = -x + 1, i1 = -y + 1) |
| bool(x), float(bool) | `test.fadd.ne`/`testmask.fsub.gt` + `movc.nz.i32 d, m, c52, c48` |
| a[i] (uniform/attribute array) | float index truncated (`fsubflr` + `fmad`), `pck.s16.f32` converts it to s16, `ima16.sgn idx1, x, #stride, #base [f1=u8 f2=u8]`, then the access `sa[idx1+off]` (ix_n, ix_v, vix_a, vix_skin) |
| a[i] on a temp array / vector component (ix_comp, ix_tmparr) | `pck.s16.f32`, `imae.sgn addr, idx, #stride, base [t2=u32]` (base from a secondary attribute; prologue `limm r0, #size` + `imae.sgn pa1, r0, g17, pa1` is a per-instance offset), then `st.l.d`/`ld.l.d` |
| lowp (C10/U8) arithmetic (*_l shaders, c1/c2) | sopwm (add/sub/min/max), sop2 (select/mov), sop3l (mul/div/pow/mad), dot3/dot4 (dp3/dp4/norm3), fpma (mad/sin) |

#### What a backend would use beyond GLSL ES 1.0

For true integer ops (GLSL ES 3 / OpenCL style, never emitted by this compiler):
- **add/sub/mul**:
  - 32-bit: `ima32` (USEASM 0x65; signed/unsigned via h9; the high word to an internal register; carry-in from i0-i2). `isub32` = USEASM 0x167/0x168.
  - 16-bit: `ima16` (USEASM 0xe4-0xe9 = iadd16 isub16 imul16 and the unsigned variants).
  - `imae` = 16x16 + 32 with carry in/out through an internal register (USEASM 0xea/0xf0 = iadd32).
- **compares**: `test` with integer ALU ops (hw 09: `test.isub16.<cond>`, `test.iadd32.<cond>`, see notes_pack).
- **shifts and logic**: hw 0a `and`/`or`, 0b `xor`, 0c `shl`/`rol`, 0d `shr`/`asr` (hw_pack.py / notes_pack; USEASM 0x4f-0x55). There is also `test.and/shl...` for flag tests.
- **conversion**:
  - float to int: `pck.s16.f32`, `pck.u16.f32`, `pck.s8.f32`, ...
  - int to float: `pck.f32.s16`, `pck.f32.u16`, ...
  - pck has no 32-bit integer format. A 32-bit conversion needs two halves, or the float path above.
- **division**: there is no hardware integer divide; use frcp + multiply + fix-up. op1adrc (0x66) is unknown and not on SGX545.

### Open questions

- **Operand bit 6** (format flag vs register number bit 6) in efo/sop*/dot/ima8/fpma: is there a global mode? See above.
- **efo**:
  - h22 (a0lneg) is a name guess.
  - h8 (flags3 b10) is unknown.
  - The exact M1 product of m-mode 2 is unknown.
  - What d=i0/i1 returns (the old or new internal value).
- **INTSRCSEL names**: 2 (asat), 0x18/0x19 (k18/k19), u8/s8/o8, and 0x2c are not in the binary.
- **sop3x** (USEASM 0xe2): its operation.
- **dot**:
  - f6 (h6, 0x5e-0x61), f4 (0xdb/0xdd vs 0xda/0xdc) and a1/a2 = 3 are unknown.
  - The meaning of 0x5c/0x5d/0x60/0x61, which have no alpha op.
- **sat** (flags2 b10) in ima8/fpma/ima16/imae is a name guess.
- ima16/imae **f2_15** (flags2 b15-18) and ima16 **dflag1** (dest flag b1) are unknown.
- **hw 16/17/1a**: INTSRCSEL 0x1b-0x1d and 0x38-0x3b, register types 0x11 and 10, the adifsum and ssum16 h15 flags, firv a5/a8, and the operations of op16s1 and op1adrc.

## smp, ld/st, and the special class (hw 1c, 1d, 1e, 1f)

Forms: `hw_mem.py`. Check: `check_mem.py` (about 290 directed oracle cases, every
corpus word of these classes, and a mutation sweep: about 13,000 instructions
libusc accepts, made by changing one flag bit, argument type, number,
argument flag or index at a time from a valid instruction per opcode. All of
them decode with no unexplained bit and round-trip through the text).
`hwcheck.py` reports no hw 1c-1f word without a form, no unexplained bits and
no round-trip failures.

Evidence tags: **encoder** = read from the decompiled encoder and confirmed by
running it (oracle); **corpus** = also seen in what the GLSL corpora compile
to; **guess** = the name or meaning has no backing beyond plausibility.
Function names are Ghidra addresses (libusc + 0x10000). "fN bK" = USEASM
flags word N (dw1-3) bit K; "argument flag K" = bit K of an argument's flags
dword.

Target feature bits used below (SGX545: word0 = 0x57fdbffd, word1 =
0xe387c601, core flag word at core entry +8 = 0x2c194000, feature +0xc = 8,
+0x10 = 16, +0x20 = 18):

| predicate | test | SGX545 | gates |
|---|---|---|---|
| FUN_0011de30 | core byte 9 & 0x40 | yes | smp: write mask must be 1, no repeat |
| FUN_0011e150 | word0 & 0x40000 | yes | smp PCF (h5/h6); icvt_core.c says "PCF isn't supported on this processor" without it |
| FUN_0011e170 | word0 & 0x80000 | yes | smp h14/h15 |
| FUN_0011e2a0 | word1 & 1 | yes | smp repeat/mask encoding |
| FUN_0011dfa0 | word0 & 0x200 | yes | eld exists; nosched on ld rejected |
| FUN_0011dfc0 | word1 & 0x200 | yes | ld/st cache bits h1/h19 via f2/f3 |
| FUN_0011dfe0 | word1 & 0x400 | yes | st h0 (f3 b13) |
| FUN_0011dde0 | core byte 9 & 1 | no | (would allow inc flags with mode "l") |
| FUN_0011e1d0 | word0 & 0x400000 | yes | branch "extended sync-end" (asm.c: SGX_FEATURE_FLAGS_USE_BRANCH_EXTSYNCEND) |
| FUN_0011e0c0 | feature +0x10 (16) | 16 | br/ba f1 b22 allowed; USEASM 0xd7 immediate limit |
| FUN_0011e0d0 | feature +0x20 (18) | 18 | branch / phas address width |
| FUN_0011e0b0 | feature +0xc (8) | 8 | 0xcd/0xce immediate limit |
| FUN_0011dee0 | word0 & 4 | yes | MOE register forms (h19) |
| FUN_0011e230 | word0 & 0x2000000 | yes | emit: SGX545 forms 0x101-0x104, 3-bit n |
| FUN_0011e250 | word0 & 0x8000000 | no | (would reject 0x9c/0x9d) |
| FUN_0011e1b0 | word0 & 0x200000 | yes | USEASM 0x98 |
| FUN_0011e190 | word0 & 0x100000 | yes | nop l1 (f3 b5) |
| FUN_0011e080 | word0 & 0x1000 | yes | phas (0xd8/0xd9) |
| FUN_0011e210 | word0 & 0x1000000 | yes | USEASM 0xfe-0x100 |
| FUN_0011e1f0 | word0 & 0x800000 | yes | USEASM 0xfd |

### USEASM opcode -> form

| USEASM | form | encoder |
|---|---|---|
| 0x6f-0x71 | `smp` (1d/2d/3d) | FUN_0010a3f0 |
| 0x72-0x74 | `smp.bias` | FUN_0010a3f0 |
| 0x75-0x77 | `smp.lod` (replace LOD) | FUN_0010a3f0 |
| 0x78-0x7a | `smp.grad` | FUN_0010a3f0 |
| 0x7b/0x7c/0x7d (0x7e rejected) | `ld.a.b` / `ld.a.w` / `ld.a.d` | FUN_0010b190 |
| 0x7f/0x80/0x81 (0x82) | `st.a.b/w/d` | FUN_0010b190 |
| 0x83-0x85 (0x86) | `ld.l.b/w/d` | FUN_0010b190 |
| 0x87-0x89 (0x8a) | `st.l.b/w/d` | FUN_0010b190 |
| 0x8b-0x8d (0x8e) | `ld.t.b/w/d` | FUN_0010b190 |
| 0x8f-0x91 (0x92) | `st.t.b/w/d` | FUN_0010b190 |
| 0x93 / 0x94 | `eld.d` / `eld.q` | FUN_0010ace0 |
| 0x95 | `ba` | FUN_00119f50 inline |
| 0x96 | `br` (with f1 b19: call) | FUN_00119f50 inline |
| 0x97 | `lapc` | FUN_00119f50 inline |
| 0x98 | `op0x98` | FUN_00119f50 inline |
| 0x99 | `idf` | FUN_00119f50 inline |
| 0x9a | `wdf` | FUN_00119f50 inline |
| 0x9b | `limm` | FUN_00119710 |
| 0x9c, 0x9d, 0xa2 (0x9e, 0x9f-0xa1 rejected on SGX545) | `emit.t0` / `emit.t0` / `emit.t2` | FUN_00109700 |
| 0x101, 0x102 / 0x103, 0x104 | `emit.t1` / `emit.t3` | FUN_00109700 |
| 0xa8 | label definition: no instruction, records the address and patches waiting branches | FUN_00119f50 |
| 0xb7 | nothing (returns 0) | FUN_00119f50 |
| 0xbc | `smoa` | FUN_00108120 |
| 0xbd / 0xbf / 0xc0 | `smr` / `smbo` / `imo` | FUN_00108a50 |
| 0xbe | `smlsi` | FUN_001084b0 |
| 0xc1 | `setfc` | FUN_00119f50 inline |
| 0xcd / 0xce | `lock` / `release` | FUN_00119f50 inline |
| 0xd0 / 0xd1 | `ldr` / `str` | FUN_00119f50 inline |
| 0xd2 / 0xd3 | `pcoeff` / `ptoff` | FUN_0010a1e0 |
| 0xd4 | `atst8` | FUN_00111ef0 |
| 0xd5 | `depthf` | FUN_00109eb0 |
| 0xd6 | `op0xd6` | FUN_00119f50 inline |
| 0xd7 | `op0xd7` | FUN_00119f50 inline |
| 0xd8 / 0xd9 | `phas` / `phas.i` | FUN_00108ff0 |
| 0xf1 | `nop` | FUN_00119f50 inline |
| 0xfd | `op0xfd` | FUN_00119970 |
| 0xff / 0xfe / 0x100 | `op0xff` / `op0xfe` / `op0x100` | FUN_00108d50 |

A scan of all USEASM opcodes 0-0x1ff with generic arguments finds no other
opcode that lands on hw 1c-1f.

libusc's internal opcodes (from its instruction descriptor table at Ghidra
0x159960, field +0x30 = USEASM opcode): 0x22-0x2f loads/stores (byte group
0x22-0x25, word 0x26-0x29, dword 0x2a-0x2f), 0x30/0x31 eld, 0x32 idf, 0x33
wdf, 0x6b IBR and 0x6c ICALL (both 0x96; asm.c adds f1 b19 for the call),
0x6d ILABEL (0xa8), 0x6e ILAPC (0x97; asm.c asserts `eOpcode == ILAPC`
around return labels), 0x71 and 0x76 (0xf1: INOP and IDRVPADDING), 0x72
ISMLSI (0xbe), 0x73 (0xbd), 0x74 (0xc1), 0x75 (0xbf), 0x04 (0x9b), 0xa7
(0xd6), 0xaf (0x101: created only by the geometry-shader path, behind "Geometry
shaders aren't supported on this core"), 0xb7/0xb8 (0xcd/0xce).

---------------------------------------------------------------------------

### hw 1c: smp (FUN_0010a3f0)

`smp.<dim>[.bias|.lod|.grad][.f16|.c10][.pcf|.pcf16][.rdata|.rinfo] dst, coord, state[, lod]  [drc=n ...]`

Arguments (USEASM): dest, coordinate, texture state, [LOD source], DRC
(register type 10, number 0/1), and int-source-select 0x15 (a fixed
terminator the encoder checks).

| bits | field | values | evidence |
|---|---|---|---|
| h27-31 | class | 0x1c | encoder |
| h24-26 | pred | 3-bit predicate (PRED3) | encoder |
| h23 | skipinv | f1 b0 | encoder |
| h22 | nosched | f1 b3 (not h11 as in the ALU classes) | encoder |
| h21 | rcnt | set with a repeat count; SGX545 rejects any repeat | encoder |
| h20 | syncs | f1 b2 (sync-start) | encoder |
| h12-15 | mask (rpt when h21) | write mask; SGX545 accepts only mask 1, so h12 is always set | encoder |
| h14 | ret = rdata | f3 b3. asm.c sets it for the sample's mode field = 1 | encoder; name guess (raw sample data) |
| h15 | ret = rinfo | f3 b2 (mode 2). Mode 3 (f3 0x8000, "both") is rejected on SGX545; h14+h15 together is rejected | encoder; name guess (sample info) |
| h10-11 | dim | 0 1D (USEASM 0x6f+3k), 1 2D (+1), 2 3D (+2; cube maps) | encoder, corpus (2D for texture2D, 3D for samplerCube) |
| h8-9 | lod | 0 none, 1 bias, 2 replace (texture2DLod), 3 gradients | encoder, corpus (bias, lod, grad seen in ef_c3) |
| h7 | dst bank | 0 r, 1 pa (no other destination) | encoder |
| h5 | pcf | f3 b0: percentage-closer compare, F32 coordinates | encoder; asm.c sets it only for an F32 coordinate |
| h6 | pcf16 | f3 b1: the same with F16 coordinates. Not with C10 coordinates | encoder |
| h3 | f16 | coordinate is F16 (argument-1 flag 22) | encoder, corpus (mediump tx_*_m) |
| h4 | c10 | coordinate is C10 (argument-1 flag 24) | encoder, corpus (lowp tx_*_l) |
| h2, h18 | coord bank | r 00, pa h2, o h18, sa h2+h18. Internal registers become r124+n; no indexing, no constants/immediates | encoder |
| l14-20 | coord | first register of the coordinate | encoder |
| l7-13, l30-31, h17 | state | src1 slot, any source kind (r o pa sa c g i # indexed) | encoder |
| l0-6, l28-29, h16 | lod source | src2 slot. With lod 0 it holds the unused-source pattern (#0: l29 + h16) | encoder |
| l21-27 | dst | first result register | encoder |
| h0 | drc | dependent-read counter the result signals (0/1); a later `wdf drc` waits on it | encoder, corpus (ef_c3 tx_dep*) |
| h1 | - | never set | encoder |

Result format: nothing in the word selects it; the texture state (src1)
does. The `state` operand is a placeholder in the USP binary: dependent
`smp` words sit in USP sample blocks, which the patcher turns into code with
the real state registers at finalize time (ef_c3 logs them with `#0`).
Projective reads have no smp bit: the compiler divides the coordinate first
(tx_dep_proj*).

### hw 1d: ld (FUN_0010b190), h22 = 0

`ld.<a|l|t>.<b|w|d> dst, base, off[, range]  [drc=n preinc postinc range moe rpt=n ...]`

USEASM 0x7b + i (load when (i / 4) is even): address mode = i / 8, size =
i % 4. Arguments: dest, base, offset, range (only with f1 b29; otherwise a
placeholder), DRC.

| bits | field | values | evidence |
|---|---|---|---|
| h27-31 | class | 0x1d | encoder |
| h24-26 | pred | PRED3 | encoder, corpus (the `(p0) ld.a.d` of every secondary program) |
| h23 | skipinv | f1 b0 | encoder, corpus |
| h22 | 0 | nosched would set it but libusc rejects nosched on loads; h22 = 1 is eld | encoder |
| h21 | moe | set unless f1 b31: a repeated load steps its operands by the MOE increments (same bit as the ALU classes) | encoder; meaning by analogy (guess) |
| h20 | syncs | f1 b2 | encoder |
| h19 | f2_3 | f2 b3; f3 b9 sets h19 and h1 | encoder; data-cache control (guess) |
| h18, h2 | base bank | src0 slot: r, pa (h2), o (h18), sa (h2+h18) | encoder, corpus (base sa1 = local memory base) |
| h17, h16 | src exts | of offset / range | encoder |
| h12-15 | rpt | repeat count - 1 (1-16) | encoder, corpus (st rpt=16) |
| h10-11 | am | 0 "a" (USEASM LDA*, the secondary program's uniform loads), 1 "l" (LDL*: spills, temp arrays), 2 "t" (LDT*; the offset must be a register; no pre/post flags) | encoder, corpus (a, l); names a/l/t guess (absolute / local / tiled) |
| h8 | preinc | f1 b27 | encoder; name guess |
| h9 | postinc | f1 b28 (not both) | encoder; name guess |
| h7 | dst bank | 0 r, 1 pa | encoder |
| h6 | range | f1 b29: src2 holds a 4th source | encoder; name guess |
| h4-5 | size | 0 d (dword: 0x7d...), 1 w (0x7c...), 2 b (0x7b...); 3 (0x7e... "q") rejected | encoder, corpus (d); b/w from libusc's internal opcode order (byte, word, dword groups) |
| h3 | f2_9 | f2 b9 | encoder; unknown |
| h1 | f2_2 | f2 b2 | encoder; data-cache control (guess) |
| h0 | drc | DRC the loaded data signals | encoder, corpus (ix_tmp*: loads alternate drc 0/1, each followed later by `wdf`) |
| l21-27 | dst | | encoder |
| l14-20 | base | | encoder |
| l7-13, l30-31 | off | any source kind, including #0-127 and indexed | encoder, corpus |
| l0-6, l28-29 | range | without h6: the unused pattern | encoder |

### hw 1d: eld (FUN_0010ace0), h22 = 1

`eld.<d|q> dst, src1, src0, #imm11, src2  [drc=n ...]` (USEASM 0x93 / 0x94;
target feature word0 & 0x200).

| bits | field | values | evidence |
|---|---|---|---|
| h24-26 | pred | PRED3 | encoder |
| h23 | skipinv | | encoder |
| h21 | f2_3 | f2 b3 (h19 in ld) | encoder |
| h20 | syncs | | encoder |
| h12-15 | rpt | count - 1; a repeat needs f1 b31 | encoder |
| h5-11, h16-19 | imm | 11-bit immediate (USEASM argument 3): low 7 bits h5-11, high 4 h16-19, so no source has an extension bit (r/pa/o/sa only) | encoder |
| h4 | dst bank | r / pa | encoder |
| h3 | size | 0 d (0x93), 1 q (0x94) | encoder; names from USEASM order |
| h2 | src0 bank | r / pa | encoder |
| h1 | f2_2 | f2 b2 | encoder |
| h0 | drc | | encoder |
| l21-27, l14-20, l7-13 + l30-31, l0-6 + l28-29 | dst, src0 (argument 2), src1 (argument 1), src2 (argument 4) | | encoder |

nosched is rejected.

### hw 1e: st (FUN_0010b190)

`st.<a|l|t>.<b|w|d> base, off, data  [...]` (arguments: base, offset, data).
Same layout as ld, with these differences: no destination (l21-27 and h7
are 0), no DRC (h0 = f3_13, f3 b13, target feature word1 & 0x400; unknown),
h22 = nosched (allowed on stores), the data is the src2 slot (any source
kind, including immediates). Corpus: ix_tmpw (`st.l.d sa1, pa1, sa9`,
dynamic array writes; `st.l.d sa1, sa13, pa0 [rpt=16]`) and ix_comp.

---------------------------------------------------------------------------

### hw 1f: the special class

The encoders' constant high words split the class on h20-22:

| h22 h21 h20 | group | op field |
|---|---|---|
| 0 0 0 | flow control | h6-8 |
| 0 0 1 | MOE control | h24-26 |
| x 1 0 | other (h22 is data in limm and emit) | h24-26 |
| x 1 1 | visibility test (h22 is data in atst8) | h24-26 |
| 1 0 0 | phase / 0xfe-0x100 (h23 = 0) | h24-26 |

So unlike the ALU classes the predicate lives in h24-26 only in flow control;
elsewhere h24-26 is the op and the predicate (where there is one) moves:
limm h9-11 (PRED3), depthf h9-10 (PRED2). Everything else rejects a
predicate.

#### flow control (h20-22 = 0)

| form | fixed | fields | evidence |
|---|---|---|---|
| `ba #target` | h6-8 = 0, h19 = 0 | target l0-17: label address + the program's load offset (encode()'s `offset`), unsigned 18 bits (feature +0x20) | encoder |
| `br #target` | h6-8 = 1, h19 = 0 | target l0-17: label + label-argument offset - this instruction's index, signed 18 bits (-0x20000..0x1ffff) | encoder, corpus |
| `lapc` | h6-8 = 2 | return to the saved link address | encoder, corpus (fn_call) |
| `nop` | h6-8 = 5 | h23 syncs (f1 b2), h18 end (f1 b17), l0 = f2 b26, l1 = f3 b5 | encoder, corpus (secondary-program prologue `nop [nosched]`, discard placeholder) |
| `op0x98` | h6-8 = 0, h9 = 1, h19 = 1 | no arguments; needs word0 & 0x200000 | encoder; meaning unknown |

Fields of ba/br/lapc: h24-26 pred (PRED3; pN rejected), h23 syncend (f1 b18;
asm.c's branch sync-end), h12 xsyncend (f3 b4: the encoder sets h12 and
h23; not with f1 b18), h11 nosched, and for ba/br h9 link (f1 b19: save the
return address; asm.c ICALL = `br` + this flag) and h10 f1_22 (meaning
unknown; allowed because feature +0x10 is non-zero).

Branch targets: the label argument is register type 0xb (label number) or
0x12 (label number, with the argument's flags dword as an offset added to the
target); type 0x15 is a raw target. The encoder asks the context's callback
3 for the label's address; -1 means not yet defined, and the instruction is
queued (FUN_00107760) and patched when USEASM 0xa8 defines the label
(FUN_00107580 writes the field). In the USP binaries every branch has low
word 0: the branch is stored in a branch block with its label number, and the
patcher fills the target at finalize. `lapc` is stored in a branch block with
label 0xffff.

#### MOE control (h20-22 = 1)

h18 = nosched for the whole group; no predicate. h19 = register form for ops
0-3 (the values come from src1/src2 instead of immediates; word0 & 4).

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `smoa v0, v1, v2, v3 [am0..am3]` (0xbc) | four signed 10-bit values: v0 l30-31 + h0-7, v1 l20-29, v2 l10-19, v3 l0-9; four 2-bit modes (USEASM register type 0xe): am0 h14-15, am1 h12-13, am2 h10-11, am3 h8-9 | encoder; name and meaning guess (MOE operand addressing) |
| 1 | `smr v0..v3` (0xbd) | four 12-bit: v0 h4-15, v1 l24-31 + h0-3, v2 l12-23, v3 l0-11 | encoder; name guess |
| 2 | `smlsi v0..v3 [lim0 lim1 lim2]` (0xbe) | per operand (dest, src0, src1, src2) an 8-bit value: v0 l24-31, v1 l16-23, v2 l8-15, v3 l0-7; flag h3/h2/h1/h0 set = the value is a swizzle (four 2-bit selects, printed `#0.xyzw`), clear = a signed increment; lim0 h12-15, lim1 h8-11, lim2 h4-7 = USEASM arguments 8-10 / 4 (multiples of 4, up to 60) | encoder, corpus (every swizzled operation is bracketed by `smlsi`; libusc's ISMLSI); limits' meaning guess (temp / pa / sa wrap limits) |
| 3 | `smbo v0..v3` (0xbf) | as op 1 | encoder; MOE base offsets (the USP code block keeps 4 MOE base offsets) — guess |
| 4 | `imo v0..v3` (0xc0) | as op 1, signed (the encoder takes -32..31); no register form | encoder; name guess (increment MOE offsets) |
| 5 | `setfc #a0, #a1` (0xc1) | a0 l0, a1 l8 (USEASM arguments must be #0/#1) | encoder, corpus (`setfc #1, #1` in tx_dep_loop, cos_l, sin_l); name guess (set format control: EFO / colour) |
| 6 | `op0xfd dst, #n` (0xfd) | dst l21-27 + h0-1 + h19 (destination slot), n h11-14 (0..8), h23 skipinv | encoder; meaning unknown |

The USP code block header's four MOE words are this state at block entry:
0x8000 | increment (low 8 bits signed) or 0x4000 | swizzle, per operand (for
example after `smlsi #1, #1, #0.zyxw, #1` the next block has 0x8001 0x8001
0x40c6 0x8001; corpus).

#### other (h20-21 = 2)

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `idf [drc=d path=p]` (0x99) | h0 DRC, h14 second argument (#0/#1) | encoder; idf = issue data fence (guess) |
| 1 | `wdf [drc=d]` (0x9a) | h0 DRC; waits until the loads/samples that signal that DRC have written their results | encoder, corpus (after every uniform load and before the first use of a loaded or sampled register) |
| 2 | `op0xd7 #n` (0xd7) | n l0-3 (below feature +0x10 = 16) | encoder; unknown |
| 3 | `emit.<t0..t3> src0, src1, src2 [n f10 f12 f13 f3_20 imm end]` | tgt h14-15 (0x9c/0x9d 0, 0x101/0x102 1, 0xa2 2, 0x103/0x104 3); f12 h12 (0x101, 0x103; 0xa0 on older cores; f2 b5 on 0xa2); f13 h13 (0x101 + f3 b8: libusc's geometry "emit and cut" sets f3 0x100; 0xa1 on older cores; f2 b4 on 0xa2); f10 h10 (f3 b6 on 0x101/0x103, f3 b7 on 0x102/0x104); l21 = f3 b20; n = first argument (h0-1 + h9, 0..5); h18 end; h11 nosched. Sources: 0x101/0x102 src1 + src2; 0x103/0x104 src2 only; 0x9c-0x9e/0xa2 src0 (extension h19) + src1 (+ src2). Unused sources hold #0. 0x9d/0x9e/0xa1/0xa2 add a 14-bit immediate (l22-27, h3-8, h22-23); 0x9c sets h8 (= immediate 0x800) | encoder; 0x101 is libusc's geometry-shader vertex emit; target names unknown |
| 4 | `limm dst, #imm32` (0x9b) | dst = destination slot (l21-27, h0-1, h19); imm l0-20 (bits 0-20), h4-8 (21-25), h12-17 (26-31); a label argument (type 0xb) gives its absolute address (+ offset); argument flag 11 inverts the value; h9-11 pred (PRED3); h23 skipinv; h22 nosched; h18 end | encoder, corpus (`limm r0, #4096` before indexed stores) |
| 5 | `lock #n` / `release #n` (0xcd / 0xce) | l0 = 0 / 1; n l4-6 (below feature +0xc = 8) | encoder; names guess (mutexes) |
| 6 | `ldr dst, addr [drc=d]` (0xd0), `str addr, data` (0xd1) | h19 = str; dst l21-27 + h7 (r/pa); addr = src2 slot whose immediate form has 14 bits (low 7 l0-6, high 7 l14-20); data = src1 slot; h0 DRC (ldr); h23 skipinv; h11 nosched | encoder; names guess |
| 7 | `op0xd6 #n` (0xd6) | n l0-2 (0..5), l3 = f3 b6 (libusc sets it under the same compile flag as emit's f3 b6) | encoder; unknown |

h11 = nosched for all of them except limm (h22).

#### visibility test (h20-21 = 3)

Sources: src0 (l14-20, bank h2, extension h19), src1, src2. The USEASM
int-source-select arguments: 0x34 sets h5 ("sel34"), 0x36 sets h3, 0x37 sets
h4; 0x15 and 0x35 mean none. h18 = end, h11 = nosched, h23 = syncs (atst8,
depthf).

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `pcoeff src0, src1, src2` (0xd2), `ptoff` (0xd3: h15, no sources) | | encoder; names guess (plane coefficients / punch-through off) |
| 1 | `atst8 dst, pdst, src0, src1, src2` (0xd4) | dst l21-27 + h0-1 without extension (r o pa); pdst = predicate p0-p3 in h7-8, h6 = write it (cleared by the argument's flag 16, "disable write-back": printed `p1.nw`); h22 = f2 b25; h3/h4/h5 sel. The GLES driver's own atst8 (`f9340406`) also sets h10, which libusc never does: h9-10 is a PRED2 predicate as in depthf (2 = p1); a false predicate kills the pixel (driver, hardware: "Phases, visibility feedback and discard") | encoder; name guess |
| 3 | `depthf src0, src1, src2` (0xd5) | h9-10 pred (PRED2); h4/h5 sel (0x36 rejected) | encoder; name guess |

#### phase / 0xfe-0x100 (h20-22 = 4, h23 = 0)

| op | form | layout | evidence |
|---|---|---|---|
| 2 | `phas src1, src2` (0xd8, h18 = 0) | h19 end, h11 nosched | encoder; name guess |
| 2 | `phas.i #pc, #temps4 [rate wait sel43 end]` (0xd9, h18 = 1) | pc l0-17 (18 bits, absolute, no load offset added); temps4 h0-7 = USEASM argument / 4 (multiple of 4, up to 0x3fc); rate h11-12: int-source-select 0x3c 0, 0x3e 1, 0x3d 2; wait h8-10: none (type 0xe #0) 0, 0x3f 1, 0x40 2, 0x41 7; sel43 h13 (0x42 0, 0x43 1); h19 end | encoder; reading as "next phase address, temporaries, execution rate, wait condition" is a guess (libusc talks about feedback/split phases: uMainProgFeedbackPhase1Start) |
| 0 | `op0xff` / `op0xfe` / `op0x100 src1, src2, #n` | h12-13 = 1 / 2 / 3; n h8-9 (0..3); h15 = f3 b11, h14 = f3 b12; h11 nosched | encoder; unknown |

---------------------------------------------------------------------------

### How the corpus builds control flow (c3 lp_*, fl_*, dc_*, fn_call)

USP binaries keep control flow symbolic: code blocks (type 2), label blocks
(type 4) and branch blocks (type 3: one encoded branch with low word 0, plus
the target label). The patcher lays the blocks out and fills the targets.

- **Conditions** are always a `test` (hw 09) writing a predicate, then a
  predicated `br` (`(p0) br` / `(!p0) br`); the predicate is the branch's
  own h24-26.
- **Static loops** (lp_s4, lp_nest): the counter is a float register
  (`mov r0, c48` = 0.0; `fmad r0, r0, c52, c52` = r0 * 1 + 1), the test
  compares it against the bound (`test.fsub.lt p0, r1, c6`) and `(p0) br`
  jumps back to the label at the loop head (a do-while: the body runs
  before the first test). Nested loops are two such back-edges; nothing is
  unrolled, no hardware loop counter is used.
- **Guarded loops** (lp_s100, lp_while, tx_dep_loop): a test before the loop
  and `(!p0) br` to the exit label skip it when the trip count may be zero.
- **Dynamic trip counts** (lp_n: `if (i >= n) break` against a uniform
  int): the same counter, compared with the uniform's secondary attribute
  (`test.fsub.ge p0, r4, sa9`), with an exit branch.
- **break** (lp_brk, lp_n, lp_while): a predicated forward `br` to a label
  after the loop (the code that runs at loop exit, often a predicated
  `mov` copying the result, is placed before that branch), then the
  back-edge `(p0) br`, then an unconditional `br` over the break target.
- **continue** (lp_cont): no branch; the rest of the body is predicated
  (`(!p0) fmad`), and the loop test follows.
- **if/else** (fl_*): small or even large bodies (fl_big: sin/exp2/log2/sqrt
  on one side, cos on the other) are if-converted: both sides computed, the
  result picked with a predicated `mov`. Branches appear around texture
  reads (tx_dep_if: `(!p0) br` then `(!p0) br [syncend]` over a block with
  gradients (fdsx/fdsy) and a sample block).
- **discard** (dc_*): the condition is a test writing the keep-predicate (p1,
  e.g. `test.fsub.le p1, pa4, pa5` for `if (a.x > b.x) discard`), then a
  code block holding only `nop` between two labels (USP header 0x76 / 0x78
  name them). The kill is not in the USP binary: the draw-time builder
  replaces that nop with `atst8 ... [end]` predicated on p1 and splits the
  program into two phases (driver; see "Phases, visibility feedback and
  discard").
- **calls** (fn_call): the function body is its own labelled region ending
  in `lapc` (a branch block with label 0xffff); each call site is `br
  [link]` (asm.c ICALL: f1 b19) to that label; arguments and results go
  through ordinary registers (r0-r4 here).
- **dependent reads / loads**: `smp` / `ld` set a DRC (h0); `wdf drc` before
  the first use of the result (ix_tmp*: loads alternate drc0/drc1 so two
  can be in flight). Every secondary (uniform) program starts `nop
  [nosched]`, `test.and.ne p0, #0, #0`, `(p0) ld.a.d pa0, pa0, #0`, `wdf`.
- **MOE**: swizzled or strided operations are bracketed by `smlsi` (set the
  per-operand swizzle/increment, run the repeated op with `moe`, restore
  `smlsi #1, #1, #1, #1`).

### Open questions

- Names/meanings marked guess, in particular: ld/st address modes (a/l/t),
  pre/post increment, range (h6), f2_2/f2_3/f2_9 (cache control?), st h0
  (f3 b13); smp h14/h15 (rdata/rinfo); the MOE ops other than smlsi/smbo;
  op0x98, op0xd6, op0xd7, op0xfd, op0xfe-0x100; emit targets; the
  visibility-test names and their int-source-select options; phas fields.
- br/ba h10 (f1 b22) and USEASM 0xd7 share target feature +0x10 (16).
- smp's coordinate count (how many registers the coordinate spans) is not in
  the word; it follows from the dimension and the projective/PCF modes.
- The discard path is traced (see "Phases, visibility feedback and
  discard"); the visibility-test operands are not.

## USP texture-sample blocks and the driver's sample finalizer

How a texture read gets from GLSL to USSE code on SGX545 (Cedarview DDK 1.7):
libusc writes USP blocks 5 and 6 instead of code, and the USP finalizer that
is statically linked into `libGLESv2.so.1.7.862890` turns them into
instructions once the texture formats are known.

Addresses are Ghidra addresses (file vaddr + 0x10000) for **both** libraries:
libusc `FUN_001042a0` = libusc vaddr 0xf42a0. `re/pds/PDS.md` gives libGLESv2
addresses as file vaddrs (0x36b00 there = `FUN_00046b00` here).

Evidence tags: **writer** (read from libusc's writer), **reader** (read from
the driver's USP loader `FUN_000511d0`), **driver** (read from the finalizer),
**corpus** (c1/c2/c3 and `ef_c3.txt`), **guess**.

Tools: `smpblock.py` parses the three block types; `usp.py` now calls it
(`block["sample"]`). `python3 -I smpblock.py corpus/c3` prints every sample
block of a corpus. Decompiles used: `$S/usc.c`; `$S/gles.c` (libGLESv2,
DumpAll) and `$S/gles_sw.c` (the three switch functions Ghidra could not
recover, re-decompiled with jump-table overrides by `$S/gs2/FixSw.java`).

### Where the blocks come from

libusc's block walker `FUN_001042a0` (uspbin.c) inlines the sample writers.
They are switch cases on the IR opcode, not separate functions: ISA.md's
"0xf48da, 0xf46e0, 0xf43a0" are addresses inside that function. (**writer**)

| IR opcode | name (from asserts / `aeUSPOpcodes` at 0x12841c) | block |
|---|---|---|
| 0xa0 | ISMP_USP (LOD mode 0, normal) | 5 |
| 0xa1 | ISMPBIAS_USP (LOD mode 1) | 5 |
| 0xa2 | ISMPREPLACE_USP (LOD mode 2, explicit LOD) | 5 |
| 0xa3 | ISMPGRAD_USP (LOD mode 3, gradients) | 5 |
| 0xa4 | ISMP_USP_NDR (non-dependent read) | 5 |
| 0xa5 | ISMPUNPACK_USP | 6 |
| 0xa6 | texture/image write ("Invalid register format for image write") | 9 |

(The non-USP opcodes are 0x5f..0x62, table 0x12842c.) The driver's reader
`FUN_000511d0` dispatches on the block type through the table at 0x74ff8:
type 1 → 0x521bd, 2 → 0x51f5e, 3 → 0x52928, 4 → 0x51bca, **5 → 0x51c79,
6 → 0x51a18, 9 → 0x513c7**, 0/7/8 end. The reader's field order is the
writer's, field for field. (**writer**, **reader**)

Every sample block sits where the instruction stood in the program, between
code blocks; the finalizer inserts its generated code at that point.

#### Shared sub-records

- **Format control + MOE state, 18 bytes** (also the start of code blocks;
  reader `FUN_0004f940`): u16 format control (b0: F16 format select enabled,
  b1: C10 format select enabled; tracked from IR opcode 0x74, SETFC), 4 u16
  per-operand MOE mode (`0x8000|inc` = increment, `0x4000|swz` = four 2-bit
  swizzle selects, from IR 0x72 SMLSI; default `0x8001`), 4 u16 MOE base
  offsets (IR 0x75 SMBO). (**writer**, **reader**)
- **Per-instruction flags (iflags), u16** (libusc `FUN_00103fd0`, same word
  as the code block's per-instruction flags; driver converts it with
  `FUN_0004f700`): b0 internal registers live at this instruction (driver
  flag 0x2: it must save/restore i0..i2 around code it inserts); b2 the IR
  instruction lacks the skip-invalid flag (driver 0x1000; when clear, every
  instruction the driver generates gets skipinv); b4-7 the IR instruction's
  +0x88 low nibble, only when compile flag +0x9e bit 4 is clear (driver
  0x100..0x800; meaning **unknown**). (**writer**, **reader**)
- **Register types.** Two USP enums: 2-valued fields 1 = r, 2 = pa; 4-valued
  fields 1 = r, 2 = o, 3 = pa, 4 = i (writer table 0x136c18 maps USEASM
  types 0/1/2/7). The reader turns them into its own kind codes, which are
  also what its operand encoders take: 0 r, 1 pa, 2 sa, 3 o, 4 i, 6 c/g
  constant, 7 immediate (`FUN_000573a0` bank table). (**writer**, **reader**, **driver**)

### Block 5: SAMPLE (56 bytes)

| off | type | field | values / meaning | tag |
|---|---|---|---|---|
| 0 | u16 | flags | b0 non-dependent (IR sample +0xa0), b1 projected (+0x98, only with b0), b2 IR +0x9c (driver +0x70, matched against the iterator's third key; **guess**: centroid) | writer, reader; corpus: proj4 sets b1 |
| 2 | 18 B | fmt ctrl + MOE | see above; c3 is always `2, 8001×4, 0×4` except `tx_dep_loop` | writer |
| 20 | u32 | sample id | pairs the block with its unpack (block 6); 0, 1, ... per shader | writer, reader, corpus |
| 24 | u16 | iflags | see above | writer |
| 26 | u16 | sampler | texture unit, 0..15 (reader rejects >15) | writer, reader, corpus (`tx_nd_two` 0/1, `tx_nd_same2` 0/0) |
| 28 | u16 | swizzle | 4 × 3 bits: result channel i ← texel channel sel_i (0-3 = xyzw; ≥4 not seen). Bits 12-15 must be 0. The compiler folds a following channel reorder into it: `0x60a` = zyxw when the texel goes straight to the BGRA colour output, `0x688` = xyzw otherwise | writer (IR +0xac), reader (+0x58), driver (`FUN_0005cb10`), corpus |
| 30 | u16 | dest type | 2-valued: where the smp writes the raw texel data | writer (IR dest 0), driver (+0x90: smp destination base) |
| 32 | u16 | dest number | | writer, driver |
| 34 | u16 | usrc type | 4-valued: IR dest +0xb0; the driver compares this register with the unpack's source and first destination and sets "in place" (sample +0x2d8) when they match | writer, driver (`FUN_00053d00`); meaning **guess** |
| 36 | u16 | usrc number | | writer |
| 38 | u16 | scratch type | 2-valued: first temporary the driver may use for its own code (iReg saves, state loads, coordinate fix-ups) | writer (IR +0xb4/0xb8), driver (+0xa0/+0xa4) |
| 40 | u16 | scratch number | c3: r20..r83 fragment, r294/r302 vertex | writer, driver |
| 42 | u16 | ilive | mask of internal registers live across the sample | writer (`FUN_00101c30` state), driver (+0x544) |
| 44 | u16 | ilive C10 | the subset holding C10 data (saved with two instructions instead of a mov) | driver (+0x546) |
| 46 | u16 | forced format | result format override: 8 = none (the reader's table 0x750dc maps n-1 → 1 f32, 2 f16, 3 c10, 4 u8, 6, 7, else none); only written ≠ 8 under a compile option (state +0xaf bit 0), from IR +0xb0 (0→1, 4→7, 5→6) | writer, reader; 6/7 **unknown** |
| 48 | 8 B | dependent: smp template | the smp libusc encoded (`FUN_00018a50` → `FUN_0010a3f0`), with the destination zeroed and IR args 0, 6, 11, 12 renumbered from their +0x24 field. Identical to the smp words logged in `ef_c3.txt` for every dependent shader | writer, corpus |
| 48 | 4 u16 | non-dependent | coordinate iterator + 1 (reader stores n-1, accepts 1..10), coordinate dimension (≤3; 2 = 2D, 3 = cube), coordinate register type (4-valued), number. The reader substitutes the template `0x20000000 / 0xe0411000` | writer, reader, corpus |

What the compiler makes non-dependent (corpus): `texture2D(s, v)`,
`texture2DProj(s, v4)` (proj bit), `textureCube(s, v)` (dim 3) with `v` an
unmodified varying and no bias. Everything else is dependent: swizzled
(`a.zw`), `texture2DProj` with a vec3 (the divide is computed into i0, coord
`r124`), any bias or LOD, uniform coordinates, all vertex shaders.

Dependent templates in c3 (decoded with `smpblock.smp_fields`):

| shader | template (lo hi) | reading |
|---|---|---|
| tx_dep | a0000000 e0031404 | smp2d, coord pa0, state `#0` placeholder, src2 `#0` |
| tx_dep_m / _l | e003140c / e0031414 | h3 = F16 coordinate (mediump), h4 = C10 coordinate (lowp) |
| tx_dep_bias | b0000009 e0021504 | h8-9 = 1 bias, src2 = sa9 (the uniform) |
| tx_dep_biasv | a0000004 e0021504 | bias from pa4 (a varying) |
| tx_dep_projb | 901f0034 e0031500 | coord i0 (r124), bias c52 = 1.0 |
| tx_dep_cube | a0000000 e0031804 | h10-11 = 2: 3-component coordinate |
| tx_dep_u | a0024000 e0071404 | coordinate sa9 (h2 + h18) |
| tx_dep_if | a0010000 e0821704 | h8-9 = 3 gradients (inside `if`), src2 = pa0, skipinv |
| vtx_lod | 90000030 e0831604 | h8-9 = 2 explicit LOD = c48 (0.0), skipinv |
| vtx_lod2 | a0000004 e0821604 | LOD from pa4 |

### Block 6: SAMPLE UNPACK (74 bytes)

| off | type | field | meaning | tag |
|---|---|---|---|---|
| 0 | 18 B | fmt ctrl + MOE | as above | writer, reader |
| 18 | u32 | sample id | must match a block 5 (`FUN_00053d00` links them: sample +0x5d0) | writer, reader |
| 22 | u16 | iflags | | writer |
| 24 | u16 | channels | channels to write (IR unpack +8); must be a subset of the next field | writer, reader (+0x54) |
| 26 | u16 | mask | live channels of the destination (from the IR dest mask, `iStack_bc` per format) | writer, reader (+0x58) |
| 28 | 4 u16 | per-channel register type | 4-valued (0 for unwritten channels) | writer, reader (+0x5c + 0x14 i) |
| 36 | 4 u16 | per-channel register number | base + i / (channels per register) | writer, reader |
| 44 | 4 u16 | per-channel format | 1 f32, 2 f16, 3 c10, 4 u8 (writer table 0x136c34 from the IR dest format) | writer, reader, corpus (highp f32, mediump f16, lowp to the colour output u8) |
| 52 | 4 u16 | per-channel byte offset | 0 for f32; 0/2 for f16; 0..3 for c10/u8 (reader checks the bound per format) | writer, reader |
| 60 | u16+u16 | source | 4-valued type, number: where the unpack expects the raw data (IR arg 4) | writer, reader (+0xac/+0xb0) |
| 64 | u16+u16 | smp dest | 2-valued: IR arg 0 = block 5's dest | writer, reader (+0xb4/+0xb8), corpus |
| 68 | u16+u16 | scratch | 2-valued: temporaries for the conversion code (IR unpack +0xc/+0x10) | writer, reader (+0xbc/+0xc0) |
| 72 | u16 | ilive | internal registers live here | writer, reader (+0x2d4) |

### Block 9: TEXTURE WRITE (82 bytes; was 78 in usp.py)

20 u32 then one u16, in this order (writer case 0xa6, reader 0x513c7): IR
params +0x2c (an id); eight (register number, operand format) pairs for IR
args 0..7 (format code from `FUN_00103f30`: USEASM format 0→1, 2→2, 3→3,
6→4, 7→5; anything else is an error); IR params +4; a format from table
0x136c58 (IR params +0x70: 0..5 → 1, 2, 3, 4, 7, 6); `FUN_0009af00()`;
iflags. The reader keeps them in a 0x324-byte record (list at shader +0x34).
GLSL ES never produces it; the field meanings beyond the layout are
**unknown**.

### The finalizer (libGLESv2)

`PVRUniPatchSetTextureFormat` = `FUN_0004f1b0`/`FUN_0004f230`: per sampler
(16, 0x2c bytes each in the patch context) a 5-word format descriptor (+0xc,
word 0 is a format id; 0x21 is special-cased below), two words, and
`FUN_0004f230`'s (+0x28 = arg==0, +0x2c, +0x30 width?, +0x34 height?).

**Linking and planning** (`FUN_00053e10`):
1. `FUN_00053d00`: each sample finds its unpack by id.
2. `FUN_00066760` then `FUN_00064540` → `FUN_0005cb10` per sample: from the
   format descriptor and the swizzle, which *chunks* of the texel are needed.
   A texture format is split into up to 4 chunks (table 0x75ec0 gives bits per
   channel format: 8, 8, 16, 16, 16, 32, 24, 32, 32); chunk c returns
   `ceil(bits/32)` dwords at a dword offset in the raw area (+0x548 record:
   +0x550 needed-chunk mask, +0x5b8.. offsets/sizes). Channels can also be
   constant 0/1 (descriptor kinds 5/6), which need no chunk. A YUV-like case
   (all three of channels 0-2 of kind 2..4 from one chunk, flag +0x548) adds
   a FIRH colour conversion later (`FUN_0005a040`, coefficient tables
   0x75f48/0x75f6c).
3. `FUN_00065d30`: builds the final **iterator list** (what GL's PDS
   generator turns into DOUTIs; PDS.md "DOUTI"): for non-dependent samples it
   adds sample entries (key: coordinate iterator, dimension, flag b2) within a
   PA budget of `0x80 - header[0x3c]`. Only the **first** needed chunk of a
   non-dependent sample can come from the PDS (`FUN_0005d5a0`, tables
   0x76200 = lowest set bit, 0x76240 = the rest). +0x554 = chunks the PDS
   delivers.
4. Per non-dependent sample: `FUN_0005ac30` adds a *plain* iteration of the
   coordinate when some chunk still needs a USSE smp; `FUN_0005a800` places
   PDS-delivered chunks; `FUN_0005a420` allocates **texture state**: per
   (sampler, chunk) 4 consecutive dwords, placed in SA registers after the
   program's own (header +0x30/+0x32/+0x24..0x28/+0x36) or, when they don't
   fit (`header +0x30 + +0x34`), in memory. The table (7 dwords per entry:
   sampler, chunk, tag, 4, register, in-memory, tag2; in the program-output
   record at shader +4: list +0x30, count +0x20) is copied into the
   finalized program (`FUN_00054da0` sizes it at 0x1c bytes per entry) for
   GL to fill.
   Dependent samples get the same allocation.

**Code generation** (`FUN_00054570`): `FUN_000646c0` per sample (non-dependent
list +0x4c, then dependent list +0x40), `FUN_000655f0` per unpack. Generated
instructions go into a per-sample array (+0x270 words, +0x3a8 USP opcode,
+0x444 flags; opcodes are the driver decoder's enum, `FUN_00055d60`: hw class
→ 0x01..0x4c, smp = 0x2b + LOD mode, ld 0x2f, mov 0x10, pck 0x13, and 0x15,
limm 0x41, smlsi 0x39, smbo 0x3a, setfc 0x3c, wdf 0x3e).

`FUN_000646c0` → `FUN_0005d790` (the sample), in order:
1. If the MOE state at the sample isn't the default: `smbo` 0,0,0,0 (0x3a,
   `FUN_00058f30`) and/or `smlsi` 1,1,1,1 (0x39, `FUN_00058e40`).
2. If internal registers are live and some chunk needs a USSE smp: save each
   live i_n to scratch (`mov`, `FUN_00058b00`; C10 ones with a sopwm
   `FUN_00059c00` + `pck` pair), after a `setfc` if C10 ones are live.
3. Texture state held in memory: `ld` 4 dwords per chunk into scratch
   (`FUN_00059f70`: hi `0xe8800000 | (n-1)<<12`, base = SA header[0x24] +
   offset), merging adjacent chunks up to 16 dwords, then `wdf` (0x3e).
4. Sampler with ctx +0x28 set: coordinate fix-up before the smp:
   `fsubflr` (op 4, wrap) unless ctx +0x2c, `limm` 1/w (and 0.5/w), op 0x11
   (hw class 6) multiply-add; same for h. **Guess**: NPOT/rectangle
   emulation.
5. Per needed chunk not delivered by the PDS: copy the template from
   sample +0x7c/+0x80; non-dependent: set h10/h11 from the dimension
   (`FUN_0005a160`) and src0 = the plain iteration's PA register (`FUN_00057100`);
   set src1 = the chunk's 4 state registers (`FUN_000573a0`); set the
   destination to dest + chunk offset (`FUN_00056cc0`).
6. After the last smp: `wdf drc` (`FUN_00059bd0`: hi `0xf9200000 | drc`,
   drc = template h0-1, `FUN_00059bc0`).
7. Format id 0x21: `and dst, dst, #0x7fffffff` on the result (`FUN_00057cf0`).
8. Restore the saved internal registers.

Then back in `FUN_000646c0`: chunks the PDS delivered are found in PA
(`FUN_0005a2a0`) and, if needed, moved; the YUV conversion; `FUN_0005cfe0` and
**`FUN_0005fcc0` the unpack**: per output channel, by result format (block 6,
or the forced format): f32: `pck.f32.<src>` with normalise (`FUN_000595c0`
with dst code 7, src code from table 0x75fc4 by channel format: u8, s8, u16,
s16, f16, f32), or `mov` when the channel is already f32; f16: `pck.f16.<src>`
(dst code 6, table 0x76070); c10/u8: pck to c10/u8 or in-place when the raw
data already have that layout (with the swizzle); formats 6 and 7 other
paths (`limm 0xffff8000`, test). pck format codes: dst h6-8 and src h9-11 =
code-1 (`FUN_000595c0`), so 0 u8, 1 s8, 3 u16, 4 s16, 5 f16, 6 f32
(**driver**; names per USEASM order, **guess** for 2 and 7). Finally the MOE
state is put back (smbo/smlsi/setfc) if it was changed.

### The smp instruction, as libusc's FUN_0010a3f0 writes it

USEASM ops 0x6f..0x7a = smp{1d,2d,3d} × {normal, bias, replace, grad}.

| field | bits | source |
|---|---|---|
| opcode | h27-31 = 0x1c | |
| predicate | h24-26 | flags1 pred |
| skipinv / syncstart / nosched | h23 / h20 / h22 | flags1 b0 / b2 / b3 |
| write mask, or repeat | h12-15 mask; with h21 set, repeat-1 | flags1 b9-12 / b4-8 |
| LOD mode | h8-9: 0 none, 1 bias, 2 LOD, 3 gradients | opcode |
| dimension | h10-11 = dim-1 | opcode |
| dest | l21-27; h7 = pa, else r (only r and pa allowed) | arg0 |
| coordinate | src0 l14-20, bank h2, ext h18 (r, pa, o, sa) | arg1 |
| texture state | src1 l7-13, bank l30-31, ext h17 | arg2 |
| bias / LOD / gradients | src2 l0-6, bank l28-29, ext h16 (none: `#0`) | arg3 |
| coordinate format | h3 F16, h4 C10 | arg1 flags b22 / b24 |
| h5 | flags3 b0/b1 | **unknown** |
| h14-15 path | flags3 b2/b3/b15 on a feature-gated path | **unknown** |
| drc | h0-1 (0 or 1) | the arg after the last source, type 0xa |
| (check only) | next arg must be type 0x10, number 0x15 | |

### The texture-sample sequence a compiler must emit

The driver does all of this today; an own compiler that skips USP has to do
it itself. Field names as above; exact bit forms belong to `hw_mem.py`.

**State words (all cases).** For each (sampler, chunk) the texture unit needs
4 control words, the same 4 a PDS `DOUTT` sends (PDS.md: word 1 = width-1 /
height-1, word 2 = base address, the rest format/filter/wrap). Put them in 4
consecutive SA registers (load them in the secondary update program or by
PDS `DOUTD` into the SA bank), or keep them in memory and `ld` them into 4
temps followed by `wdf`. One chunk for any ≤32-bit texel format.

**Dependent 2D read** `c = texture2D(s, uv)`, uv computed in r2..r3, c wanted
as f32 in r8..r11:
1. (MOE state default, internal registers not live; otherwise save them.)
2. `smp2d drc0 r4, r2, sa[S]` — opcode 0x1c, h10-11 = 1, h8-9 = 0, dest r4,
   src0 r2 (2 consecutive F32 coordinates; h3/h4 for F16/C10 ones), src1 =
   first of the 4 state SAs, src2 `#0`, mask 1 (one dword for RGBA8).
3. `wdf drc0` before the first use of r4.
4. Unpack r4: four `pck.f32.u8` with normalise, one per channel, each
   picking byte `swizzle[i]` of r4 into r8+i (or `pck.f16.u8` two per
   register for mediump; for lowp output to the colour register, the raw
   RGBA8 can be written to the output as is with the channel order chosen
   by the swizzle).

**Cube read** `textureCube(s, d)`: as 2D with h10-11 = 2 and a 3-register
coordinate (the direction); the state words describe the cube texture.

**Bias read** `texture2D(s, uv, b)`: h8-9 = 1 and src2 = the bias (any F32
source: pa, sa, r, or a hardware constant such as c52 = 1.0).

**LOD read** `texture2DLod(s, uv, l)`: h8-9 = 2 and src2 = the LOD.
Gradients (`h8-9 = 3`, what libusc emits for a sample inside non-uniform
flow) take src2 = the first of the derivative registers.

**Vertex texture fetch**: the same smp with h8-9 = 2; `texture2D` in a vertex
shader uses LOD c48 (0.0) since there are no derivatives; libusc also sets
skipinv. No PDS sampling in vertex programs: every read is a USSE smp.
Result and unpack as above (c3's vertex reads come back as f16 pairs).

**Non-dependent fragment read** (coordinate = an untouched varying): no
instruction. Emit an iterator entry with a sample: PDS `DOUTI` with the
coordinate set + 1 in bits 0-4, 2-D (0x200) or projected (0x400), followed by
the `DOUTT` with the 4 state words; the texel lands in the next PA registers
in raw format, and only the unpack runs in the USSE. A texel format needing
more than one chunk gets its other chunks by smp from a plainly iterated
coordinate (template `0x20000000 / 0xe0411000`: src2 `#0`, nosched, mask 1).
The PDS side (DOUTI/DOUTT bit layouts, `FUN_00046b00` = 0x36b00, `FUN_00047b90`
= 0x37b90) is in `re/pds/PDS.md`.

### Open

- Block 5 flag b2 (IR sample +0x9c) and the "usrc" pair: guessed meanings.
- iflags b4-7; smp h5 and the h14-15 path; forced formats 6 and 7.
- Which GL code writes the exported texture-state table into SA registers
  (not traced past `FUN_00054da0`, which builds the finalized program).
- `FUN_00066760` (1121 lines) and `FUN_0005fcc0` cases 6/7 were only skimmed.
- Block 9 field meanings; never produced by GLSL ES.
- Gradient operand layout (how many registers, which order): one corpus case.

## The USP container

Read from libusc's writer (`tools/intern/usc2/uspbin.c`; in
`libusc.so.1.7.862890`: header and lists `0xf28a0` = `FUN_001028a0` in the
decompile, code-block walker `0xf42a0`, code block `0xf4090`, branch `0xf26e0`,
labels `0xf2380`/`0xf23d0`, secondary-program writer `0xf2660`, record kind 3
`0xf2250`) and from the driver's reader (libGLESv2 `FUN_000511d0`, block type
1, in `$S/gles_sw.c`), which copies the header into a 0xbc-byte record (the
*record* below, `rec+N`) that the finalizer and `FUN_00054da0` (the finalised
program, *PROG*) read. **Confirmed**: `usp.py` sizes every list from the header
alone and parses all 552 shaders of c1-c3; every one ends exactly on the end
block at the declared size.

Everything goes through u32 / u16 / raw-byte write callbacks, packed with no
padding, so a u16 field leaves the next u32 unaligned.

### Header

The writer's sources: *state* is libusc's compile state, *SAO* the driver's
`psSAOffsets` parameter block (state +0x714; glCompileShader fills it, e.g.
SAO[6] = 9 / 10 and SAO[7] = 0x77 / 0x1f6 for fragment / vertex). Tags: writer
(field source), reader (where the loader puts it), driver (what reads it),
corpus (values in c1-c3).

| off | type | field | meaning | tag |
|---|---|---|---|---|
| 0x00 | u32 | magic | `0x55535020` ("USP ") | writer, reader |
| 0x04 | u32 | version | `0x23` (the reader rejects anything else) | writer, reader |
| 0x08 | u32 | size | size of the binary **minus 12** | writer |
| 0x0c | u32 | block type | 1: PC shader header (the reader requires it first) | writer, reader |
| 0x10 | u32 | program type | 0 fragment, 1 vertex, 2 geometry (SAO[0]) | writer, reader |
| 0x14 | u32 | flags A | 2 = state +0x9c b20; 4 = +0x9c b21 (the finalizer then lets the colour stay in pa0 in output mode 3); **8 = feedback split**: libusc split the program at a feedback point (`psPreFeedbackBlock`, `psPreFeedbackDriverEpilogBlock`, finalise.c), PROG flag 0x40; 0x10 = state +0xa0 b1, PROG flag 0x80. Corpus: 2 most texture shaders, 8 dc_* except dc_always, else 0 | writer, reader, corpus; meaning of 2/4 unknown |
| 0x18 | u32 | compile flags | state +0xac verbatim (the flags glslcompiler passes to libusc). b5 (0x20) = the driver will insert visibility feedback (finalise.c only builds the pre-feedback epilog block when it is set); discard shaders also get b11 (0x800); b3 with dynamically indexed uniforms (ix_n, vix_*); b29/b30 vary | writer, corpus; other bits unknown |
| 0x1c | u32 | flags B | 1 = state +0x9c b14 (all dc_*), PROG 1; 2 = the last instruction cannot take `end` (read by `FUN_00054da0` with the block list), PROG 2; 4 = state +0x7cc ≠ 0 or +0xa0 b10, PROG 4 (sets `phas` h13 and fragment state +0x22c = 2 at draw time); 8 / 0x10 = state +0x9c b11 / b12, PROG 8 / 0x10 (0x10: the finalizer counts one more colour register); 0x20 = +0x9c b4; **0x40 = no secondary update program** (state +0x70c empty), PROG 0x100. The finalizer adds 0x100 when a branch is out of range (PROG 0x800) | writer, reader, driver, corpus |
| 0x20 | u32 | SAO[0x1a] | 1..10 (reader rejects others), always 9; PROG +4 | writer, reader; unknown |
| 0x24 | u32 | SAO[0x1b] | always 0x2793; PROG +8 | writer; unknown |
| 0x28 | u32 | workaround count | u32 ids after the header: 0x54c1, 0x54d1, 0x5518 (21697, 21713, 21784) when state +0xbc b0/b1/b2; none in the corpora | writer, reader; hardware-erratum (BRN) numbers is a guess |
| 0x2c | u32 | flags C | which SA areas exist (`FUN_00054da0` turns them into PROG +0x30 bits): 1 = list B non-empty; 4 = memory area 0x34/0x60 (state +0x788); 8 = constants in SA (0x3c, 0x7e; state +0x13ec); 0x10 = indexable temporaries in memory (0x40, 0x4a; state +0x9c b2). Corpus: 8 with uniforms, 0x18 with temp arrays | writer, reader, driver, corpus |
| 0x30 | u16 | SAO[1] | 0; PROG +0x34 (flags C 1) | writer, driver |
| 0x32 | u16 | | 0 | writer |
| 0x34 | u16 | SA of memory area | SAO[2] when flags C 4, else 0xffff | writer, driver |
| 0x36 | u16 | | 0xffff | writer |
| 0x38 | u32 | in-memory constants | state +0x110 (0 in the corpora) | writer |
| 0x3c | u16 | constant SA base | first SA register holding constants (SAO[6]: 9 fragment, 10 vertex; `tx_dep_bias` reads its uniform from sa9). List C register numbers are relative to it | writer, driver, corpus |
| 0x3e | u16 | constant SA count | SA registers available from 0x3c (SAO[7]: 0x77, 0x1f6); the finalizer puts texture state after the program's SAs and moves it to memory when it doesn't fit below 0x3c + 0x3e | writer, driver |
| 0x40 | u16 | scratch SA | SA register with the address of the indexable-temporary memory (SAO[8], 1 in ix_comp/ix_tmp*), else 0xffff | writer, corpus |
| 0x42 | u16 | SAO[0xf] | 0; rec +0x3c, PROG +0x26; the finalizer's PDS budget is `0x80 - rec+0x3c` | writer, driver; meaning unknown |
| 0x44 | u16 | PA count | primary attribute registers the main program uses (fragment: iterated inputs, vi_4 = 16; vertex: attribute registers) | writer, driver, corpus |
| 0x46 | u16 | temp count | temporaries (r) of the main program (0 straight-line, 5 loops, 13 fl_big); the finalizer raises it for its own code | writer, driver, corpus |
| 0x48 | u16 | secondary temp count | temporaries of the secondary update program (mod_m: r0-r4 = 5, sign 4, div_m 1) | writer, corpus |
| 0x4a | u32 | scratch size | bytes of indexable-temporary memory (state +200 × +0x734; ix_comp 0x1000, ix_tmparr 0x4000) | writer, corpus |
| 0x4e | u16 | list A entries | iterated inputs (fragment only; vertex 0) | writer, reader |
| 0x50 | u16 | list C records | constants in SA registers | writer, reader |
| 0x52 | u32 | list B records | constants in memory | writer, reader |
| 0x56 | u16 | list D entries | 3-u16 entries (libusc 1.7 writes 0) | writer, reader |
| 0x58 | 2 u32 | sampler mask | one bit per sampler (≤ 16, asserted), state +0x744; set only by the vertex texture shaders (vtx_*) | writer, corpus; meaning unknown |
| 0x60 | u32 | memory area size | state +0x788 × +200 × 4 (flags C 4) | writer |
| 0x64 | u16 | | -(state +0x7bc × 4); 0 | writer; unknown |
| 0x66 | u16 | colour type | fragment: register type of the colour result, 4-valued (1 r, 2 o, 3 pa): 3 | writer, reader, corpus |
| 0x68 | u16 | colour register | fragment: its number (pa0; pa2 in dc_gt) | writer, reader, corpus |
| 0x6a | u16 | colour format | fragment: 1 f32, 2 f16, 3 c10, 4 u8 (4 everywhere: packed BGRA8) | writer, reader, corpus |
| 0x6c | u16 | colour registers | fragment: 1 | writer, reader |
| 0x6e / 0x70 / 0x72 | u16 | colour alternatives | fragment: the r, pa and o register the finalizer may move the colour to (output modes 2, 3, 1 below) | writer, driver |
| 0x74 | u16 | start label | label at the main program's start (3, fn_call 4). The loader opens a code block there; the finalizer puts its placeholders in it | writer, reader, driver, corpus |
| 0x76 | u16 | feedback label | label just before the discard `nop` (the pre-feedback driver epilog), 0xffff if none. PROG +0x16 = its instruction index | writer, reader, driver, corpus (dc_* 4) |
| 0x78 | u16 | phase-1 label | label after the epilog, where the post-feedback phase starts. PROG +0x18 | writer, reader, driver, corpus (dc_* 5) |
| 0x7a | u16 | fourth label | PROG +0x1a; 0xffff in the corpora | writer, reader; guess: the sample-rate split point (`psPreSplitBlock`) |
| 0x7c | u16 | secondary instructions | length of the secondary update program | writer, reader |
| 0x7e | u16 | SA constant count | SA registers from 0x3c holding constants (flags C 8; tx_dep_bias 1) | writer, driver, corpus |
| 0x80 | u16 | input mask bits | vertex: the mask's bit count (SAO block +0x14) | writer, reader |
| 0x82 | u16 | output mask bits | always 0x80 | writer, reader |

The colour *output mode* is the patch context's +0x2cc, set per draw
(`FUN_0004f2a0`): 0 when blend or colour-mask code will be appended (the
colour stays where it is), 1 otherwise (moved to the o register: the
hardware capture's `mov o0, sa9`); the finalizer (`FUN_000541a0`,
`FUN_000546f0`) adds the move. (**driver**, **hardware**)

### Lists and blocks

After the header, in this order (writer and reader agree):

1. **Workaround ids**: header 0x28 u32s.
2. **List A** (fragment): 12 bytes (6 u16) per iterated input, in PA
   order: flags (4 or 0xc for a plain iteration; a sample entry has the
   sampler in the next field; b1 from another input flag; other bits not
   traced), sampler, iterator (texture coordinate set + 1 for varyings;
   0xb/0xc, 0xd, 0xe, 0xf for the writer's other input kinds: the
   draw-time builder iterates the first two from DOUTI source 0x20, the
   colour, and the rest from source 0), components, format (1 f32, 2 f16,
   3 c10: highp, mediump, lowp in the corpus), registers (PA registers it
   takes: 4 for a highp vec4, 2 for a mediump one). A highp `a` gives
   `(4, 0, 1, 4, 1, 4)`. (**writer**, **driver** `FUN_00046b00`, **corpus**;
   which GLSL input each of 0xb-0xf is, gl_FragCoord, gl_PointCoord or
   gl_FrontFacing, is open)
3. **List B** then **List C**: constant records whose first u16 is a kind.
   Kinds 1, 2 and 4 are 8 bytes; kinds 3 and 5 are 10. Kind 1 `(1, n, n, 0)`
   is one uniform component; 2 is two (F16 halves, `(n & 3) != 3`), 3 a C10
   triple, 4 a 32-bit value. List B holds constants the driver leaves in
   memory, list C those it loads into SA registers from header 0x3c on
   (`FUN_00054da0` adds 0x3c to list C's register field and copies both into
   PROG). Only list C occurs in the corpora. (**writer**, **driver**)
4. **List D**: header 0x56 entries of 3 u16 (reader checks ≤ 16, ≤ 4);
   never written.
5. **Secondary update program**: header 0x7c instructions, 8 bytes each.
   **Confirmed** separate from the main program: the compiler moves all
   uniform-only math here (`gl_FragColor = -u` puts the negate here and
   leaves one instruction in the main program). It runs once per draw and
   its pa registers are the main program's sa (**hardware**, re/NOTES.md).
6. Vertex only: the input mask, `ceil(header 0x80 / 32)` u32: the primary
   attribute registers the program reads (the PDS fetches those; corpus vo_*).
7. **Output mask**, `ceil(header 0x82 / 32)` = 4 u32 from state +0x7fc,
   always `ffffffff ffffffff 0 0`. libusc renumbers vertex outputs by its set
   bits (`FUN_0007d380`: output n → popcount of the bits below n), so with all
   64 bits set the numbering is the identity. Its meaning beyond that is a
   **guess** (the output registers that exist).
8. **Block stream**, each block a u32 type and a body, until type 7:

| type | body | meaning |
|---|---|---|
| 2 | u16 n, u16 flags, 4 u16 MOE state, 4 u16 MOE base offsets, n u16 per-instruction flags, n × 8-byte instructions | straight-line code. flags = format control at block entry (bit 0 EFO F16, bit 1 C10; 2 unless the code uses `setfc`; **corpus**). MOE state per operand: 0x8000 \| increment or 0x4000 \| swizzle |
| 3 | 8-byte instruction, u16 label | branch (the target is patched at finalize) |
| 4 | u16 label | label; header 0x74-0x7a name the ones the driver patches at |
| 5 | 56 bytes | texture sample: non-dependent (iterated coordinate) or dependent (libusc's encoded smp as a template); see the sample-block section |
| 6 | 74 bytes | sample unpack: texel data to the shader's registers and formats |
| 9 | 82 bytes | texture write (not produced by GLSL ES) |
| 7 | none | end |

The GLES driver's USP finalizer generates the sampling code from the sample
blocks at link time, for dependent and non-dependent reads alike, which is
why no `smp` appears in USP code.

Each instruction is 64 bits, stored as (low word, high word). The disassembler
names the bits l0..l31 and h0..h31.

## Phases, visibility feedback and discard

A USP fragment program is not runnable as it stands: the finalizer and the
draw-time builder `FUN_00046b00` (libGLESv2 0x36b00, also the DOUTI builder
of re/pds/PDS.md) wrap it in phase instructions, and for `discard` add the
visibility feedback. Read from the decompile and, where Ghidra merged
variables, from the x86 (`objdump -d --start-address=0x36b00`). Tags:
**driver** (read from the code), **hardware** (the probe capture in
re/NOTES.md), **guess**.

### What the finalizer prepares

- The loader opens a code block at the start label (header 0x74), and
  `FUN_00054b80` puts *k* placeholder instructions (driver opcode 0x60) into
  it, k = patch context +0x2d0 = 1, or 2 when the draw needs feedback
  (`FUN_0004f2b0`, set from the draw state below). PROG +0x14 = the index of
  the first placeholder. (**driver**)
- PROG +0x16 = the instruction index at the feedback label (0x76): the USP's
  discard `nop`. PROG +0x18 = the index at the phase-1 label (0x78).
  (**driver**)
- PROG byte 0 flags: 1, 4, 8, 0x10 = header flags B 1, 4, 8, 0x10; 2 = the
  program's last instruction cannot carry `end` (flags B 2, or the last
  instruction's opcode, `FUN_000593e0`); 0x40 / 0x80 = flags A 8 / 0x10;
  0x100 = flags B 0x40; 0x800 = branch fix-up; 0x20, 0x200, 0x400 come from
  finalizer state that was not traced. The draw-time builder reads 2, 4 and
  0x40 (0x40 only to add one more `phas` when blend code gets a phase of its
  own in a feedback program). PROG +0x78 = the colour register after the
  finalizer's move. (**driver**)

### The draw state (ctx +0x7a0)

`FUN_00049a00` sets ctx +0x7a0 together with ISPA (STATE.md: pass type in
b25-27) when the linked fragment shader's compiled-object flags (+0x1c,
glslcompiler's result +4) have b3, *the shader discards* (glslcompiler sets
b3 in `FUN_00019690` when the parser met `discard`, statement 0x23 in a
fragment shader, `FUN_0004c460`):

| ctx +0x7a0 | when | ISPA pass type |
|---|---|---|
| 0 | the shader doesn't discard | 0 opaque / 1 translucent (blend) |
| 1 | discards, triangles (ctx +0xc74 = the GL primitive 4-6) | 4 punch-through, or 2 when blending |
| 9 | discards, points or lines (primitive 0-3, or 7) | 5 |

(**driver**; the pass-type names are STATE.md's.)

### What the draw-time builder emits

Instruction words as written (low high), decoded with `dis.py -x`:

| words | decodes as | role |
|---|---|---|
| `00000000 fa440700` | `phas.i #0, #0 [wait=7]` | single phase: no next phase |
| `pc>>3 fa440800 \| t` | `phas.i #pc, #t` | phase 0 → phase 1 at `pc` (byte address relative to the code heap, /8); t = (temps + 3) / 4 in h0-7; h13 = PROG flag 4; h11 (`rate=1`) when the blend code runs in a phase of its own |
| `00000000 fa4c0700` | `phas.i #0, #0 [end wait=7]` | last phase ends here |
| `00000000 f8040140` | `nop [end]` | appended when PROG flag 2 says the last instruction can't take `end` |
| `n<<14 \| (n+1)<<7 \| b0000004 f8300004` | `pcoeff pa<n>, ...` | n = PA registers taken by the program's iterated inputs: the first register after them |
| `c<<21 \| c<<14 \| f0000184 f9340406` | `atst8 pa<c>, p0.nw, pa<c>, ... [end]` with h10 set | c = PROG +0x78, the colour register |
| `a0000000 28a10002` | `mov pa0, pa0 [skipinv rcnt]` | a no-op in pcoeff's place (state 9) |
| `(n+2)<<14 \| f0000184 fb340404` | `(p1) depthf pa<n+2>, ... [end]` | built for state 9, see below |

Without feedback (ctx +0x7a0 bit 0 clear):
- placeholder 0 = `phas.i #0, #0 [wait=7]`, then the program, with `end`
  set (h18) on its last instruction, or `nop [end]` appended (PROG flag 2).
  **hardware**: the probe's finalised main program is
  `phas.i #0,#0 [wait=7]`, `mov o0, sa9 [end]`, and its secondary program
  starts with the same `phas`.
- Blend and colour-mask code (`FUN_000457d0`; the mask is the
  `sopwm ... 0x90b00601 | mask<<11` of STATE.md) is appended after the
  program; with ctx +0x1280 it gets a phase of its own (a `phas` to it, then
  `phas.i #0,#0`).

With feedback, ctx +0x7a0 = 1 (discard on triangles):

```
[+0x14]   phas.i #phase1, #temps4             (phase 0 runs up to the feedback)
[+0x14+1] pcoeff pa<n>, ...                   (PDS-iterated value, see below)
          ... the shader up to the discard test (writes p1 = keep) ...
[+0x16]   atst8 pa<c>, p0.nw, pa<c>, ... [end] (replaces the USP's nop; h10 set)
[+0x18]   phase 1: the rest of the shader (colour packing)
          phas.i #0, #0 [end wait=7]          (appended after the program)
```

- The PDS program gets one more plain iteration after the shader's own:
  DOUTI iterate half `0x7811000` (source 0x22, 3 components, 0x6000000),
  or `0x7c11000` (4 components) with ctx +0x1280; it fills pa<n>..: that is
  what `pcoeff` reads (**driver**; that source 0x22 is the triangle's depth
  plane, so `pcoeff` = hand the ISP its plane coefficients, is a **guess**
  from the name).
- **Which predicate kills**: atst8's h9-10, a PRED2 predicate (0 none,
  1 p0, 2 p1, 3 !p0, as in depthf). libusc's encoder never sets it
  (`{?h10}` in dis.py); the driver's word has 2 = p1, the keep predicate the
  USP's `test` computed. A pixel whose atst8 predicate is false is killed.
  **hardware** (EA3, `re/isa/discard/`, the left half of a triangle kept
  by `if (q.x > 0.0) discard`): the stock driver keeps exactly the left
  half (21600 of 43200 pixels); with h9-10 patched to 0 nothing is killed;
  with p0 / !p0 about 83 % / 19 % survive, scattered over both halves (p0
  is whatever the shader left there; the two are close to complementary).
- atst8's other operands (src1 = 3, src2 = 4 with bank bits l28-31 = 0xf)
  are fixed, and the colour register is both its source and destination.
  The colour's alpha does not matter: the same draw with alpha 0 keeps the
  same 21600 pixels (**hardware**), so whatever alpha test atst8 can do is
  off in GL's ISP state. Operand meanings not traced.

With ctx +0x7a0 = 9 (discard on points/lines): a second extra iteration
(`0x7800000`, source 0, 3 components) is added, pcoeff's slot gets
`mov pa0, pa0`, and the builder computes `(p1) depthf pa<n+2>, ... [end]`
(hi `fb340404`, predicated on p1). **But the x86 stores those two words into
its own stack slots (`0xb8(%esp)`, `0xbc(%esp)` at 0x37304-0x3731e), not into
the program**: the discard `nop` stays, and the slot at 0xbc is the temporary
count returned by the blend builder, so the program's temp count becomes
0xfb340404 (**driver**).

On hardware (EA3, 2026-10-28; `re/isa/discard/results.txt`):

| draw | stock DDK 1.7 | `depthf_patch.py` | `pt_patch.py` |
|---|---|---|---|
| triangle, discard right half | left half kept | same | same |
| 4 points (size 8), discard right two | **all 4 drawn**: discard ignored | left two kept | left two kept |
| 3 lines, discard right half | **frame broken**: 88 % black, no clear, no SGX recovery | same | same |
| 3 lines, discard never taken (`q.x > 5.0`) | frame broken | | same |
| line strip, or a pbuffer | | | frame broken |

- `depthf_patch.py` rewrites those 33 bytes to store the depthf through the
  slot pointer (and leaves the temp count alone): points then discard
  correctly in the depth-only pass (type 5) with `(p1) depthf`. So depthf's
  predicate kills like atst8's (**hardware**).
- `pt_patch.py` instead sends points and lines down the triangle path
  (0x39d55 in `FUN_00049a00`: pass type 4/2, atst8): points discard
  correctly that way too.
- **Lines with any discarding shader render a broken frame** on every
  path, also when the discard is never taken, so it is the feedback
  program with lines (pcoeff's plane-coefficient iteration, or the ISP's
  line handling), not the kill. Lines without discard draw normally. Not
  explained; avoid discard on lines (or draw lines as thin triangles).

### What a compiler must emit to discard

Nothing is "killed" in the USSE: a discarding pixel shader is a two-phase
program whose phase 0 ends in a visibility test, and the ISP decides.

1. Compute the keep condition into a predicate (libusc uses p1).
2. Start the program with `phas.i #<phase-1 address>, #<temps/4>` and
   `pcoeff pa<n>, ...` (with the PDS iterating the extra 3-component
   source-0x22 input into pa<n>.. after the varyings).
3. Run everything the feedback depends on in phase 0, then
   `atst8 pa<c>, p0.nw, pa<c>, #3?, #4? [end]` with h9-10 = 2 (p1):
   copy the driver's words `f0000184 | c<<21 | c<<14`, `f9340406`.
4. Phase 1 (the colour output) follows, ending with
   `phas.i #0, #0 [end wait=7]`.
5. ISPA pass type 4 (2 when blending) for triangles, and also for points
   (hardware). Points also work with pass type 5 and
   `(p1) depthf pa<n+2>, sa3, sa4 [end]` in atst8's place (the driver's
   intent, after `depthf_patch.py`). Lines with a feedback program break
   the frame either way (hardware): don't discard on lines.

Open: the atst8 / depthf / pcoeff operand meanings, the 0x22 iterator
source, why lines break, and whether an unconditional discard
(dc_always: flags A 8 clear, so PROG 0x40 clear) behaves differently.

## What a compiler needs

A summary for a backend (a future Mesa gallium driver). Everything here is
backed above; this section only collects it.

### Register files and operand limits

- **Files**: see "Registers". Allocatable temporaries are r0-r123: r124-r127
  are the internal registers i0-i3 (efo, fddp, fdual and the sample code the
  driver generates use them; libusc saves live ones around samples).
- **Slots**: a destination takes r/o/pa/indexed, or with its extension bit
  sa/g/idx/i (some forms have no extension bit: fddp, fdual, efo). In the
  float classes src0 is r or pa only; constants (c, g), immediates and
  internal registers go in src1/src2. src0 can't be indexed.
- **Immediates**: 7 bits in a source (signed in some integer forms); a
  bitwise op's src2 takes a 16-bit rotated immediate; anything bigger is
  `limm` or a secondary attribute.
- **Format select** (float classes, h22): any F16 source switches the whole
  instruction to 6-bit numbers, so every source of it must be r0-r63 (etc.).
- **Format control** (`setfc`, USP code block flags): while on (the
  compiler's default state is C10 on, EFO F16 off), number bit 6 of the
  colour/integer classes' operands (efo, sop2/sop3/sopwm, dot, ima8, fpma)
  marks a C10 (F16 for efo) operand, so those instructions can only name
  registers 0-63. Emit `setfc` to change it; the state isn't in the
  instruction.
- **Repeats**: a vec4 op is one instruction with a repeat count, not four:
  float 3-source ops repeat up to 4 (h12-13), scalar ops and mov up to 16;
  with `moe` each operand steps by the MOE increment (default 1), else only
  operands with their inc bit step. Swizzles and strides are MOE state:
  `smlsi` before, `smlsi #1, #1, #1, #1` after (the USP code block header
  records the MOE state at block entry). Scalar ops (hw 1) and moves can
  use a write mask instead of a count.
- **Constants**: the hardware constant table covers 0, 1, powers of two,
  1/2^n, e, π fractions, √2; anything else is a uniform in sa. Negation and
  abs are free on most float sources (not on src0's negate: fold it into
  another source as libusc does).

### Program structure and calling conventions

- A shader is a **secondary update program** (runs once per draw: loads
  uniforms from memory with `ld`, waits with `wdf`, and does uniform-only
  math; its pa registers are the main program's sa) plus the **main
  program**. Every secondary program starts `nop [nosched]`,
  `test.and.ne p0, #0, #0`, `(p0) ld ...`, `wdf`.
- The last instruction of a finalised program has `end` (h18; hardware).
  USP code leaves that to the finalizer.
- **Branches**: `br` offset = target index - this instruction's index,
  signed 18 bits, in instructions; `ba` is absolute (plus the program's load
  offset). Conditions are a `test` writing p0-p3, then a predicated `br`.
- **Calls**: `br [link]` to the function's label; the function returns with
  `lapc`; arguments and results in ordinary registers.
- **Loops**: do-while with a float counter (`fmad r, r, c52, c52` = +1) and
  `test.fsub.lt` against the bound; a guard branch when the trip count may
  be zero; `break` = predicated forward branch; `continue` = predication.
  libusc unrolls nothing and uses no hardware loop counter.
- **if/else**: libusc if-converts (both sides, predicated `mov`), even for
  large bodies; branches only around texture reads.
- **discard**: a `test` writes the keep predicate (p1, the inverted
  condition); the USP holds a `nop` at the feedback label. At draw time the
  program becomes two phases: `phas` + `pcoeff` at the start, the nop
  replaced by `atst8 ... [end]` predicated on p1 (h9-10 = 2; a false
  predicate kills, hardware), the rest in phase 1, and ISPA pass type 4.
  Not on lines: any feedback program breaks a lines draw (hardware). See "Phases, visibility feedback and
  discard" for the words and what is a guess.
- **Phases**: every finalised fragment program (main and secondary) starts
  with a `phas.i` (`phas.i #0, #0 [wait=7]` for a one-phase program;
  hardware) and ends with `end` on its last instruction or an appended
  `nop [end]`; a feedback program ends with `phas.i #0, #0 [end wait=7]`.
- **Loads and samples** set a DRC (drc0/drc1): `wdf drc` before the first
  use of the result; alternate DRCs to keep two in flight.

### Comparisons and conversions

- Float compare: `test.fsub.<cond> pN, a, b` with lt/le/eq/ne/gt/ge on a-b;
  against zero `test.fadd.<cond> pN, x, c48`. Vector compares: `testmask`
  then `movc.nz.i32 d, m, c52, c48` (1.0 / 0.0). Select: `movc.<cond>.<type>`
  (==0, !=0, <0, >=0 on src0).
- GLSL ES ints are floats (mediump F16 via fmad16, highp F32): truncate with
  `fsubflr` + subtract, divide with `frcp` + multiply. True integer ops for a
  backend: `ima32`/`ima16`/`imae` (add, sub, mul, mad, carry), `and`/`or`/
  `xor`/`shl`/`shr`/`asr`/`rol`, `test.isub16`/`test.iadd32` for integer
  compares, `pck` for conversions to/from 8/16-bit and C10/F16/F32. There is
  no integer divide.

### Fragment shaders

- Varyings arrive iterated in pa (USP list A: one entry per iterated input,
  with its precision); non-dependent texture results also land in pa (PDS
  DOUTI/DOUTT, re/pds/PDS.md).
- The colour is built in pa0 as packed BGRA8: two `pck.u8.f32 ... scale`
  with destination byte masks (byte 0 B, 1 G, 2 R, 3 A: hardware); the
  finalised program writes the colour to o0 (hardware, the probe). The USP
  header (0x66-0x6c) says where the compiler left it; blending and the
  colour mask are code the driver appends, reading it from there.
- Iterators: each list-A entry is one DOUTI in PA order; a varying's
  iterator is its *fragment-side* texture coordinate set + 1, and GL's
  linker maps it to the vertex shader's set (below).
- Texture reads: see "The texture-sample sequence a compiler must emit"
  above: `smp` (dimension, LOD mode, coordinate, 4 texture-state words in
  sa), `wdf`, unpack with `pck`. Projection has no bit: divide first.

### Vertex shaders (corpus c3 vo_*, vtx_*)

- Attributes arrive in pa, in order of first use, one register per
  component used (a vec4 takes 4); the USP's vertex bitmask is the set of
  live pa registers and header +0x44/+0x80 their count.
- **The output buffer** (what the TA reads per vertex) is: the position,
  o0-o3; then the enabled *texture coordinate sets* 0..9 in set order, each
  taking its size (1-4 dwords) with no gaps; then the point size, one
  dword, when the shader writes it. The driver builds the TA words from the
  vertex shader's compiled-object fields (glCompileShader copies them from
  glslcompiler's UniFlex program +0x34..+0x84): +0x20 bits 3-12 = set 0-9
  enabled, b13 (0x2000) = point size; +0x24.. = size per set; +0x4c.. =
  precision per set. Output select = `dwords << 16 | 0x3000`, `| 0x100` with
  point size (dwords = 4 + sizes + 1); TSP word 0 = 3 bits per set (1, 2, 3,
  or 5 for 4 components); TSP word 1 = the sets with precision value < 3
  (libGLESv2 at 0x2f4b0, `+0x248/+0x24c/+0x250`; re/state/STATE.md). The USP
  has none of this: its header has no output count. (**driver**)
- **Varyings to sets**: glslcompiler (`FUN_00029ff0`, 10 sets × 4
  components, "Failed to allocate varyings") takes the active varyings by
  type, mat4, mat2, vec4, mat3, vec3, vec2, float (table 0x464c0), within
  a type the largest first (by the symbol's +0x14, the array size: guess),
  and gives each row a new set while sets remain; past 10 it
  packs rows into free components of sets with the same precision.
  (**driver**; corpus: vo_vall's o0, o1, o2 (vec2), o3 (float) land at o4,
  o8, o12, o14 = sets 0-3 of sizes 4, 4, 2, 1.) Unwritten varyings get no set.
- **gl_PointSize**: when a vertex shader writes it, glslcompiler reserves
  set 0 *before* allocating varyings: enabled, 2 components, precision 1
  (`FUN_00029ff0`: `+0x218 = 1` first free set, component bits 0-1 used).
  The shader never writes it, so o4-o5 are a hole and varyings start at
  o6, with the point size last (vo_ps: o6; vo_psv: vec4 at o6-o9, size at
  o10) (**driver**, **corpus**). Guess: set 0 is where the hardware puts
  the point-sprite coordinate (gl_PointCoord) for points. The size is
  clamped first: glslcompiler applies ICode op 0x24 with the constant 511.0
  to it (`FUN_0003c5a0`; a min, guess) and libusc emits
  `fminmax oN, x, x, saK.f16` against the driver's range in sa.
- **Matching the fragment shader**: each shader is allocated on its own;
  GL's linker (0x2f4b0) matches varyings by name (type, size and
  invariance must agree) and keeps a per-program table, program +0xb0[fs
  set] = vs set (and +0xd8 the reverse). The draw-time DOUTI for a varying
  takes source = +0xb0[its set] + 1, so the fragment program reads the
  vertex shader's set into its own PA order. A compiler that compiles both
  stages can give a varying the same set on both sides and needs no table.
- **What a compiler must produce**: position in o0-o3; each varying in
  the o registers of its set (4 + the sizes of the lower sets); the point
  size, if written, after the last set, with set 0 reserved (2 dwords,
  unwritten) ahead of the varyings; and to the driver/TA: the set mask,
  sizes and precisions (output select and TSP words above). Outputs can be
  read back as sources.
- Vertex texture fetch is always a USSE `smp` with LOD (c48 = 0.0 for
  `texture2D`).
