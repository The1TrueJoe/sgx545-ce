# smp, ld/st, and the special class (hw 1c, 1d, 1e, 1f)

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

## USEASM opcode -> form

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

## hw 1c: smp (FUN_0010a3f0)

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
kind, including immediates). Corpus: ix_tmpw (`st.l.d sa1, pa1, sa9`,
dynamic array writes; `st.l.d sa1, sa13, pa0 [rpt=16]`) and ix_comp.

---------------------------------------------------------------------------

## hw 1f: the special class

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

### flow control (h20-22 = 0)

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

### MOE control (h20-22 = 1)

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

### other (h20-21 = 2)

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

### visibility test (h20-21 = 3)

Sources: src0 (l14-20, bank h2, extension h19), src1, src2. The USEASM
int-source-select arguments: 0x34 sets h5 ("sel34"), 0x36 sets h3, 0x37 sets
h4; 0x15 and 0x35 mean none. h18 = end, h11 = nosched, h23 = syncs (atst8,
depthf).

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `pcoeff src0, src1, src2` (0xd2), `ptoff` (0xd3: h15, no sources) | | encoder; names guess (plane coefficients / punch-through off) |
| 1 | `atst8 dst, pdst, src0, src1, src2` (0xd4) | dst l21-27 + h0-1 without extension (r o pa); pdst = predicate p0-p3 in h7-8, h6 = write it (cleared by the argument's flag 16, "disable write-back": printed `p1.nw`); h22 = f2 b25; h3/h4/h5 sel | encoder; name guess |
| 3 | `depthf src0, src1, src2` (0xd5) | h9-10 pred (PRED2); h4/h5 sel (0x36 rejected) | encoder; name guess |

### phase / 0xfe-0x100 (h20-22 = 4, h23 = 0)

| op | form | layout | evidence |
|---|---|---|---|
| 2 | `phas src1, src2` (0xd8, h18 = 0) | h19 end, h11 nosched | encoder; name guess |
| 2 | `phas.i #pc, #temps4 [rate wait sel43 end]` (0xd9, h18 = 1) | pc l0-17 (18 bits, absolute, no load offset added); temps4 h0-7 = USEASM argument / 4 (multiple of 4, up to 0x3fc); rate h11-12: int-source-select 0x3c 0, 0x3e 1, 0x3d 2; wait h8-10: none (type 0xe #0) 0, 0x3f 1, 0x40 2, 0x41 7; sel43 h13 (0x42 0, 0x43 1); h19 end | encoder; reading as "next phase address, temporaries, execution rate, wait condition" is a guess (libusc talks about feedback/split phases: uMainProgFeedbackPhase1Start) |
| 0 | `op0xff` / `op0xfe` / `op0x100 src1, src2, #n` | h12-13 = 1 / 2 / 3; n h8-9 (0..3); h15 = f3 b11, h14 = f3 b12; h11 nosched | encoder; unknown |

---------------------------------------------------------------------------

## How the corpus builds control flow (c3 lp_*, fl_*, dc_*, fn_call)

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
  code block holding only `nop` between two labels. The kill itself is not
  in the USP binary; the patcher replaces that slot (the visibility-test
  instructions above are the likely candidates; guess).
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

## Open questions

- Names/meanings marked guess, in particular: ld/st address modes (a/l/t),
  pre/post increment, range (h6), f2_2/f2_3/f2_9 (cache control?), st h0
  (f3 b13); smp h14/h15 (rdata/rinfo); the MOE ops other than smlsi/smbo;
  op0x98, op0xd6, op0xd7, op0xfd, op0xfe-0x100; emit targets; the
  visibility-test names and their int-source-select options; phas fields.
- br/ba h10 (f1 b22) and USEASM 0xd7 share target feature +0x10 (16).
- smp's coordinate count (how many registers the coordinate spans) is not in
  the word; it follows from the dimension and the projective/PCF modes.
- Where the discard is finally emitted (patcher, not in USP code).
