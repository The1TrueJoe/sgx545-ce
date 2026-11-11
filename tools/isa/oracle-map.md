# Oracle input map (spec side)

How the vendor assembler's input (USEASM opcodes, flag words, argument types) maps
to the forms in docs/ISA.md, for the oracle sweeps (`check_*.py`, `uinst.py`,
`oracle.py`). It is test material only: docs/ISA.md states the forms without it, and
nothing that ships reads this file (docs/CLEANROOM.md). Moved out of docs/ISA.md
2026-11-10, grouped by the ISA.md section it came from.


## Registers

From the `idx` row's encoding cell:

| idx | index registers | idx1, idx2 (USEASM type 4, 1-3) | set by an integer op (ima16/imae into idx), then `bank[idxN+off]` (**encoder**, **corpus**) |


Vendor-binary addresses (FUN_xxxxxxxx, Ghidra offsets) and vendor source/function
names were dropped from these excerpts; the USEASM numbers are as they were.

## Float, moves and dual issue (hw 00-06, 1b)

### Common fields (all ALU classes)

- h24-26 pred: USEASM predicate 0-7 map to the codes 0 always, 1 p0, 2 p1, 3 p2,
  4 p3, 5 !p0, 6 !p1, 7 pn.

### Float classes (hw 0-4, 6)

- hw0 sub 3: USEASM 0xcf "ffrc" = fsubflr(a, a) = fract.
- fmin/fmax: USEASM 0xc/0xd are rewritten to 0x11/0x12 (fmin(a, b) -> hw3 sub 1
  on (a, a, b), fmax(a, b) -> sub 0 on (a, b, a)).
- Repeat, hw0/2/3/4: with h21 clear, h15/h14/h8 (first/second/third source
  steps) = USEASM flags2 bits 29/30/31.
- h22 fmt: set when any source has USEASM argument flag 22.
- hw1 source format: h8 (packed source, h3-4 channel) = USEASM argument flag 24.
  h2 = USEASM flags2 bit 22.
- fmad16 per-source swizzle = USEASM argument flags 25-26.
- fdpc clip-plane operand cp0-7 = USEASM register type 0xd.

### Moves (hw 5)

- op0xf3.s6 / op0xf3.s7 = USEASM 0xf3, target feature 0x2000; never emitted by
  the vendor compiler. Flags: `f1_1` = h7 and `f1_26` = h22 (the USEASM flags
  that pick movc's u8 / f32 types), `a3f` = h5-6 (the third source's argument
  flags 5-6, 0-3).
- movc conditions 0 `z`, 1 `nz`, 2 `ltz`, 3 `gez` are the only USEASM test specs
  movc accepts. USEASM test spec: bits 0-1 sign test (1 sign set, 2 sign clear),
  bits 2-3 zero test (1 zero, 2 nonzero), bit 4 AND instead of OR.
- The vendor assembler's test types are, in order: always, >0, >=0, ==0, <0,
  <=0, !=0, sign clear, sign set (specs 0x10, 0x1a, 0x6, 0x14, 0x19, 0x5, 0x18,
  0x12, 0x11).
- USEASM mov with a register-type-9 operand encodes on hw 1f (`f80000c0`:
  type 9 <- src1; `f8000100`: dst <- type 9).

### Dual issue (hw 1b)

- USEASM flags1 bit 25 pairs an instruction with the next (target feature
  0x100).
- pri: 6 ima32 (USEASM 0x65), 7 fssq (USEASM 0xa7).
- sec: 3 fsubflr (USEASM fadm and ffrc encode as it too), 14 ima32 (USEASM
  0x65).
- msrc: 4/5/6 = USEASM int-source-selects 4/5/6 (src0/src1/src2 slot).
- fssq = USEASM 0xa7.

## Pack/unpack, test, testmask, bitwise (hw 08, 09, 0a-0e, 0f)

Encoders (function column dropped):

| what | USEASM ops |
|---|---|
| pack/unpack, hw 08 | 0x14-0x4e, 0x16f, 0x170 |
| test (hw 09) / testmask (hw 0f) | any ALU op with flags1 b30 (TEST); the dispatcher sends it there before anything else |
| bitwise, hw 0a-0e | 0x4f-0x56 |

### Shared ALU control bits (all three encoders)

| bits | field | USEASM source | tag |
|---|---|---|---|
| h24-26 | pred | flags1 b13-16 | encoder |
| h23 | skipinv | flags1 b0 | encoder |
| h22 | nosched | flags1 b3 (h22 here, not h11 as in the float classes) | encoder |
| h20 | syncs | flags1 b2 | encoder |
| h18 | end (pck, bitwise only) | flags1 b17 (test rejects it) | encoder |
| h21 | rcnt (pck, bitwise) | set when flags1 repeat count n>0 | encoder |
| h12-15 | rpt = n (stored n-1) when rcnt; else mask = flags1 b9-12 verbatim | flags1 b4-8 / b9-12 | encoder; per-iteration write enable: **hardware** for the scalar class (hwt_p3 `frcp_mask5_*`), assumed the same here |

### hw 08: `pck.<dst>.<src>`

