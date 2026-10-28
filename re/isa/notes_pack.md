# Pack/unpack, test, testmask, bitwise (hw 08, 09, 0a-0e, 0f)

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

## Shared ALU control bits (all three encoders)

| bits | field | USEASM source | tag |
|---|---|---|---|
| h24-26 | pred | flags1 b13-16 | encoder |
| h23 | skipinv | flags1 b0 | encoder |
| h22 | nosched | flags1 b3 (h22 here, not h11 as in the float classes) | encoder |
| h20 | syncs | flags1 b2 | encoder |
| h18 | end (pck, bitwise only) | flags1 b17 (test rejects it) | encoder |
| h21 | rcnt (pck, bitwise) | set when flags1 repeat count n>0 | encoder |
| h12-15 | rpt = n (stored n-1) when rcnt; else mask = flags1 b9-12 verbatim | flags1 b4-8 / b9-12 | encoder; "mask" = per-iteration enable mask is a **guess** (compiler always writes 1) |

## hw 08: `pck.<dst>.<src>` (FUN_001165c0)

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

## hw 09 `test`, hw 0f `testmask` (FUN_00112770)

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

### ALU op field (l14-19)

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

### Conditions, and what GLSL produces

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

### test vs testmask

`test` writes predicate pdst from one channel (chan) of the ALU result, or
with mask > 1 one predicate per channel starting at p0 (the encoder forces
pdst = p0 then; guess that channel n writes pn). The ALU result itself is
also written when there is a dest (l20).

`testmask` has no predicate: it writes into dst, per enabled channel (mask), a
mask of the given width that is all ones when the test passes and zero
otherwise (corpus: every testmask is followed by `movc.nz.i32 d, d, c52, c48`
= select 1.0/0.0, e.g. step, lt, bo_xor, ic_eqv, and vo_if's `movc.z.i32`
select). The all-ones/zero reading is a **guess** consistent with movc.nz.

## hw 0a-0e: bitwise (FUN_00117270)

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

## Open questions

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
