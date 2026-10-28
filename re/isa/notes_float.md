## Common fields (all ALU classes)

| bits | field | meaning | evidence |
|---|---|---|---|
| h27-31 | opcode | hardware class | encoder |
| h24-26 | pred | 0 always, 1 p0, 2 p1, 3 p2, 4 p3, 5 !p0, 6 !p1, 7 pN (per-instance predicate) | encoder (FUN_00106f30 maps USEASM 0-7 to these codes); the pN name is a **guess** |
| h24-25 | pred (short form, fdual and some special ops) | 0 always, 1 p0, 2 p1, 3 !p0 | encoder |
| h23 | skipinv | skip invalid instances (libusc INST_SKIPINV; set on nearly every compiled instruction) | encoder; name from libusc's flag name |
| h20 | syncs | sync start (libusc INST_SYNCSTART) | encoder; name from libusc's flag name |
| h11 | nosched | no scheduling (libusc INST_NOSCHED, named in an asm.c assert) | encoder |
| h18 | end | last instruction of the program | encoder; hardware (the finalised probe programs end with it) |

## Operands (generic encoder FUN_00107830)

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

## Float classes (FUN_001147f0, hw 0-4, 6)

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

## Moves (hw 5)

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
(libusc's own compare pseudo-ops pin the sign values; notes_pack.md).
libusc's test types (asm.c g_auEncodeTest) are, in order: always, >0, >=0,
==0, <0, <=0, !=0, sign clear, sign set (specs 0x10, 0x1a, 0x6, 0x14, 0x19,
0x5, 0x18, 0x12, 0x11).

USEASM mov with a register-type-9 operand encodes on hw 1f (`f80000c0`:
type 9 <- src1; `f8000100`: dst <- type 9); see notes_mem.md.

## Dual issue (hw 1b, FUN_00113620, **encoder**)

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
uses it once, in `v_norm`); 0x65 is described in notes_int.md.

Checked: `check_float.py` (3217 oracle encodings over every operand type,
modifier, repeat/mask/increment, predicate, flag, swizzle, movc type and
condition and fdual pairing) and `hwcheck.py` (all corpus words of these
classes): no unexplained bit, every word round-trips.