- The format code -> name map follows the USEASM op enum, whose order
  PCK<dst><src> is fixed by the corpus: 0x20 = F32->U8 color writes, 0x4e = C10
  lowp, 0x14 = F32->F16 for mediump.
- dmask = USEASM arg0 flags b7-10.
- l16-17 src1 lane = USEASM arg1 flags b5-6; l14-15 src2 lane = USEASM arg2
  flags b5-6.
- l18: `scale` = flags2 b8; `rint` = flags1 b24. The vendor compiler's trunc
  (intermediate op 0x96) is `pck.f32.f32 [rint rnd=zero]`.
- l19-20 rnd: arg3 must be type 0x10: USEASM 0x3a -> 0, 0x39 -> 1, 0x3b -> 2,
  0 -> 3.
- Immediates: the range check is signed for USEASM ops that unpack from
  s8/o8/c10/s16 (0x35-0x37, 0x3a, 0x3c-0x3e, 0x41, 0x43-0x45, 0x48, 0x4a-0x4c)
  and unsigned otherwise.
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

### hw 09 `test`, hw 0f `testmask`

- Any ALU USEASM op with flags1 b30 goes here; dw4 (the test spec) picks the
  condition and test vs testmask.
- opcode: 9 = test (spec b9-11 = 0), 0xf = testmask (spec b9-11 != 0).
- alu: the op the USEASM instruction names.
- cond fields: sign test = spec b0-1, zero test = spec b2-3, combine = spec b4.
- chan = spec b5-8 (0-7, 8+ rejected).
- pdst = USEASM arg1, type 0xc, p0-p3.
- testmask type = spec b9-11: 1 -> 0 `byte`, 2 -> 1 `word`, 3 -> 2 `dword`;
  others rejected. Spec 3 = USEASM_TEST_MASK_DWORD.
- mask: flags1 write mask verbatim (fdp: USEASM 0xc2 -> 7, 0xc3 -> 0xf).
- l20 dest written: clear when USEASM arg0 has flag b16.
- h18 fmt: set when a source has arg flag b22 (float group, F16) or b24 (group
  2 ops 0xf7-0xf9, C10; the dest flag counts there too).
- h21 f2_0 = USEASM flags2 b0.
- Operands: dst = arg0, src1 = arg1 (testmask) / arg2 (test), src2 = next arg.
  One-source ops (frcp ... fexp2, fadd from 0xa4) get src2 = #0; fsubflr from
  0x3 repeats src1.
