# Integer, fixed-point and multi-unit ops: hw 07, 10-19, 1a

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

## USEASM opcode -> form

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

## Fields shared by hw 10-19 and 1a (encoder)

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

## efo (hw 07, USEASM 0x13, FUN_00116100)

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

## sop2 (hw 10, USEASM 0x57 + 0xe3, FUN_0010bea0)

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

## sopwm (hw 12, USEASM 0x58 / 0xfa, FUN_0010c9e0)

`dst = (src1 * c1) cop (src2 * c2)` per byte, with a write mask. There is no coissue and no repeat; flags1 mask must be 0x200.

| bits | field | source | tag |
|---|---|---|---|
| h12,h13,h14,h11 | mask, in USEASM order x y z w (stored rotated); default 15 | dest arg flags b7-10 with flag b20; a partial mask needs a non-indexed dest | encoder; usc puts auDestMask << 7 there |
| h6-8, h24 | c1: 0 zero 1 asat 2 s1 3 s1a 6 s2 7 s2a (4, 5 unused) + complement | arg3 | encoder |
| h3-5, h15 | c2: same | arg4 | encoder |
| h20-21 | cop add sub min max | arg5 | encoder |
| h9-10 | aop | arg6 | encoder |

h2 and l14-20 are unused. Corpus: lowp add/sub/min/max (`sopwm d, a, b [c1=one c2=one cop=sub aop=sub]`) and lowp mix (0xfa).

## sop3 family (hw 11)

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

## dot3 / dot4 (hw 18, USEASM 0x5a-0x61, FUN_0010e460)

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

## ima8 (hw 13, USEASM 0x62) / fpma (hw 19, 0xf4), FUN_0010f1a0

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

## ima16 (hw 14, USEASM 0x63; 0xe4-0xe9), FUN_0010f960

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

## imae (hw 15, USEASM 0x64; 0xea 0xf0), FUN_00110060

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

## hw 16 / 17 / 1a (no corpus words; encoder or encoder (patched))

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

## What GLSL integer code compiles to (c3 corpus, ef_c3.txt)

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

### What a backend would use beyond GLSL ES 1.0

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

## Open questions

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