- No negate/abs bits: a subtract is the `fsub` ALU op (USEASM 0xa9 = 0xa6 with
  src2's negate flag).

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
| 0x27 | 2 | 7 | `fpadd8` | 0xf8 | **guess** (TEST-only, C10-capable) |
| 0x28 | 2 | 8 | `fpsub8` | 0xf9 | corpus: every lowp compare (fl_*_l) is 0xf9 with C10 sources; name **guess** |
| 0x30-0x35, 0x37 | 3 | 0-5, 7 | `and or xor shl shr rol asr` | 0x4f 0x50 0x51 0x52 0x53 0x55 0x54 | encoder; corpus (`test.and.ne p0, #0, #0` clears p0; `test.shl.pos/neg p2, x, #31` in step_l) |

#### Conditions, and what GLSL produces

| spec / cond | sign | zero | comb | name | meaning on a-b | corpus |
|---|---|---|---|---|---|---|
| 0x19 | set | nonzero | AND | `lt` | a < b | fl_lt, ic_lt, if_lt, loop counters; `testmask` in lt, ic_ltv |
| 0x05 | set | zero | OR | `le` | a <= b | fl_le, ic_le, dc_gt (discard if a > b writes p1 = a <= b) |
| 0x14 | none | zero | AND | `eq` | a == b | fl_eq, ic_eq, if_eq, ic_eqv (testmask) |
| 0x18 | none | nonzero | AND | `ne` | a != b | fl_ne, ic_ne, dc_eq; `fadd.ne x, c48` = x != 0 (fl_ne0, cv_ftob, bool tests) |
| 0x1a | clear | nonzero | AND | `gt` | a > b | fl_gt, ic_gt, if_gt, bo_* (`greaterThan` via testmask) |
| 0x06 | clear | zero | OR | `ge` | a >= b | fl_ge, ic_ge, step (testmask) |
| 0x11 / 0x12 | set / clear | none | AND | `neg` / `pos` | sign bit | step_l (`shl` by 31 then sign test) |

- Compare pseudo-ops: USEASM 0xaa gt, 0xab ge, 0xac eq, 0xad lt, 0xae le,
  0xaf ne are rewritten to TEST fsub with spec 0x1a, 6, 0x14, 0x19, 5, 0x18
  (and arg0 flag b16, no dest); 0xb8/0xb9 to TEST fadd x, #0 with eq/ne.

### hw 0a-0e: bitwise

| hw | h3 | mnemonic | USEASM |
|---|---|---|---|
| 0a | 0 / 1 | `and` / `or` | 0x4f / 0x50 |
| 0b | 0 | `xor` | 0x51 |
| 0c | 0 / 1 | `shl` / `rol` | 0x52 / 0x55 |
| 0d | 0 / 1 | `shr` / `asr` | 0x53 / 0x54 |
| 0e | - | `rlp` | 0x56 |

- Mnemonic names follow the USEASM op order and the TEST ALU codes.
- h11 inv = USEASM arg2 flag 0x800 (src1 takes no flags).
- h2 partial = USEASM flags1 b20.
- rlp: USEASM arg2 must be a predicate (type 0xc, p0-p3) -> l14-15.

### Open questions

- f2_0 = USEASM flags2 b0; partial = USEASM flags1 b20.


Verbatim from the original chunk, under the heading each had. Field tables are kept whole: their source columns carry the USEASM inputs (opcode numbers, flags1/flags2 bits, argN, arg flag bits, INTSRCSEL values).

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

**Operand bit 6.** In sop2/sop3/sopwm/dot/ima8/fpma, USEASM flags2 b24 makes every number 6 bits. Bit 6 is then the per-operand format flag (USEASM arg flag b24, C10 vs U8). This covers r, o, pa, sa and i operands only; indexed, c/g, immediate and idx operands keep their 7-bit fields ("Encoding conventions"). The integer classes not named here (ima16, imae, hw 16, 17, 1a) never read format control.

In efo the same happens for F16 sources (arg flag b22)

All corpus sop/dot/fpma words come from programs with flags2 b24. So a backend that wants r64-r127 in these classes cannot also use per-operand C10/F16. Whether the hardware takes a global mode from somewhere (the program header?) is **unknown**.

INTSRCSEL (USEASM register type 0x10) values met here. The numbers are **encoder**. The names marked * are named in libusc asserts; the rest are **guess**.

| value | name | value | name |
|---|---|---|---|
| 0 | zero* | 1 | one (= complement of zero) |
| 2 | asat (src alpha saturate) | 3 | complement of 2 |
| 4 | src0* | 5 | src1 |
| 6 | src2 | 7 | src0alpha* |
| 8 | src1alpha | 9 | src2alpha* |
| 0xa-0xd | cx1..cx8 (dot color scale) | 0xe-0x11 | ax1..ax8 (dot alpha scale) |
| 0x12 | add* | 0x13 | sub |
| 0x14 | neg | 0x15 | none |
| 0x16 | min | 0x17 | max |
| 0x18 / 0x19 | k18 / k19 (sop2 extra c1/c2 select) | 0x1a | complement |
| 0x1e / 0x1f / 0x20 | u8 / s8 / o8 | 0x27 / 0x28 / 0x29 | u16 / s16* / u32* |
| 0x2a / 0x2b | carry in / carry out | 0x2c | 16-bit format (ima16) |

Arg flag b15 (0x8000) is the complement of a select. The encoder folds it in as select ^ complement, so ONE = complement of ZERO.

### efo (hw 07, USEASM 0x13, FUN_00116100)

The encoder ORs USEASM arg4 (an immediate) straight into the high word. usc's asm.c builds that immediate from psEfo (usc.c ~1926-2400; SGX545 takes the `bNewEfoFeature` branch). The field meanings below are asm.c's.

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

### sop2 (hw 10, USEASM 0x57 + 0xe3, FUN_0010bea0)

| bits | field | source | tag |
|---|---|---|---|
| h6-8, h24 | c1: 0 zero 1 s1 2 s2 3 s1a 4 s2a 5 asat 6 k18; h24 complement | arg4 | encoder |
| h3-5, h15 | c2: same, 6 = k19 | arg5 | encoder |
| l18-19 | cop: 0 add, 1 sub, 2 min, 3 max | arg6 | encoder |
| l20 | cs1c: complement src1 (color) | arg3 0x1a (else none 0x15) | encoder; usc bComplementCSrc1 |
| h20-21, h11 | a1: zero s1a s2a k18 + complement | 0xe3 arg1 | encoder |
| h9-10, h2 | a2: zero s1a s2a k19 + complement | 0xe3 arg2 | encoder |
| l16-17 | aop: 0 add, 1 sub, 2 min, 3 max | 0xe3 arg3 | encoder |
| l15 | as1c: complement src1 (alpha) | 0xe3 arg0 0x1a | encoder; usc bComplementASrc1 |
| l14 | aneg: negate alpha result | 0xe3 arg4 0x14 | encoder |

### sopwm (hw 12, USEASM 0x58 / 0xfa, FUN_0010c9e0)

`dst = (src1 * c1) cop (src2 * c2)` per byte, with a write mask. There is no coissue and no repeat; flags1 mask must be 0x200.

| bits | field | source | tag |
|---|---|---|---|
| h12,h13,h14,h11 | mask, in USEASM order x y z w (stored rotated); default 15 | dest arg flags b7-10 with flag b20; a partial mask needs a non-indexed dest | encoder; usc puts auDestMask << 7 there |
| h6-8, h24 | c1: 0 zero 1 asat 2 s1 3 s1a 6 s2 7 s2a (4, 5 unused) + complement | arg3 | encoder |
| h3-5, h15 | c2: same | arg4 | encoder |
| h20-21 | cop: 0 add, 1 sub, 2 min, 3 max | arg5 | encoder |
| h9-10 | aop: same values | arg6 | encoder |

h2 and l14-20 are unused. Corpus: lowp add/sub/min/max (`sopwm d, a, b [c1=one c2=one cop=sub aop=sub]`) and lowp mix (0xfa).

### sop3 family (hw 11)

**sop3** (h21=0, h10=0; USEASM 0x59 + 0xde asop / 0xdf arsop):

| bits | field | tag |
|---|---|---|
| h20 | cop: 0 add, 1 sub (arg6 0x12/0x13) | encoder |
| h12-13, h14 | a: alpha select zero s0a s1a s2a + complement (0xde/0xdf arg0) | encoder |
| h9 | aop: 0 add, 1 sub (0xde/0xdf arg1) | encoder |
| h11 | arev: 0xdf instead of 0xde | encoder; usc uses 0xdf when bNegateAResult. In asop the other alpha select is c2's alpha; arsop swaps the roles (b - a) |

**sop3l** (h21=0, h10=1; 0x59 + 0xe0 alrp):

| bits | field | tag |
|---|---|---|
| h20 | cop | encoder |
| h13, h9 | a1: zero / s1a + complement | encoder |
| h12, h14 | a2: zero / s2a + complement | encoder |
| h11 | cneg: color result negate (arg7 0x14); only allowed with alrp | encoder |

**lrp1** (h21=1, h10=0; USEASM 0xe1 + 0xde):

| bits | field | tag |
|---|---|---|
| h13, h20 | l1: zero / s1 + complement | encoder |
| h12, h14 | l2: zero / s2 + complement | encoder |
| h11 | cs: s0 / s0a | encoder; usc assert `psLrp1->uCS == SRC0ALPHA` |
| c1/c2 fields | the coissued asop's two selects, with the family's c1/c2 select values above | encoder |
| h9 | aop | encoder |

Color = lerp(l1, l2, cs) is a **guess** from the name.

**sop3x** (h21=1, h10=1; USEASM 0xe2 + 0xe0): c1 and c2 as above, plus the alrp alpha (a1, a2 as in sop3l).

h11 and h20 (field p, h11 the low bit) encode the pair arg6/arg7: 0 `s1.s2`, 1 `s1.1-s2`, 2 `one.s2`, 3 `1-s1.s2`. The pair 1-s1, 1-s2 is rejected (**encoder**). The name and the meaning are **guess**; usc's asm.c never emits 0xe2 explicitly.

### dot3 / dot4 (hw 18, USEASM 0x5a-0x61, FUN_0010e460)

| bits | field | source | tag |
|---|---|---|---|
| h24 | 4-vector (dot4) | 0x5b 0x5d 0x5f 0x61 | encoder; usc uVecLength 3/4 |
| h6 | f6 | 0x5e-0x61 | encoder; meaning unknown |
| h7-8 | cs: color scale x1 x2 x4 x8 | arg1 INTSRCSEL 0xa-0xd | encoder |
| h11 / h15 | src1 / src2 complement | arg3/arg4 flag b15 | encoder |
| h5 | src0 present: coissue 0xda/0xdb (forms dot3s/dot4s) | | encoder |
| h4 | f4: 0xdb or 0xdd rather than 0xda/0xdc | | encoder |
| h20-21 (+h2 when h5=0) | a1: zero s1a s2a, 3 = x3 (+ complement) | coissue arg1; 0x5c 0x5d 0x60 0x61 force 3 | encoder |
| h9-10, h3 | a2: zero s1a s2a, 3 = x3 + complement | coissue arg2 (0xda/0xdb) or arg3 (0xdc/0xdd); without a coissue libusc writes 3, but the field holds any value | encoder |
| l16-17 (h5=0) | as: alpha scale | arg2 0xe-0x11 (no coissue) or 0xdc/0xdd arg0 | encoder |
| l15 (h5=0) | s1ac: complement of 0xdc/0xdd's fixed src1alpha (arg2) | | encoder |
| l14-20 (h5=1) | src0 (r/pa) | 0xda/0xdb arg0 | encoder |

0x5b and 0x5f reject a coissue. Corpus: dp3_l/dp4_l/norm3_l use `dot3/dot4 d, a, b [a1=zero a2=x3]`, the lowp dot product with no alpha op.

### ima8 (hw 13, USEASM 0x62) / fpma (hw 19, 0xf4), FUN_0010f1a0

| bits | field | source | tag |
|---|---|---|---|
| h20-21, h10 | c0: s0 s1 s0a s1a + complement | arg4 | encoder |
| h5, h24 | c1: s1 / s1a + complement | arg5 | encoder |
| h4, h15 | c2: s2 / s2a + complement | arg6 | encoder |
| h9, h6 | a0: s0a / s1a + complement | arg7 | encoder |
| h7 / h8 | a1c / a2c: complement s1a / s2a (arg8 must be s1a, arg9 s2a) | | encoder |
| h11 | sat | flags2 b10 | encoder; name **guess** (usc psFpma field after bNegateSource0) |

### ima16 (hw 14, USEASM 0x63; 0xe4-0xe9), FUN_0010f960

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

src1 immediates are signed when sgn is set. src2 immediates are signed whenever `t2` isn't 0: s16, u32 and the unnamed 3 (`t2=x3`) (usc sign-extends 16-bit constants for s16/u32: usc.c ~69289). So `imae o72, pa36, r58, #-63 [t2=u32]` is legal, and the 7-bit number holds -63 in two's complement. Corpus: dynamic array addressing, `imae.sgn pa0, pa1, #16, sa9 [t2=u32]`.

### hw 16 / 17 / 1a (no corpus words; encoder or encoder (patched))

**adif / adifsum** (16, h20-21=0, FUN_00110800, USEASM 0x67/0x68):
- h6 = 1 makes it adifsum, with src0 (r/pa). adif has no src0 (l14-20 and h2 are 0).
- h15 = s0f13 (src0 arg flag b13, unknown).
- Probably |src1 - src2| (+ src0) (**guess**).

Operand slots: the generic dst, src0 (bank h2, no extension), src1 and src2 unless an entry says otherwise. `fmt` (h8-9) in bilin, ssum16, firv and firh: 0 u8, 1 s8, 2 o8 (ssum16i is the separate form with h8-9 = 3). It is written as a mnemonic suffix, `bilin.s8`, `ssum16.o8`, `firh.s8`; u8 has none. `[fmt=s8]` in the brackets means the same. Fields given as USEASM values (op16s1's `x39`) print as the stored field value.

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
- nf16 h15: set unless dest flag b16 (the text prints `nf16` when the bit is set).

**ssum16i** (h8-9 = 3, 0x6a, FUN_001192a0, patched): src0 is an internal register, its number written raw in l14-20 with no bank bits; there is no src1 (l7-13, l30-31 are 0).

**firv** (17, h2=0, 0x6c, FUN_00111850):
- src0 is read in src1's bank: its number in l14-20 with no bank bits (as ssum16).
- fmt h8-9.
- a5 h6-7 (immediate 0-2).
- coef h3-5 (register type 0x11, 0-7).
- shift: 5-bit two's complement, bits 0-1 in h10-11, 2-3 in h14-15, 4 in h24 (printed raw 0-31).
- a8 h20-21 (0-3).
- The destination is the generic dst slot (g<n> as everywhere: number + 0x40 on ext bank 1).

**firh** (h2=1, 0x6d inline, patched): repeat count - 1 in h12-14. The dest is an internal register, its number written raw in l21-27 with no bank bits. Shift bits 2-3 move to h0-1 (bits 0-1 stay in h10-11, bit 4 in h24). h19 = flags2 b27.

**ima32** (1a, h20=0, 0x65, FUN_00118650): `dst = src0*src1 + src2`, 32-bit.
- sgn h9 (flags2 0x1000 vs 0x800; it also makes immediates signed).
- hi h4-6: the high 32 bits go to i0-i2 (arg1; usc psHighWordDest): 0 nowhere, 4 i0, 5 i1, 6 i2.
- cin h10-11: carry-in from i0-i2 (arg5 0x2a + arg6): 0 none, 1 i0, 2 i1, 3 i2.
- negp h8 = src0 neg xor src1 neg.
- src2 neg h7.
- src0 r/pa/o/sa via ext h15.
- USEASM 0x167/0x168 are rewritten to `a*#1 + -b`: isub32 signed/unsigned.

**op1adrc** (1a, h20=1, 0x66 inline, patched):
- dest r/pa (h7), raw number.
- drc h0 (register type 10, DRC? 0-1).
- Sources src1, src2. The operation is unknown; the DRC operand suggests a long-latency op.

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

- **INTSRCSEL names**: 2 (asat), 0x18/0x19 (k18/k19), u8/s8/o8, and 0x2c are not in the binary.
- **sop3x** (USEASM 0xe2): its operation.
- **dot**:
  - f6 (h6, 0x5e-0x61), f4 (0xdb/0xdd vs 0xda/0xdc) and a1/a2 = 3 are unknown.
  - The meaning of 0x5c/0x5d/0x60/0x61, which have no alpha op.
- **sat** (flags2 b10) in ima8/fpma/ima16/imae is a name guess.
- ima16/imae **f2_15** (flags2 b15-18) and ima16 **dflag1** (dest flag b1) are unknown.
- **hw 16/17/1a**: INTSRCSEL 0x1b-0x1d and 0x38-0x3b, register types 0x11 and 10, the adifsum and ssum16 h15 flags, firv a5/a8, and the operations of op16s1 and op1adrc.


Everything below is copied verbatim from the original chunk. Field tables are
copied whole so every USEASM clause removed from a cell (fN bK, argument flag K,
USEASM opcode numbers, register types, int-source-select values) is kept.

## Target feature bits (smp, ld/st, and the special class)

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

### libusc internal opcodes (from the USEASM opcode -> form section)

libusc's internal opcodes (from its instruction descriptor table at Ghidra
0x159960, field +0x30 = USEASM opcode): 0x22-0x2f loads/stores (byte group
0x22-0x25, word 0x26-0x29, dword 0x2a-0x2f), 0x30/0x31 eld, 0x32 idf, 0x33
wdf, 0x6b IBR and 0x6c ICALL (both 0x96; asm.c adds f1 b19 for the call),
0x6d ILABEL (0xa8), 0x6e ILAPC (0x97; asm.c asserts `eOpcode == ILAPC`
around return labels), 0x71 and 0x76 (0xf1: INOP and IDRVPADDING), 0x72
ISMLSI (0xbe), 0x73 (0xbd), 0x74 (0xc1), 0x75 (0xbf), 0x04 (0x9b), 0xa7
(0xd6), 0xaf (0x101: created only by the geometry-shader path, behind "Geometry
shaders aren't supported on this core"), 0xb7/0xb8 (0xcd/0xce).

## hw 1c: smp (FUN_0010a3f0)

`smp.<dim>[.rdata|.rinfo][.pcf|.pcf16][.f16|.c10][.bias|.lod|.grad] dst, coord, state[, lod]  [drc=n ...]`

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
| h12-15 | mask, or rpt - 1 when h21 is set | write mask (text `mask=`, default 1); SGX545's encoder accepts only mask 1 and no repeat, so libusc always has h12 set, but the field holds any value. `ret` below is ORed into bits 14-15 of the same field | encoder |
| h14 | ret = 1: `.rdata` | f3 b3. asm.c sets it for the sample's mode field = 1 | encoder; name guess (raw sample data) |
| h15 | ret = 2: `.rinfo` | f3 b2 (mode 2). Mode 3 (f3 0x8000, "both") is rejected on SGX545; h14+h15 together is rejected | encoder; name guess (sample info) |
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

## hw 1d: ld (FUN_0010b190), h22 = 0

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
| h21 | moe | set unless f1 b31 (so libusc's loads nearly always have it): a repeated load steps its operands by the MOE increments (same bit as the ALU classes). The text prints `moe` when it is set | encoder; meaning by analogy (guess) |
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
| h4-5 | size | 0 d (dword: 0x7d...), 1 w (0x7c...), 2 b (0x7b...), 3 `q` (0x7e...: libusc's encoder rejects it, but the field holds it; the text is `ld.<mode>.q` and it encodes as 3) | encoder, corpus (d); b/w from libusc's internal opcode order (byte, word, dword groups) |
| h3 | f2_9 | f2 b9 | encoder; unknown |
| h1 | f2_2 | f2 b2 | encoder; data-cache control (guess) |
| h0 | drc | DRC the loaded data signals | encoder, corpus (ix_tmp*: loads alternate drc 0/1, each followed later by `wdf`) |
| l21-27 | dst | | encoder |
| l14-20 | base | | encoder |
| l7-13, l30-31 | off | any source kind, including #0-127 and indexed | encoder, corpus |
| l0-6, l28-29 | range | without h6: the unused pattern | encoder |

## hw 1d: eld (FUN_0010ace0), h22 = 1

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

## hw 1e: st (FUN_0010b190)

`st.<a|l|t>.<b|w|d> base, off, data  [...]` (arguments: base, offset, data).
Same layout as ld, with these differences: no destination (l21-27 and h7
are 0), no DRC (h0 = f3_13, f3 b13, target feature word1 & 0x400; unknown),
h22 = nosched (allowed on stores), the data is the src2 slot (any source
kind, including immediates). A store has no range operand: with the range
flag (h6) set, src2 still holds the data. Corpus: ix_tmpw (`st.l.d sa1, pa1, sa9`,
dynamic array writes; `st.l.d sa1, sa13, pa0 [rpt=16]`) and ix_comp.

## hw 1f: flow control (h20-22 = 0)

| form | fixed | fields | evidence |
|---|---|---|---|
| `ba #target` | h6-8 = 0, h19 = 0 | target l0-17: label address + the program's load offset (encode()'s `offset`), unsigned 18 bits (feature +0x20) | encoder; **hardware** (hwt_ba): the target is the absolute instruction index from the USE code base the task runs from (DOUTU's base 0 = 0x37000000): `ba #0x1077` reached the instruction at 0x37008388 + 6*8 |
| `br #target` | h6-8 = 1, h19 = 0 | target l0-17: label + label-argument offset - this instruction's index, signed 18 bits (-0x20000..0x1ffff) | encoder, corpus; **hardware** (hwt_p2 `br_1..3`, `loop_br`, `br_pred_*`): `br #n` goes to this instruction + n; `#-2` loops back; a predicated br falls through when its predicate is false |
| `lapc` | h6-8 = 2 | return to the saved link address | encoder, corpus (fn_call); **hardware** (hwt_p7 `call_lapc`): `br #3 [link]` ... `lapc` returns to the instruction after the br |
| `nop` | h6-8 = 5 | h23 syncs (f1 b2), h18 end (f1 b17), l0 = f2 b26, l1 = f3 b5; no predicate, h12 and h24-26 are 0 | encoder, corpus (secondary-program prologue `nop [nosched]`, discard placeholder) |
| `setl src` | h6-8 = 3 | set the link address from a register: src in the src1 slot (l7-13, bank l30-31, extension h17) | `static` (the DDK microkernel's code: a call through a table is `setl` + `lapc`, a nested call `savl rN` ... `br [link]` ... `setl rN`); `capture` (tools/ukernel's emulator runs a captured frame through the microkernel with this reading and matches the DDK's register and memory writes). libusc has no USEASM op for it, so there is no oracle |
| `savl dst` | h6-8 = 4 | save the link address to a register: dst in the destination slot's number and bank (l21-27, h0-1), no extension bit | as `setl` |
| `op0x98` | h6-8 = 0, h9 = 1, h19 = 1 | no arguments; needs word0 & 0x200000 | encoder; meaning unknown |

Fields of ba/br/lapc/setl/savl: h24-26 pred (PRED3; pn rejected), h23 syncend (f1 b18;
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

## hw 1f: MOE control (h20-22 = 1)

h18 = nosched for the whole group; no predicate. h19 = register form for ops
0-3 (the values come from src1/src2 instead of immediates; word0 & 4),
printed `smoa.r`, `smr.r`, `smlsi.r`, `smbo.r`: two operands, the generic
src1 and src2 slots with their banks and extension bits, and nothing else in
the word.

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `smoa v0, v1, v2, v3 [am0..am3]` (0xbc) | four signed 10-bit values: v0 l30-31 + h0-7, v1 l20-29, v2 l10-19, v3 l0-9; four 2-bit modes (USEASM register type 0xe): am0 h14-15, am1 h12-13, am2 h10-11, am3 h8-9 | encoder; name and meaning guess (MOE operand addressing) |
| 1 | `smr v0..v3` (0xbd) | four 12-bit: v0 h4-15, v1 l24-31 + h0-3, v2 l12-23, v3 l0-11 | encoder; name guess |
| 2 | `smlsi v0..v3 [lim0 lim1 lim2]` (0xbe) | per operand (dest, src0, src1, src2) an 8-bit value: v0 l24-31, v1 l16-23, v2 l8-15, v3 l0-7; flag h3/h2/h1/h0 set = the value is a swizzle (four 2-bit selects, printed `#0.xyzw`), clear = a signed increment; lim0 h12-15, lim1 h8-11, lim2 h4-7 = USEASM arguments 8-10 / 4 (multiples of 4, up to 60) | encoder, corpus (every swizzled operation is bracketed by `smlsi`; libusc's ISMLSI); limits' meaning guess (temp / pa / sa wrap limits) |
| 3 | `smbo v0..v3` (0xbf) | as op 1 | encoder; MOE base offsets (the USP code block keeps 4 MOE base offsets) — guess |
| 4 | `imo v0..v3` (0xc0) | as op 1, signed (the encoder takes -32..31); no register form | encoder; name guess (increment MOE offsets) |
| 5 | `setfc #a0, #a1` (0xc1) | a0 l0, a1 l8 (USEASM arguments must be #0/#1) | encoder, corpus (`setfc #1, #1` in tx_dep_loop, cos_l, sin_l). **hardware** (hwt_p6): **a1 (l8) is the color-class C10 enable** and is **on at program start**: `sop2 r3, r0.c10, r1.c10` reads C10 with no setfc, after `setfc #1, #1` and after `setfc #0, #1`; after `setfc #0, #0` or `#1, #0` the same word reads r64/r65 (garbage, different per pixel, no fault). **a0 (l0) is the efo F16 enable** and is **off at program start** (hwt_p8: an efo with operand bit 6 reads r64 with no setfc or after `setfc #0, #1`, and the F16 half after `setfc #1, #1`) |
| 6 | `op0xfd dst, #n` (0xfd) | dst l21-27 + h0-1 + h19 (destination slot), n h11-14 (0..8), h23 skipinv | encoder; meaning unknown |

## hw 1f: other (h20-21 = 2)

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `idf [drc=d path=p]` (0x99) | h0 DRC, h14 second argument (#0/#1) | encoder; idf = issue data fence (guess) |
| 1 | `wdf [drc=d]` (0x9a) | h0 DRC; waits until the loads/samples that signal that DRC have written their results | encoder, corpus (after every uniform load and before the first use of a loaded or sampled register) |
| 2 | `op0xd7 #n` (0xd7) | n l0-3 (below feature +0x10 = 16) | encoder; unknown |
| 3 | `emit.<t0..t3> src0, src1, src2 [n f10 f12 f13 f3_20 imm end]` | tgt h14-15 (0x9c/0x9d 0, 0x101/0x102 1, 0xa2 2, 0x103/0x104 3); f12 h12 (0x101, 0x103; 0xa0 on older cores; f2 b5 on 0xa2); f13 h13 (0x101 + f3 b8: libusc's geometry "emit and cut" sets f3 0x100; 0xa1 on older cores; f2 b4 on 0xa2); f10 h10 (f3 b6 on 0x101/0x103, f3 b7 on 0x102/0x104); l21 = f3 b20; n = first argument (h0-1 + h9, 0..5); h18 end; h11 nosched. Sources: 0x101/0x102 src1 + src2; 0x103/0x104 src2 only; 0x9c-0x9e/0xa2 src0 (extension h19) + src1 (+ src2). Unused sources hold #0. 0x9d/0x9e/0xa1/0xa2 add a 14-bit immediate (bits 0-5 in l22-27, 6-11 in h3-8, 12-13 in h22-23); 0x9c sets h8 (= immediate 0x800). In the word all three source slots exist whatever the USEASM op (src0: l14-20, bank h2, extension h19; src1, src2 generic), the text always shows all three, and `imm=` and `n=` are the raw fields (n: bits 0-1 in h0-1, bit 2 in h9) | encoder; 0x101 is libusc's geometry-shader vertex emit; target names unknown |
| 4 | `limm dst, #imm32` (0x9b) | dst = destination slot (l21-27, h0-1, h19); imm l0-20 (bits 0-20), h4-8 (21-25), h12-17 (26-31); a label argument (type 0xb) gives its absolute address (+ offset); argument flag 11 inverts the value; h9-11 pred (PRED3); h23 skipinv; h22 nosched; h18 end | encoder, corpus (`limm r0, #4096` before indexed stores); **hardware** (every hwt test loads its inputs with it: 0x12345678, 0xdeadbeef exact) |
| 5 | `lock #n` / `release #n` (0xcd / 0xce) | l0 = 0 / 1; n l4-6 (below feature +0xc = 8) | encoder; names guess (mutexes) |
| 6 | `ldr dst, addr [drc=d]` (0xd0), `str addr, data` (0xd1) | h19 = str; dst l21-27 + h7 (r/pa); addr = src2 slot whose immediate form has 14 bits (low 7 l0-6, high 7 l14-20, and the slot's immediate bank: l29 + h16); data = src1 slot; h0 DRC (ldr); h23 skipinv; h11 nosched | encoder; names guess |
| 7 | `op0xd6 #n` (0xd6) | n l0-2 (0..5), l3 = f3 b6 (libusc sets it under the same compile flag as emit's f3 b6) | encoder; unknown |

h11 = nosched for all of them except limm (h22).

## hw 1f: visibility test (h20-21 = 3)

Sources: src0 (l14-20, bank h2, extension h19), src1, src2. The USEASM
int-source-select arguments: 0x34 sets h5 ("sel34"), 0x36 sets h3, 0x37 sets
h4; 0x15 and 0x35 mean none. h18 = end, h11 = nosched, h23 = syncs (atst8,
depthf).

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `pcoeff src0, src1, src2` (0xd2), `ptoff` (0xd3: h15, no sources: every source field is 0) | | encoder; names guess (plane coefficients / punch-through off) |
| 1 | `atst8 dst, pdst, src0, src1, src2` (0xd4) | dst l21-27 + h0-1 without extension (r o pa); pdst = predicate p0-p3 in h7-8, h6 = write it (cleared by the argument's flag 16, "disable write-back": printed `p1.nw`); h22 = f2 b25; h3/h4/h5 sel. The GLES driver's own atst8 (`f9340406`) also sets h10, which libusc never does: h9-10 is a PRED2 predicate as in depthf (2 = p1); a false predicate kills the pixel (driver, hardware: "Phases, visibility feedback and discard") | encoder; name guess |
| 3 | `depthf src0, src1, src2` (0xd5) | h9-10 pred (PRED2); h4/h5 sel (0x36 rejected) | encoder; name guess |

## hw 1f: phase / 0xfe-0x100 (h20-22 = 4, h23 = 0)

| op | form | layout | evidence |
|---|---|---|---|
| 2 | `phas src1, src2` (0xd8, h18 = 0) | h19 end, h11 nosched | encoder; name guess |
| 2 | `phas.i #pc, #temps4 [rate wait sel43 end]` (0xd9, h18 = 1) | pc l0-17 (18 bits, absolute, no load offset added); temps4 h0-7 = USEASM argument / 4 (multiple of 4, up to 0x3fc); rate h11-12: int-source-select 0x3c 0, 0x3e 1, 0x3d 2; wait h8-10: none (type 0xe #0) 0, 0x3f 1, 0x40 2, 0x41 7; sel43 h13 (0x42 0, 0x43 1); h19 end | encoder; reading as "next phase address, temporaries, execution rate, wait condition" is a guess (libusc talks about feedback/split phases: uMainProgFeedbackPhase1Start) |
| 0 | `op0xff` / `op0xfe` / `op0x100 src1, src2, #n` | h12-13 = 1 / 2 / 3; n h8-9 (0..3); h15 = f3 b11, h14 = f3 b12; h11 nosched | encoder; unknown |

## Open questions

- br/ba h10 (f1 b22) and USEASM 0xd7 share target feature +0x10 (16).


## USP texture-sample blocks and the driver's sample finalizer

### Shared sub-records

From the "Register types" bullet:

> 4-valued fields 1 = r, 2 = o, 3 = pa, 4 = i (the USEASM register types 0/1/2/7 map to them).

### Block 9: TEXTURE WRITE (82 bytes; was 78 in usp.py)

Operand format code mapping:

> (format code: USEASM format 0→1, 2→2, 3→3,
> 6→4, 7→5; anything else is an error)

### The smp instruction, as the vendor compiler writes it

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
| drc | h0 (0 or 1; h1 is never set) | the arg after the last source, type 0xa |
| (check only) | next arg must be type 0x10, number 0x15 | |

