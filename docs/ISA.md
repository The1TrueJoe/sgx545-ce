# SGX545 USSE instruction set (Cedarview DDK 1.7)

The 64-bit USSE instructions of the SGX545, as the DDK 1.7 shader compiler emits them
and the hardware runs them. `dis.py` disassembles USP shader binaries, and `asm.py`
assembles the same text. Mesa's encoder and disassembler (`mesa/src/sgx/isa/sgx.xml`)
are written from this page.

## How each fact is established

Every field below carries one of these tags:

- **oracle**: a field's position, its values, whether a form is accepted, and what an
  input encodes to. These come from encoding through the vendor's shader assembler as
  a black box and decoding the words. The `check_<area>.py` sweeps encode every field
  value of every form (about 20,000 outputs) and require each word to decode with no
  unexplained bit and to re-encode from its text. The same encoding is bit-exact
  against 5045 instructions logged from real compiles on a CE5300 board. A field's position
  and values are therefore facts. Its meaning is a fact only where corpus or hardware
  evidence backs it.
- **corpus**: what the DDK's GLSL compiler emits for a one-feature shader (`gen.py`,
  `gen2.py`, `gen3.py`; corpora `corpus/c1..c3`), or what toggling that feature
  changes.
- **hardware**: run on the CE5300 board. Most of these come from `hwt.py`: a hand-assembled
  fragment program moves its result to `o0`, which the PBE stores unchanged, so one
  pixel of the back buffer is the 32-bit register. Each hardware tag names the test
  (`hwt_<suite>.py` / test name; logs in `hwt_out/`). See "Hardware checks" below.
  Where a form is in `mesa/src/sgx/isa/tests/gles2.txt`, the open compiler emits it
  and passes dEQP-GLES2 on the CE5300 board with it; that exercises the form, though not the
  field values the compiler leaves at their defaults.
- **capture**: seen in the vendor driver's output: its USP binaries (`usp.py` parses
  every corpus binary), its finalized programs and state in GPU memory, its traces.
- **unconfirmed**: stated but not yet backed by any of the above. Each one is listed
  in "Gaps" at the end with the test that would confirm it. Shipping code must not
  rely on an unconfirmed fact.
- **guess**: a name or meaning that fits but has no other backing.

In the terms of docs/CLEANROOM.md: **oracle** and **corpus** are `oracle`, **hardware**
is `hardware`, and **capture** is `capture`.

What an encoder needs from this page, and where it is:
- The 64-bit word, bit names and instruction order: below.
- Which class a word is: "Instruction classes" (h27-31) and, inside each class, the
  sub-op bits named in its section.
- Register banks, extension bits, number widths, indexing, constants: "Registers" and
  "Operands". Slots that differ are listed in their class.
- Per-class fields: each class's table. A field written `h6-8, h24` or `h13 + h18` is
  split: the first range holds the low bits.
- Modal state that the word alone doesn't carry:
  - format select (h22 in the float classes, per instruction);
  - format control (`setfc`, per program; "MOE control");
  - repeats and increments ("Float classes", "Fields shared by hw 10-19");
  - dual issue ("Dual issue").
- Bank codes, number fields, unused slots and the text notation's defaults: "Encoding
  conventions" below.

Bit names: `l<n>` is bit n of the low word, `h<n>` of the high word. An instruction is
stored low word first.

## Tools

| file | what it does |
|---|---|
| `hw.py`, `hw_*.py` | the instruction forms (framework + one module per group of classes); `decode`, `encode`, `fmt`, `parse` |
| `dis.py` | disassemble USP binaries (`python3 dis.py corpus/c3`) or hex words (`-x hi lo`) |
| `asm.py` | assemble dis.py's text; `--check` runs `hwcheck.py` |
| `hwt.py`, `hwt_lib.py`, `hwt_*.py`, `hwt_out/` | run hand-assembled fragment programs on the CE5300 board through the replay and read the result register back from the back buffer; one suite per area, logs per suite |
| `hwcheck.py` | every corpus word: has a form, no unexplained bit, text round-trips |
| `check_float.py`, `check_pack.py`, `check_int.py`, `check_mem.py` | oracle sweeps per area |
| `usp.py`, `smpblock.py` | the USP container and its texture-sample blocks |
| `isadoc_cov.py` | does this page state every form's identifying bits, field positions, operand slots and enum values (necessary, not sufficient) |
| `blind.py` | random valid instructions per form, and the score of an encoder written from this page alone against them (docs/CLEANROOM.md "Validation"). Two blind passes on 2026-11-01, by fresh sessions that read only `docs/`: 120/122 words exact before the "Encoding conventions" section and the fixes it prompted (41% marked sure), 120/120 after (88% sure) |
| `gen3.py` | the texture / flow / int / index / vertex corpus (`corpus/c3`) |

Oracle input formats for the sweeps are in `tools/isa/oracle-map.md`.

Status: all 4154 instructions (1570 distinct) of the three corpora decode with no
unexplained bit and round-trip (`python3 hwcheck.py`), and the four oracle sweeps pass
(about 20,000 encoder outputs).
## Encoding conventions (read this first)

These rules hold in every class unless the class's section says otherwise.
All **oracle** (and every one of them is exercised by the corpora's words);
the bank codes are also **hardware** (every hwt test builds its operands from
them).

### Operand slots and bank codes

A register operand is a number field, a bank field and, in most slots, an
extension bit. The generic slots:

| slot | number | bank | extension |
|---|---|---|---|
| dst | l21-27 | h0-1 | h19 |
| src0 | l14-20 | h2 (one bit) | none in the float classes; efo h6, ima32 h15, smp/ld/st h18, emit and the visibility-test ops h19 |
| src1 | l7-13 | l30-31 | h17 |
| src2 | l0-6 | l28-29 | h16 |

Bank and extension values (bank, ext):

| dst | src1 / src2 | src0 |
|---|---|---|
| r 0,0 | r 0,0 | r 0,0 |
| o 1,0 | o 1,0 | pa 1,0 |
| pa 2,0 | pa 2,0 | o 0,1 |
| indexed 3,0 | sa 3,0 | sa 1,1 |
| sa 0,1 | indexed 0,1 | |
| g 1,1 | c or g 1,1 | |
| idx 2,1 | immediate 2,1 | |
| i 3,1 | i 3,1 | |

A slot without an extension bit takes only the ext = 0 kinds; a slot whose
class gives it a one-bit bank (the destination of smp, ld, eld, ldr) is
r = 0, pa = 1. A slot with no bank bits takes the one kind its class names.

### The number field

- r, o, pa, sa: the register number.
- c<n>: n (0-63) on the c/g bank; g<n>: n + 0x40 on the same bank, so a
  special register needs the full 7-bit field (never under format select).
- #n: n (0-127). Bigger constants: `limm`, or a constant in sa.
- i<n>: n on the i bank. The hardware has i0-i3; the disassembler prints
  whatever number is there. In a slot that can't select the i bank the
  register file's alias is used: i<n> is r(124+n), and the field holds
  124 + n (under format control that is register 60 + n with the format
  flag set, which is how the vendor compiler emits a C10 internal register in
  sop3's src0: **corpus**, and pixel-identical on hardware in the compiler's A/B
  runs).
- idx<n> (destination ext bank 2): n as printed.
- Indexed `bank[idx<k>+off]`: with a field of w bits (7, or 6 under format
  select), the number is `ib << (w - 2) | (k - 1) << (w - 3) | off`, where ib
  is r 0, o 1, pa 2, sa 3, and off has w - 3 bits (0-15, or 0-7). A source
  slot can't be indexed when it has no extension bit; src0 never can.

### Format select and format control

Two different things change how numbers read.

- **Format select** is a bit in the instruction: h22 in the float classes
  (hw 0, 2, 3, 4) and in fdual, h18 in test/testmask. (The hw 1 scalar
  functions mark their one source F16 or C10 with h7 / h8 instead, and
  fmad16 is always F16.) While it is set, every slot the class lists as
  following it has a 6-bit number (bits 0-5)
  and bit 6 says that operand is F16. An F16 r, o, pa or sa operand's number
  counts 16-bit halves: `r3.f16.lo` is 6, `r3.f16.hi` is 7 (**hardware**,
  hwt_p1 / hwt_p10). Indexed, immediate, constant and idx operands are not
  halved; a bare `.f16` on them only sets bit 6. For example, a test's
  `idx16.f16` destination has number 16 with bit 6 set.
- **Format control** is program state, set by `setfc` (MOE control, op 5)
  and at the start of a program C10 on, F16 off (**hardware**, hwt_p6 /
  hwt_p8). It applies only to these classes: sop2, sopwm, the sop3 family,
  dot3/dot4, ima8, fpma (the C10 enable) and efo (the F16 enable). In them,
  while the enable is on, an r, o, pa, sa or i operand's 7-bit field (the
  destination's as well as the sources') holds a
  6-bit number and bit 6 marks it C10 (or F16 for efo, whose F16 numbers
  count halves like the float classes'). Indexed, c/g, immediate and idx
  operands keep the whole 7-bit field. No other class reads format control.

### Unused slots and fixed patterns

A slot a form doesn't use is zero, except where the class names a pattern.
The pattern the vendor assembler writes for an unused source is the
immediate #0: bank 2 and the extension bit, number 0: in src2, l29 + h16.
mov, rlp, smp without a LOD source and ld without a range operand write it
in src2 (**oracle**, **corpus**).

A bit that no field, operand slot or fixed value of the form names is 0:
the vendor assembler never sets one, and no corpus word has one (**oracle**,
**corpus**).
That covers, for example, hw 1's h22, hw 3/4's h10 beside the one-bit sub-op,
h7 (|src0|) in the forms without a src0, mov/movc's h3-6, smp's h19,
testmask's h2-3 and h6, and every bit the special class's forms don't list.
A form without a predicate field (nop, idf, wdf, lock, the 0xd6/0xd7 ops,
emit, ldr, str) has none: a `(p0)` prefix on it is an error.

### Text notation and defaults

`asm.py` takes, and `dis.py` prints, the syntax below. An encoding check
compares words, so spellings that encode the same bits (bracket order,
number base, spacing) are equivalent.

    [(pred) ]mnemonic[.suffix ...] [operand, operand, ...][  [field field=v ...]]

- **Predicate.** The 3-bit field (PRED3): no prefix 0, `(p0)`-`(p3)` 1-4,
  `(!p0)` 5, `(!p1)` 6, `(pn)` 7 (lower case). The 2-bit field (PRED2, fdual):
  `(p0)` 1, `(p1)` 2, `(!p0)` 3.
- **Suffixes** are the class's enum fields by name, **in the order the class's
  syntax line gives** (`smp.2d.rdata.f16.bias`, `ld.a.b`, `movc.nz.u8`,
  `pck.f16.f32`, `test.fsub.lt`, `testmask.fpsub8.s1z1a.word`,
  `fdual.fadm.mov`); the order is fixed. A field at a value whose name is
  empty (smp's `ret`, `pcf`, `cfmt` and `lod` at 0) has no suffix. An enum
  value with no name is the field name and the value: `dim3`, `ret3`, `pcf3`,
  `cfmt3`, `am3`, `alu<two hex digits>`, and test's conditions
  `s<sign>z<zero><o|a>`. smp's `cfmt` is h3-4 as one field (1 `.f16` = h3,
  2 `.c10` = h4, 3 both) and `pcf` is h5-6 the same way.
- **Operands** come in the order of the class's syntax line, separated by
  commas. Registers are the prefix and a decimal number (`r3`, `pa12`, `c52`,
  `g17`, `i1`, `idx1`); `#n` is an immediate. An indexed operand is
  `bank[idx<k>+<off>]` with the offset always written (`pa[idx1+0]`;
  `pa[idx1]` is rejected). Negate is `-x`, absolute value `|x|`, both `-|x|`
  (not `|-x|`), each only in a slot with that bit.
- **Format suffixes.** Under format select (the `fmt` field), an F16 r, o, pa
  or sa operand is `r3.f16.lo` / `r3.f16.hi` (the number counts halves; a bare
  `.f16` on them is an error). An F16 indexed, constant, immediate or internal
  operand is a bare `.f16` that only sets bit 6: `pa[idx1+3].f16`, `c3.f16`,
  `#3.f16`, `i1.f16` (`.lo` / `.hi` on them is an error). The 6-bit numbers
apply to every slot that follows format select whatever the op, so a
test with `fmt` and a C10 ALU op has them too. An f32 operand under
  format select is written plain (`r40` is rejected: 6-bit numbers). Any format
  suffix sets `fmt`. Write `[fmt]` when all the operands are f32. Under format
  control (sop2, sopwm, the sop3 family, dot3/dot4, ima8, fpma) a C10 operand
  is `pa4.c10` (a whole register), and for efo an F16 one is `r3.f16.lo` /
  `.hi` (an internal register only takes the flag: `i1.f16`, as under format
  select). The destination follows format control like the sources, for efo's
  F16 enable as well as the C10 one (**oracle**). Either is accepted only while the enable is on.
  hw 1's source format is not an operand suffix: its operand is plain and the
  brackets carry `f16` (h7), `c10` (h8), `chan=n` (h3-4) and `f16d` (h2,
  F16 destination).
- **Numbers** print in decimal everywhere (`limm r1, #305419896`,
  `ba #4215`). Wherever a number goes, `0x` hex is accepted too, and a signed
  field takes `-n`: `br #-2` (relative to the branch), smoa, imo, smlsi
  increments.
- **Brackets.** There must be whitespace before the trailing `[` (an indexed
  operand's `[` has none); dis.py writes two spaces. Inside, order doesn't
  matter (dis.py prints a fixed order per class, not part of this spec). A
  bare name is a one-bit field that is set; `name=v` is any other field.
  An enum field in brackets takes its name or its number (`sd=i0s0` and `sd=3`
  are the same); a select value with no known meaning is `x<value>` (`c1=x7`,
  or `c1=7`). dis.py prints some bracket enums even at 0 (efo's `d= i= a= m=`,
  sop2's selects), but they can be left out. A field the current mode doesn't
  use is an error, not ignored: `rpt=4` without `rcnt` in the classes below.
- **Repeat or mask (h21 `rcnt`)** in hw 1, mov / movc / op0xf3, pck, the
  bitwise classes and smp: with h21 set, h12-15 is the repeat count, written
  `rcnt` for a count of 1 and `rcnt rpt=n` otherwise. With h21 clear, h12-15
  is the mask, written `mask=v` when it differs from the class's default
  (below). The other classes write `rpt=n` directly.
- **Other spellings asm.py takes.** efo's `i`, `a` and `m` as in the efo
  corpus table (`i=m0/m1`, `a=s0+s2/s1+s2`, `m=s1*s0/s2*s2`) or as dis.py
  prints them (`i=i0=m0,i1=m1`, `a=a0=s0+s2,a1=s1+s2`). dot's scales as
  `cs=x4` / `as=x2` or `cs=cx4` / `as=ax2`. A carry-register field (imae's
  `ci`, op16s1's `i`) as `i1` or `1`. fdual's `neg1` / `neg2` as the bracket
  flags or as `-x` on the src1 / src2 operand. Any enum field at 0 may be left
  out; dis.py prints the ones whose 0 has a name (efo's, the sop selects,
  dot's `a1`/`a2`), so the examples on this page differ in which they show.
- **fmad16's swizzles** are bracket fields, not operand suffixes:
  `swz0=`, `swz1=`, `swz2=` for src0-2 with the values `ll`, `hh`, `f32`;
  0 (lo, hi as stored) is the default and isn't written:
  `fmad16 r0, r1, r2, r3  [swz0=ll swz1=hh swz2=f32]`.
- **smp's mask and ret** share h12-15. With `.rdata` / `.rinfo`, ret sets
  h14-15, so only the mask's bits 0-1 (h12-13) are free. `mask=v` writes v
  into h12-15 first and the suffix then sets h14-15, so `mask=1` and `mask=5`
  give the same word with `.rdata`. dis.py prints the whole h12-15 value
  when it isn't 1.
- **Predicates outside h24-26.** depthf's and atst8's 2-bit predicate (h9-10,
  PRED2) is written as the usual prefix: `(p1) atst8 pa0, p0.nw, pa0, sa3, sa4  [end]`.
- **drc** (smp, ld, eld, ldr) is the bracket field `drc=n`, default 0. ld/st's
  range operand is written if and only if `range` (h6) is set. smp's LOD
  operand is written only when the `lod` suffix names one (`.bias`, `.lod`,
  `.grad`). Otherwise src2 holds the unused-source pattern and nothing is
  written for it.
- **fdual** is `fdual.<pri>.<sec> dst, src0, src1, src2` with the selects
  `sd=i0|i1|i2|i0s0`, `sb=i0|i1|i2|src1`, `sc=i0|i1|i2|src2` and
  `msrc=i0|i1|i2|src0|src1|src2` (the values in the Dual issue table, in
  order; `i0s0` is sd = 3).
- **Format control** is program state. A text check starts at the program-start
  state (C10 on, efo F16 off) and applies each `setfc #efo_f16, #c10` line in
  the order the lines come.
- Field widths are the word's; ranges a section quotes from the vendor
  assembler (imo -32..31, op0xfd n 0..8, the sizes and modes it rejects) are
  limits of that assembler (**oracle**). A value outside them still has the encoding the field gives;
  what the hardware does with it is not known.
- In the trailing brackets a bare name is a one-bit field that is set, and
  `name=v` is the field's value, except `rpt=n`, which is the repeat count:
  every class stores n - 1.
- A field the text doesn't show holds its default. The default is 0, except:
  - rlp's `unused2` (the src2 slot as one 10-bit value): 0x300, the unused
    pattern. Any other value is written `unused2=v` in the brackets.
  - `mask` (when there is no repeat count): 1 in smp, pck, test, testmask
    and the bitwise classes; 15 in sopwm; 0 elsewhere;
  - `dmask` in pck: 15;
  - `rpt`: 1 (stored as 0);
  - test/testmask with no destination: the destination field is 0x280 (see
    that section).
- A field written as several ranges (`h6-8, h24`) takes its value's low bits
  from the first range listed, and within a range the lower bit is the less
  significant (h0-1 = 2 sets h1). sopwm's `mask=v` puts v's bits 0-3 in h12,
  h13, h14, h11.
- Signed fields (branch targets, smoa, imo, smlsi increments) are two's
  complement in the field's width.
- smlsi's `#0.zyxw` is a swizzle: the operand's flag bit set and the value
  `x | y << 2 | z << 4 | w << 6` with x = 0, y = 1, z = 2, w = 3 for the
  channel each position reads (`#0.zyxw` = 0xc6).
- Values the vendor assembler takes as a multiple of 4 and stores divided
  by 4 (smlsi `lim0-2`, phas.i `temps4`) print as the stored field.
- `syncend` is h23 and `xsyncend` h12, two separate bits. The vendor
  assembler's extended sync-end sets both (**oracle**).

## Hardware checks (hwt.py, CE5300, 2026-10-28)

    python3 hwt.py hwt_p1 [-k name] [--dry]     (CAP, REPLAY, BOARD_SH set; see hwt.py)

Each test is a main pixel program poked into the probe frame's replay (main
program at 0x37008388, its DOUTU given 16 temporaries) that ends `mov o0, rN
[end]`; the PBE writes o0 unchanged, so the back buffer's pixels inside the
triangle are rN (all 32 checked pixels equal; the background is the clear
color). Inputs come from `limm`. About 400 draws, no GPU hang or recovery.
Logs: `hwt_out/<suite>.txt` (a second full run, identical to the first).
On the open stack (`tools/isa/hwt_open.py`, 2026-11-11), hwt_p1-p4, p6-p8 and p10-p12
reproduced all 650 recorded values exactly; open-stack logs are in `hwt_out/open/`.

| suite | what | result |
|---|---|---|
| hwt_sanity | limm, mov to o0, c52, r15 | readout path exact |
| hwt_p1 | fmad modifiers; fadm/fmsa/fsubflr; repeat with moe / inc0-2 / none / rpt=2; fmt + .f16 in each slot | confirmed; fadm/fmsa formulas found; **.f16 numbers count halves** |
| hwt_p2 | movc 4 conds x 5 types; test 6 conds + neg/pos; p0-p3, !p; testmask; br offsets, loop, predicated br | confirmed; movc u8/i16/c10 are per byte / half / channel, signed; f32 denormal = 0, NaN only nz |
| hwt_p3 | frcp frsq flog2 fexp2 fsqrt fsin fcos values; src modifiers; f16/c10 source + chan; rpt vs mask; c0-c63 | sin/cos argument in quarter turns; **h2 = F16 destination**; C10 = clamp(v/255, ±2); **c46/c47 = F16 (1,1)**, c57-c63 |
| hwt_p4 | fminmax/fmaxmin (all orders); fmad16 swizzles x 3 sources; pck dmask lanes, u8/s16/f16 rounding, rint rnd 0-3, u8 unpack lanes | confirmed; swizzle 0-3 = lohi, lolo, hihi, f32; rnd = nearest-even, down, up, zero; **f32 -> f16 truncates** |
| hwt_p5 | fadm/fmsa on new inputs; h2 partial write; C10 chan 3 and range | as above |
| hwt_ba | ba absolute target | instruction index from the code base |
| hwt_window | code base windows 0-2; ba from bases 0-2; DOUTU offset and ba target past 512 KiB | base n runs heap + n·512K + offset; **ba, and phas.i, count from the task's code base, not the heap**; both fields reach past the window |
| hwt_p6 | sop2 U8 select/add/sub/mul; C10 operands with each setfc | **setfc's 2nd argument is the C10 enable, on at start** |
| hwt_p7 | efo F16 numbering; fmad fused?/rounding; fmad16 rounding; C10 alpha; testmask byte/word; call + lapc; fdp; bitwise | efo F16 counts halves; **fmad rounds the product (not fused)**; fmad16 rounds to nearest; fdp writes only the last iteration's dst |
| hwt_p8 | setfc's first argument; C10 alpha source; C10 -> U8 byte order | **setfc's 1st argument is the efo F16 enable, off at start**; C10 alpha reads channel 0 |
| hwt_p10 | test with format select: F16 numbering, chan on F16 and C10 compares | **test's F16 operands count halves too**; C10 operands are whole registers; chan table |
| hwt_p11 | a repeat over an F16 operand | moe / inc step it by one half |
| hwt_p13 | pck.u8.u8 lane picks per dmask (3, 12, 6, 9) and RGBA8 -> BGRA8 in two; and / or of 0 and 0xffffffff test masks; test.and.eq into p0 on a mask | an 8-bit source's lane picks its byte, the first source filling the lowest dmask lane; and / or are bitwise on masks; test.and.eq is true for a zero mask, false for 0xffffffff (2026-11-09, all 11 ok) |
| hwt_gaps (open stack) | ld.a.d/w/b offsets and sizes; i0 across loads and a wdf, with and without nosched; test with a dest and with mask=15; fddp's i<n>; ima32 hi | see "hw 1d: ld", "Dual issue", "test vs testmask", fddp, ima32 (2026-11-11, `tools/isa/hwt_open.py` through `sgx_hwt`: powervr_sgx and the open microkernel, nothing from the vendor stack) |
| hwt_int (open stack) | ima32 add/sub/multiply low words, immediates, `-a` as `a * c56`, high words unsigned and `.sgn`; testmask.xor eq/ne and testmask.or neg/pos/gt/le dwords; test.xor / test.and into p0; and.inv with a register src2, xor.inv #0, asr / shl by a register; pck.f32.s16/u16 lanes, pck.u16.f32 pairs and clamping; frcp | all 50 expected words (the integer forms the compiler's native_integers path emits: COMPILER-GL3.md); frcp exactly rounded on 3, 7, 1000003, 4294967040, 1.5 (2026-11-11, pvrsrvkm normal boot, `sgx_hwt` services backend) |
| hwt_p12 | efo's m/a/i/d modes, wi, negates, chaining; fdual's every secondary and primary, i0s0, slot sharing, negates, rpt=2, same-word timing | all units as tabled; secondary sources settled (sd = 1st, sb, sc); sec 3 is fsubflr; neg1/neg2 primary only; same-word read of an internal register being written is a **race** (efo d, fdual primary) |

hwt_p7-p9's `test` lines with `.f16` / `.c10` were drawn before the assembler
fix below: without `[fmt]` the assembler of the time **silently dropped** the
suffix, so those draws compared plain 32-bit registers (the suites are written
that way now, so they assemble to the words that ran; their `test` results
are not F16/C10 facts). hwt_p10 redoes them with format select.

**Text syntax (changed 2026-10-28, from these results):** where an F16 operand
number counts halves (float classes and test under format select, efo under
F16 format control), dis.py prints and asm.py takes the register and the half:
`r0.f16.lo` = encoded number 0, `r0.f16.hi` = 1, `pa3.f16.lo` = 6. A bare
`.f16` there is an error, as is a `.f16` / `.c10` an instruction can't encode
(it used to be dropped); asm.py sets `fmt` itself when an operand has a format
suffix. C10 operands stay whole registers (`pa4.c10`).

## Instruction classes

h27-31 is the hardware opcode:

| hw | class | forms | section |
|---|---|---|---|
| 00 | float 3-src | fmad fadm fmsa fsubflr | Float |
| 01 | float scalar | frcp frsq flog2 fexp2 fsqrt fsin fcos | Float |
| 02 | dot products | fdp fdpc fddp fddpc | Float |
| 03 | min/max | fmaxmin fminmax | Float |
| 04 | derivatives | fdsx fdsy | Float |
| 05 | moves | mov movc op0xf3 | Moves |
| 06 | f16 | fmad16 | Float |
| 07 | efo | efo | Integer |
| 08 | pack | pck.<dst>.<src> | Pack |
| 09, 0f | test | test testmask | Pack |
| 0a-0e | bitwise | and or xor shl rol shr asr rlp | Pack |
| 10-1a | integer / fixed point | sop2 sop3 sopwm ima8 ima16 imae fpma dot3/dot4 ... | Integer |
| 1b | dual issue | fdual | Dual issue |
| 1c | sample | smp | Memory |
| 1d, 1e | memory | ld eld st | Memory |
| 1f | special | branches, MOE setup, wdf, nop, limm, ... | Memory |

## Registers

| prefix | file | encoding | notes |
|---|---|---|---|
| r | temporaries | 7-bit number | r124-r127 are the internal registers i0-i3: the vendor assembler encodes i<n> as r(124+n) in any slot that can't name the internal bank (**oracle**); under format select, numbers are 6 bits |
| pa | primary attributes | 7-bit | fragment: iterated varyings and non-dependent texture results; vertex: the vertex attributes; the fragment color is assembled here (**corpus**). The budget is 0x80 minus USP header +0x3c (**unconfirmed**) |
| sa | secondary attributes | 7-bit | uniforms, texture state, results of the secondary update program (**corpus**) |
| o | outputs | 7-bit | vertex outputs; the finalized fragment color (**hardware**) |
| c | hardware constants | number < 64 on the c/g bank | table below (**hardware**, hwt_p3) |
| g | special registers | number + 0x40 on the c/g bank | g0-g45 (**oracle**: range); the corpus uses g17 as the per-instance stride when addressing temp arrays |
| i | internal | i0-i3 | bank 3 + ext in dst/src1/src2; fdual's secondary op works in i0-i2 |
| idx | index registers | idx1, idx2 | set by an integer op (ima16/imae into idx), then `bank[idxN+off]` (**oracle**, **corpus**) |
| p | predicates | p0-p3 | written by `test`, read by every instruction's predicate field |
| # | immediate | 7 bits in a source slot | bigger constants: `limm` (special class) or sa |

Indexed operands (**oracle**): any of r, o, pa, sa as `bank[idxN+off]`; the
7-bit number field holds bank(2) | index register(1) | offset(4) (offset 3
bits under format select). The destination's indexed bank is bank 3; a
source's is bank 0 + extension. src0 can't be indexed.

### Hardware constants (c<n>)

The hardware's special constants, every entry read back on the CE5300 board
(**hardware**, hwt_p3 `c0`..`c63`: `mov o0, cN`). **c46 and c47 are the
F16 pair (1.0, 1.0) = 0x3c003c00 like c45**, and c57-c63 exist. c28-c44 are the correctly rounded f32
values (c28 = 0x402df854, c32 = 0x3f490fdb, c35 = 0x3ea2f983, c40 =
0x35d00d01, c44 = 0x3caaaaab).

| c | value | c | value | c | value |
|---|---|---|---|---|---|
| 0-2 | 0 | 3 | 1.0 | 4-13 | 2, 4, 8, ... 1024 |
| 14-27 | 0.5, 0.25, ... 2^-14 | 28 | e | 29 | 1/e |
| 30 | √2 | 31 | 1/√2 | 32 | π/4 |
| 33 | π/2 | 34 | π | 35 | 1/π |
| 36 | 2/π | 37 | 4/π | 38 | 2π |
| 39 | 2^-16 | 40-44 | sin/cos series coefficients (1.55e-6, 2.17e-5, 2.60e-4, 2.60e-3, 0.0208) | 45 | F16 pair (1.0, 1.0) |
| 46 | F16 pair (1.0, 1.0) | 47 | F16 pair (1.0, 1.0) | 48-51 | 0.0 (four, for vec4 repeats) |
| 52-55 | 1.0 (four) | 56 | 0xffffffff | 57-59 | 0xffffffff |
| 60-63 | 0x7fff7fff (F16 NaN pair, or two s16 maxima) | | | | |

## Float, moves and dual issue (hw 00-06, 1b)

### Common fields (all ALU classes)

| bits | field | meaning | evidence |
|---|---|---|---|
| h27-31 | opcode | hardware class | oracle |
| h24-26 | pred | 0 always, 1 p0, 2 p1, 3 p2, 4 p3, 5 !p0, 6 !p1, 7 pn (per-instance predicate) | oracle; hardware (hwt_p2: p1-p3 and `!p0` / `!p1`); the pn name is a **guess** |
| h24-25 | pred (short form, fdual and some special ops) | 0 always, 1 p0, 2 p1, 3 !p0 | oracle |
| h23 | skipinv | skip invalid instances (set on nearly every compiled instruction) | oracle for the bit; corpus for how often it is set; the meaning is **unconfirmed** |
| h20 | syncs | sync start | oracle for the bit; the meaning is **unconfirmed** |
| h11 | nosched | no scheduling | oracle for the bit; the meaning is **unconfirmed** |
| h18 | end | last instruction of the program | oracle; hardware (the finalized probe programs end with it) |

### Operands

| slot | number | bank | ext | values |
|---|---|---|---|---|
| dst | l21-27 | h0-1 | h19 | r, o, pa, indexed (bank 3); with ext: sa, g, idx, i | 
| src0 | l14-20 | h2 | (none in float ops) | r, pa (o and sa need an ext bit only some classes have) |
| src1 | l7-13 | l30-31 | h17 | r, o, pa, sa; with ext: indexed, c/g, #imm, i |
| src2 | l0-6 | l28-29 | h16 | same as src1 |

All **oracle**. Indexed: number = bank(2) idx(1) offset(4): `pa[idx1+3]`; under
format select the offset has 3 bits. g (special registers) = number | 0x40
on the c/g bank; numbers 0-0x2d. Immediates 0-127. Internal registers in a
slot that can't hold them (src0; dst/src without ext) are r124+n (oracle):
hardware aliases i0-i3 onto r124-r127 (hardware, hwt_p12: fdual words that
name r124 / r125 read and write i0 / i1).

### Float classes (hw 0-4, 6)

| class | sub-op | forms |
|---|---|---|
| hw0 | h9-10 | 0 fmad (s0*s1+s2), 1 fadm, 2 fmsa, 3 fsubflr (fsubflr(a, a) = fract(a)) |
| hw1 | h9-10 + h16 | 0 frcp, 1 frsq, 2 flog2, 3 fexp2, 4 fsqrt, 5 fsin, 6 fcos |
| hw2 | h9-10 | 0 fdp (l14 clear) / fdpc (l14 set), 1 fddp, 2 fddpc |
| hw3 | h9 | 0 fmaxmin, 1 fminmax (three sources) |
| hw4 | h9 | 0 fdsx, 1 fdsy |
| hw6 | - | fmad16 |

All sub-op values **oracle**; hw1 names **corpus** (one-op shaders), values
**hardware** (hwt_p3); hw0 fmad/fsubflr **corpus**. Semantics **hardware**
(hwt_p1, hwt_p5): fmad = s0*s1 + s2; **fadm = (s0 + s1) * s2**; **fmsa =
s0*s0 + s1*s2**; fsubflr(a, b) = a - floor(b) (2.75, 1.5 -> 1.75; -2.75,
-2.75 -> 0.25). Each checked on two or three operand sets (fadm (-3, 5,
7.5) -> 15, (2, 3, 4) -> 20, (0.5, -6, 3) -> -16.5; fmsa -> 46.5, 16,
-17.75).
fminmax/fmaxmin: the vendor assembler encodes fmin(a, b) as hw3 sub 1 on
(a, a, b) and fmax(a, b) as sub 0 on (a, b, a) (**oracle**). **hardware**
(hwt_p4, all six orderings of (1, 2, 3)): **fminmax (sub 1) = min(max(s0,
s1), s2)**, **fmaxmin (sub 0) = max(min(s0, s2), s1)**: a clamp is one
instruction, `fminmax d, x, lo, hi`.

Modifiers, 3-source ops (**oracle**; each bit **hardware**, hwt_p1 `fmad_*`:
-|src1| and -|src2| combine as expected): h7 = |src0|; src0 negate has no bit
(the vendor assembler folds it into src1's negate for fmad/fmad16/fddp and
into src1 and src2 for fadm; fmsa rejects it); h5/h6 = -src1 / |src1|; h3/h4
= -src2 / |src2|. Two-source ops: src1 h5/h6, src2 h3/h4.

Which slots each form uses (**oracle**). The destination is the generic
dst slot in all of them except fddp/fddpc (no extension bit). A slot not
listed is zero.

| forms | text operands -> slots | follow format select (h22) |
|---|---|---|
| fmad fadm fmsa fmaxmin fminmax | dst, src0, src1, src2 | src0, src1, src2 |
| fsubflr fdp fdsx fdsy | dst, src1, src2 (no src0: l14-20 and h2 are 0) | src1, src2 |
| fdpc | dst, cp<n> (l15-17), src1, src2; l14 = 1 | src1, src2 |
| frcp ... fcos (hw 1) | dst, src1 (the single source is in the src1 slot, -src h5, \|src\| h6; src2's slot is unused and h16 is part of the sub-op) | none: h7 / h8 per source instead |
| fmad16 (hw 6) | dst, src0, src1, src2; modifiers as hw 0 | none: every operand is a whole register holding an F16 pair, numbered as a register (no halves) |
| fddp | dst (l21-27, h0-1), i<n> (h19: 0 i0, 1 i1), src0, src1, src2; modifiers as hw 0 | src0, src1, src2 |
| fddpc | dst, i<n> (h19), cp<a> (h3-5), cp<b> (h6-8), src0, src1, src2; no modifiers | src0, src1, src2 |

hw 1's `chan=` is the h3-4 field whatever h7/h8 say; it only selects a half
or a channel when one of them is set. The repeat fields (inc0-2 and the
rest) are in every hw 0/2/3/4 form, including those with no src0; fddpc's
second clip plane (h6-8) shares h8 with inc2: the word holds the OR of
the two, so a decoder can't tell them apart.

Repeat (SGX545, **oracle**): hw0/2/3/4: h12-13 = repeat count - 1 (the
vendor assembler turns a write mask into a count; at most 4). h21 = moe
(operands step by the MOE increments). With h21 clear, h15/h14/h8 say
whether the first/second/third source steps. **hardware** (hwt_p1 `rpt_*`,
fmad rpt=4 on r0-3/r4-7/r8-11):
`moe` steps all three sources by 1 (the MOE state at program start is
increment 1); `inc0`/`inc1`/`inc2` step src0/src1/src2 and nothing else; the
destination steps on every iteration in all cases (no inc bit: four copies of
the first result in r12-r15); rpt=2 stops after two (r14 untouched).
hw1 (scalar): h21 = rcnt; set: h12-15 = count - 1 (1-16); clear: h12-15 =
write mask. **hardware** (hwt_p3 `frcp_rpt4_*`, `frcp_mask*`): rcnt rpt=4
on r0-r3 writes 1/r0..1/r3 to r4-r7; mask=5 writes r4 = 1/r0 and r6 = 1/r2
only (bit n enables iteration n; source and destination step over all four);
mask=0 writes nothing.

h22 = fmt (format select, **oracle**): set when any source is F16; every
source number then has 6 bits and number bit 6 marks that source as F16
(`pa1.f16.lo`). The destination is unaffected.
**hardware correction** (hwt_p1 `f16_*`): **an F16 source's 6-bit number
counts 16-bit halves, not registers**: encoded number N is register N>>1,
half N&1 (0 = bits 0-15). With r0 = (lo 3.0, hi 2.0) and r1 = (5.0, 4.0),
numbers 0, 1, 2, 3 (now written `r0.f16.lo`, `r0.f16.hi`, `r1.f16.lo`,
`r1.f16.hi`) read 3, 2, 5, 4, in all
three source slots. So F16 operands reach only the first 32 registers of a
bank. f32 sources of the same instruction keep whole-register numbers (0-63)
(`fmt_f32`). This is how the corpus's `fmad r0, pa0.f16.lo` / `fmad pa0,
pa0.f16.hi` pairs (raw numbers 0 and 1, printed `pa0.f16` / `pa1.f16`
before the syntax change) widen one mediump pa register's two halves. dis.py
and asm.py now write F16 operands this way (see "Hardware checks"). A repeat
steps an F16 operand by one half, with `moe` or its inc bit (hwt_p11: `fmad
r12, r0.f16.lo, c52, c48 [rpt=2 moe fmt]` reads r0.lo then r0.hi), which is
what the corpus's mediump `fdp pa0, pa2.f16.lo, pa0.f16.lo [rpt=3 moe fmt]`
(vec3 over pa2.lo, pa2.hi, pa3.lo) relies on.

Rounding, **hardware** (hwt_p7): **fmad is not fused**: (1 + 2^-12)^2 -
(1 + 2^-11) gives 0, not 2^-24, so the product is rounded to f32 before
the add; the product rounds to nearest ((1 + 2069*2^-23)^2 -> 0x3f80102b).
fmad16 rounds to nearest in F16 ((1 + 23*2^-10)^2 -> 0x3c2f, truncation
would give 0x3c2e), and so does its f32 -> F16 swizzle (1 + 3*2^-12 ->
0x3c01), unlike `pck.f16.f32`, which truncates.

hw1 source format (**oracle**): h7 = F16 source (h3 picks the high half);
h8 = packed source with h3-4 picking one of four channels. h2 is a
separate flag. **hardware** (hwt_p3, hwt_p5):
- F16 (h7): h3 = 0 low half, 1 high half; h4 is ignored (chan 2/3 read the
  high half like chan 1). The register number is a whole register here.
- h8 is **C10**: the channel is a signed 10-bit value v at bits 10*chan, read
  as **clamp(v / 255, -2, 2)** (0x0ff = 1.0, 0x100 = 256/255, 0x1fe = 2.0,
  0x1ff = 2.0, 0x3ff = -1/255, 0x300 = -256/255, 0x200/0x201 = -2.0).
  chan 3 of a 32-bit register is bits 30-31 as an unsigned 2-bit value (0x40000000
  -> 1/255; the 40-bit internal registers presumably hold a full 10-bit alpha).
- **h2 = F16 destination**: with an F16 source, the result is written as F16
  into the half the source half selects (h3), and the other half is
  **zeroed** (r3 = 0x5a5a5a5a -> 0x00003400 for 1/4 from the low half,
  0x38000000 for 1/2 from the high half). With an f32 source h2 does nothing
  (f32 result). Under a repeat the half stays fixed and the registers step.
  This is how the corpus's mediump scalar code (`frcp pa9, pa9 [f16 chan=1
  f16d]`) works in place. dis.py prints h2 as `f16d`.

fmad16 (**oracle**): per-source swizzle:
src0 h9-10, src1 h13 + h18, src2 h21-22 (so fmad16 has no end bit); repeat
1-2 in h12 (needs explicit increments h15/h14/h8). The swizzle is
written `swz<n>=` in brackets, no field / `ll` / `hh` / `f32`: **hardware** (hwt_p4 `fmad16_swz*`, the
same for all three sources): 0 = (lo, hi) as stored, 1 = (lo, lo), 2 = (hi,
hi), 3 = the register read as one f32 and converted to F16 for both lanes
(r0 = f32 1.5 -> 1.5 in both; r0 = H(2, 3) = f32 32.06 -> 32.06). fmad16
computes both lanes (r0 = (2, 3), r1 = (5, 7), r2 = (0.25, 0.5) -> (10.25, 21.5)).

fdp, **hardware** (hwt_p7 `fdp_rpt*`): `fdp r12, r0, r4 [rpt=n moe]` is the
n-component dot product, written only to the last iteration's destination
(r12 + n - 1: rpt=3 -> r14 = 3210, rpt=4 -> r15 = 43210 for (1, 2, 3, 4) .
(10, 100, 1000, 10000)); the other destinations keep their values.

fdpc (**oracle**): fdp + a clip-plane operand cp0-7 in l15-17, l14 set.
Needs a multi-channel repeat or mask (oracle). That it writes a clip-plane
result to cp<n> is **unconfirmed**.
fddp/fddpc (**oracle** for the fields): the i<n> operand is in h19, so the
destination has no extension bit; fddpc's clip planes are in h3-5 (the
first cp operand) and h6-8 (the second), no source modifiers. That fddp
computes two dot products is **unconfirmed**. **hardware** (hwt_gaps
`fddp_*`, open stack, 2026-11-11): the second result goes to the i<n> in
h19. With r0, r1, r2 = 1, 2, 3, `fddp r3, i1, r0, r1, r2` gave r3 = 2.0 and
i1 = 3.0; what each product sums over a repeat is not settled by that. The
fdual primary fddp also leaves a second product in i0 (hwt_p12, below).

### Moves (hw 5)

| h8-10 | form |
|---|---|
| 0 | mov d, s (src1 slot; src0's field is 0; src2 is the unused-source pattern: bank 2 + ext = #0) |
| 1-5 | movc.<cond>.<type> d, s0, s1, s2: d = test(s0) ? s1 : s2 |
| 6, 7 | `op0xf3.s6 d, s1, s2` / `op0xf3.s7 d, s0, s1, s2` (oracle; the vendor compiler never emits it, corpus): writes an output (**unconfirmed**). Operand slots as `movc`; 6 has no src0 (its field is 0). Flags: `f1_1` = h7 and `f1_26` = h22 (the bits that hold movc's type), `a3f` = h5-6 (0-3). Common fields, repeat and mask as below |

Repeat: h21 rcnt, h12-15 count-1 or write mask (**oracle**).
movc type (**oracle** for the bits; behavior **hardware**, hwt_p2, below):
1 u8, 2 i16, 3 i32, 4 f32, 5 c10. Condition h7 + h22 (**oracle**): 0 `z`
(s0 == 0), 1 `nz`, 2 `ltz` (< 0), 3 `gez` (>= 0); these are the only
conditions the vendor assembler accepts for movc. The test class's
conditions are in the "Pack/unpack, test, testmask, bitwise" section.

**hardware** (hwt_p2 `movc_*`, s1 = 0x11111111, s2 = 0x22222222):
- **f32**: `z` true for +0, -0 and a denormal (0x00000001: denormals are
  zero); `ltz` true only for a negative normal number (-0 and NaN are not
  < 0); `gez` true for +0, -0, positives and the denormal, false for NaN;
  `nz` true for NaN. So NaN is nz but neither ltz nor gez.
- **i32**: two's complement on the whole word (0x80000000 and 0xffffffff
  are ltz).
- **u8**: **per byte**, signed (bit 7 is the sign): s0 = 0x00ff8001 with
  `ltz` takes bytes 1 and 2 from s1 (0x22111122).
- **i16**: per 16-bit half, signed.
- **c10**: per 10-bit channel (bits 0-9, 10-19, 20-29), signed: s0 =
  0x3ff00200 with `ltz` takes channels 0 and 2 from s1 (0x11122111). (Bits
  30-31 were equal in s1 and s2, so not seen.)

A move to or from one special register type is a hw 1f word (`f80000c0`:
it <- src1; `f8000100`: dst <- it) (oracle); see the "smp, ld/st, and the
special class" section.

### Dual issue (hw 1b, **oracle**)

The vendor assembler pairs an instruction with the next into one hw 1b word;
the SGX545 target accepts it (oracle; the corpus has fdual words). The
primary op uses dst/src0/src1/src2 (dst has no ext bit); the secondary
computes into internal registers.

| bits | field | values |
|---|---|---|
| h24-25 | pred | short predicate |
| h18, h19, h26 | pri | 0 fmad, 1 mov, 2 fadm, 3 fmsa, 4 fddp, 5 fminmax, 6 ima32, 7 fssq |
| h6-9 | sec | 0 fmad, 1 fmad16, 2 fmsa, 3 fsubflr (a fadm or fract secondary encodes as it too), 4 fmaxmin, 5 frcp, 6 frsq, 7 fexp2, 8 flog2, 9 fsin, 10 fcos, 11 fadd, 12 fsub, 13 fmul, 14 ima32, 15 mov |
| h5, h13 | sd | secondary destination (and its first source): 0 i0, 1 i1, 2 i2, 3 `i0s0`: i0 with the first source taken from the src0 slot |
| h14-15 | sb | secondary second source: i0, i1, i2, or 3 = the primary's src1 |
| h10, h21 | sc | secondary third source: i0, i1, i2, or 3 = the src2 slot |
| h14, h15, h21 | msrc | instead of sb/sc when the secondary is mov: 0 i0, 1 i1, 2 i2, 4 the src0 slot, 5 the src1 slot, 6 the src2 slot (the vendor assembler puts a register operand into whichever of src0/src2 the primary leaves free) |
| h12 | rpt | 1-2 |
| h22 | fmt | format select as in the float classes |
| h4, h3 | neg1, neg2 | negate the src1 / src2 slot |
| h23, h11, h20 | skipinv, nosched, syncs | as in the other ALU classes; fdual has no end bit (h18 is part of `pri`) |

A one-source secondary op reads sb; a two-source one sb and sc; mov reads
msrc. All **oracle**.

**hardware** (hwt_p12, 2026-11-08): 113 draws, every one ran with no
recovery. Inputs were s0 = 2, s1 = 3, s2 = 5 and i0 = 7, i1 = 17, i2 = 19;
the logs are in `hwt_out/hwt_p12.txt`. The encodings were already settled:
every corpus fdual word assembles from its text to the same bits
(`check_enc.py`).

#### fdual for a compiler

**Secondary op.** It writes only the internal register `sd` names (`i0s0`
writes i0). The other two internal registers are unchanged after every op.
Its sources, by **hardware** (hwt_p12):

| sec | computes | e.g. (i0 = 7, i1 = 17, i2 = 19) |
|---|---|---|
| 0 fmad | `sd * sb + sc` | 7 * 17 + 19 = 138 |
| 2 fmsa | `sd * sd + sb * sc` | 49 + 323 = 372 |
| 3 | `sb - floor(sc)` (fsubflr; fract is it with sb = sc; not fadm) | 17 - 19 = -2 |
| 4 fmaxmin | 17 from (7, 17, 19): `min(sb, sc)` or `min(max(sd, sb), sc)`, not yet told apart | 17 |
| 5-10 frcp, frsq, fexp2, flog2, fsin, fcos | `op(sb)` (sin/cos in quarter turns, as in the float class) | 1/17, 2^17, ... |
| 11 fadd, 12 fsub, 13 fmul | `sb op sc` | 36, -2, 323 |
| 15 mov | `msrc` (i0-i2 or the src0/src1/src2 slot) | |
| 1 fmad16, 14 ima32 | ran with f32 inputs; F16 / integer meaning not drawn | |

- `sd=i0s0` replaces the first source (the one `sd` names in fmad/fmsa) with
  the src0 slot: fmad gives `src0 * sb + sc` (2 * 17 + 19 = 53). It doesn't
  touch one- and two-source ops.
- `sb=src1` and `sc=src2` read those slots (7 * 3 + 19 = 40, 7 * 17 + 5 = 124).

**Primary op**, beside a `mov i0` secondary (**hardware**):
- fmad `src0 * src1 + src2` (11); mov `src1` (3);
- fadm `(src0 + src1) * src2` (25); fmsa `src0^2 + src1 * src2` (19);
- fminmax `min(max(src0, src1), src2)` (3); fssq `src1^2 + src2^2` (34);
- fddp gives `src0 * src1` (6) in dst, and leaves `src0 * src2` (10) in i0.
  The secondary's own `mov i0` with `sd=i0` was overridden; whether fddp's
  second result follows `sd` or is always i0 is not drawn.
- ima32 with f32 inputs: not interpreted.

**Sharing a slot.** Both ops read the same slot value
(`fdual.fmad.fmad r3, r0, r1, r2 [sd=i0 sb=src1 sc=src2]`: r3 = 11,
i0 = 7 * 3 + 5 = 26). **`neg1`/`neg2` (-src1 / -src2) apply to the primary
only**: the same word with `-r1, -r2` gives r3 = -11 and still i0 = 26.

**Where a mov secondary's register source goes.** Any slot works: msrc=src0
reads src0 even while an fmad primary reads it too. The vendor compiler's
habit (corpus) is to put it in a slot the primary doesn't read (src0 beside
mov or fssq, src2 otherwise). That is a choice, not a rule.

**Pairings.** Every secondary under a mov primary, and every primary under a
mov secondary, ran cleanly. Nothing seen forbids a pairing.

**Repeat.** With `rpt=2` the primary steps (r14 = 2 * 3 + 5, r15 = 4 * 6 + 10).
The secondary's internal registers don't step: `sd` holds the one result
(17 + 19), and the others are unchanged.

**Timing.**
- The next instruction reads what the secondary wrote (every hwt_p12 test
  reads back that way).
- In the same word, the secondary reads an internal register before the
  primary's destination writes it. `fdual.fmad.mov r125, r0, r1, r2
  [sd=i0 msrc=i1]` leaves i0 = 17 (the old i1) and i1 = 11, the same on
  every pixel.
- **The other way round is a hazard.** The primary reading the internal
  register the same word's secondary writes (`fdual.mov.fexp2 r3, r0, r124,
  r2 [sd=i0 ...]`) gave the old value on some pixels of the row and the new
  one on others. A compiler must not do it. (The vendor compiler's fl_big
  corpus shader has `fdual.fmad.fsin pa1, r126, ... [sd=i2]`; don't copy it.)
- Internal registers don't survive a deschedule (**unconfirmed**). The vendor
  compiler puts `nosched` on the words between an internal register's write
  and its last read (corpus). Not reproduced on hardware (hwt_gaps `i0_*`,
  open stack, 2026-11-11): an i0 written with `mov i0, r2 [mask=1]` read back
  unchanged across one and four `ld.a.d` plus a `wdf`, with and without
  `nosched`, on every checked pixel of a 720x480 triangle. The loss may need a
  sample or other work between the write and the read.

`fssq` exists only as a dual-issue primary (oracle): two sources in the
src1/src2 slots, each source channel feeding the same destination channel
(**unconfirmed**). The corpus uses it once, in `v_norm`
(`normalize(p.xyz)`): `fdual.fssq.mov o0, pa2, pa0, pa1 [sd=i2 msrc=src0]`
then `fdual.mov.fmad o3, r0, c52, o0 [sd=i2 sb=i2 sc=src2]` and
`frsq o1, i2`: i2 = p.z, o0 = fssq(p.x, p.y), i2 = i2 * i2 + o0, so
fssq(a, b) = a*a + b*b (**corpus**; **hardware**, hwt_p12, above; the name
is a **guess**). ima32 is described in the "Integer, fixed-point and
multi-unit ops" section.

Checked: `check_float.py` (3340 oracle encodings over every operand type,
modifier, repeat/mask/increment, predicate, flag, swizzle, movc type and
condition and fdual pairing) and `hwcheck.py` (all corpus words of these
classes): no unexplained bit, every word round-trips. The float and move
forms in mesa/src/sgx/isa/tests/gles2.txt are emitted by the open compiler
and exercised by the open stack (dEQP-GLES2).

## Pack/unpack, test, testmask, bitwise (hw 08, 09, 0a-0e, 0f)

Forms: `hw_pack.py`. Checks: `check_pack.py` (oracle sweep, 1579 encodings,
0 failures) and `hwcheck.py` (all 281 pack, 107 test, 34 testmask corpus words
decode with no unexplained bits and round-trip). The pack, test, testmask
and bitwise forms in mesa/src/sgx/isa/tests/gles2.txt are emitted by the
open compiler and exercised by the open stack (dEQP-GLES2).

Operand slots are the generic ones: src1 l7-13, bank l30-31, ext h17; src2
l0-6, bank l28-29, ext h16; dest l21-27, bank h0-1, ext h19. None of these
classes has a src0 slot. Predicate h24-26 as in the float classes.

Evidence tags as in the rest of this document: **oracle**, **corpus**,
**hardware**, **guess** (name/meaning not backed).

### Shared ALU control bits (all three classes)

| bits | field | tag |
|---|---|---|
| h24-26 | pred | oracle |
| h23 | skipinv | oracle |
| h22 | nosched (h22 here, not h11 as in the float classes) | oracle |
| h20 | syncs | oracle |
| h18 | end (pck, bitwise only; test rejects it) | oracle |
| h21 | rcnt (pck, bitwise): set for a repeat count | oracle |
| h12-15 | rpt = n (stored n-1) when rcnt; else write mask | oracle; per-iteration write enable: **hardware** for the scalar class (hwt_p3 `frcp_mask5_*`), assumed the same here |

### hw 08: `pck.<dst>.<src>`

Format codes (both fields): 0 u8, 1 s8, 2 o8, 3 u16, 4 s16, 5 f16, 6 f32,
7 c10. The code -> name map is **oracle**; the u8, s16, f16 and f32
conversions are **hardware** (hwt_p4), and the corpus uses c10 for lowp
(`pck.c10.c10`), f16 for mediump (`pck.f16.f32`) and u8 for color writes
(`pck.u8.f32`); "o8" as an offset/biased 8-bit format is a **guess**.

| bits | field | values | tag |
|---|---|---|---|
| h27-31 | opcode | 8 | oracle |
| h9-11 | source format | as above | oracle |
| h6-8 | destination format | as above | oracle |
| h2-5 | dmask | destination byte write mask. Assembler limits: dst u16/s16/f16 only 3, 0xc, 0xf; f32 only 0xf; 8-bit and c10 any. A partial mask needs a non-indexed dest, and on an `i` dest only c10 takes one | oracle; corpus (`pck.u8.f32 … dmask=6/9` pairs build RGBA8 two bytes at a time); hardware (hwt_p4 `pck_u8_dmask*`, hwt_p13) |
| l16-17 | src1 lane (`.l<n>`) | 0-3 | oracle; meaning by source format is corpus: 8-bit sources pick a byte (`pck.u8.u8 pa9, pa9.l3`), 16-bit sources use 0/2 = low/high half (`pck.f16.f16 pa9, r0, pa9.l2`), c10 picks the 10-bit channel (`pck.f16.c10 pa11, i2.l2, i2.l1`); hardware for 8-bit sources (hwt_p4 u8 unpack lanes, hwt_p13 `pck.u8.u8` lane picks) |
| l14-15 | src2 lane | 0-3 | oracle, as above |
| l18 | `scale` (pairs with one float side). `rint` (float->float pairs). Int->int pairs: the assembler never sets it | oracle for when it is set. scale = normalize (corpus: every F32->U8 color write has it; hardware, hwt_p4, below). rint = round to an integral value in mode `rnd` (**hardware**, hwt_p4 `rint_*`), so a trunc is `pck.f32.f32 [rint rnd=zero]` (the name is ours) |
| l19-20 | rnd | 0-3. Required for float->float pairs; with `scale` on float<->int pairs; otherwise not encoded | oracle for when it is encoded. **hardware** (hwt_p4 `rint_*`, `pck.f32.f32 [rint]` on ±2.5, ±1.7, 3.5, 0.5): **0 = nearest-even, 1 = down (floor), 2 = up (ceil), 3 = toward zero**. It does **not** affect `pck.f16.f32` (always truncates, below) or `pck.u8.f32 [scale]` (always nearest) |
| l21-27, h0-1, h19 | dst | r o pa sa g i ix | oracle |
| l7-13, l30-31, h17 | src1 | r o pa sa c g # i ix | oracle |
| l0-6, l28-29, h16 | src2 | same | oracle |

Both bits exist in every pck form, whatever the pair: the rules above say
when the vendor assembler sets them, not when they can be encoded. The text
calls l18 `rint` when both formats are f16 or f32 and `scale` otherwise (c10
counts as "otherwise"), and shows l19-20 as `rnd=`.

Notes:
- Two sources: the compiler packs two values per instruction (corpus:
  `pck.u8.f32 pa9, pa9, pa13 [dmask=6]` then `... [dmask=9]` builds RGBA8),
  and a one-value conversion passes #0 as src2. **hardware** (hwt_p4
  `pck_u8_dmask*`, and NOTES.md's color draws): the enabled bytes, from
  low to high, take src1, src2, src1, src2 (dmask 3 -> byte 0 src1, byte 1
  src2; dmask 9 -> byte 0 src1, byte 3 src2; dmask 15 -> bytes 0/2 src1,
  1/3 src2; a single enabled byte takes src1). Disabled bytes keep the old
  value. `pck.s16.f32 d, a, b` puts a in bits 0-15, b in 16-31.
- 16-bit integer halves, **hardware** (hwt_int): `pck.f32.s16 d, r.l2, #0`
  converts r's signed high half (0xfffe0003 -> -2.0), `.l0` the low one;
  `pck.f32.u16` the same unsigned (low 3.0, high 65534.0). `pck.u16.f32 d,
  a, b` puts a in bits 0-15 and b in 16-31, each rounded and clamped to
  0-65535 (-5.0 -> 0, 70000.0 -> 0xffff; `#0` as b leaves 0).
- Conversions, **hardware** (hwt_p4): f32 -> u8 `scale` is x*255 rounded to
  nearest and clamped to 0-255 (-0.1 -> 0, 1.5 -> 255, 0.7 -> 178, 1/510 ->
  1), whatever rnd says. f32 -> s16 rounds to nearest-even (2.5 -> 2, 1.7 ->
  2) and saturates (±70000 -> 0x7fff / 0x8000). **f32 -> f16 truncates**
  (toward zero: 1 + 3*2^-12 -> 1.0, -(1 + 3*2^-12) -> -1.0) for every rnd
  value, so a compiler wanting round-to-nearest F16 has to do it some other
  way (fmad16's own rounding was not tested). u8 -> f32: `.l<n>` is byte n;
  with `scale` the result is b/255 (0x99 -> 0.6), without it the integer.
- Immediates are 7 bits. The vendor assembler range-checks them as signed for
  some forms whose source is s8/o8/c10/s16 and unsigned otherwise (oracle);
  the bits are the same, so the text shows the raw 7-bit value.
- The corpus uses `pck.f16.f32`, `pck.f16.f16`, `pck.s16.f32`, `pck.u8.f32`,
  `pck.u8.f16`, `pck.c10.f32`, `pck.f32.f16`, `pck.f16.c10`, `pck.u8.u8` and
  `pck.c10.c10`. The other format pairs are oracle only.

### hw 09 `test`, hw 0f `testmask`

Any ALU op can be a test; the fields below pick the condition and test vs
testmask (oracle). Text:

    test.<alu>.<cond> pN, [dst,] src1, src2      [chan= mask= fmt f2_0 ...]
    testmask.<alu>.<cond>.<type> dst, src1, src2

| bits | field | values | tag |
|---|---|---|---|
| h27-31 | opcode | 9 = test, 0xf = testmask | oracle |
| l14-19 | alu | ALU op; l18-19 = group (0 float, 1 int16/32, 2 8-bit/fixed, 3 bitwise), l14-17 the op in the group. Table below | oracle |
| h10-11 | cond: sign test | 0 none, 1 sign bit set, 2 sign bit clear, 3 never emitted | oracle; meaning corpus; hardware (hwt_p2) |
| h8-9 | cond: zero test | 0 none, 1 zero, 2 nonzero, 3 never emitted | oracle; meaning corpus; hardware (hwt_p2) |
| h7 | cond: combine | 0 OR, 1 AND | oracle; meaning corpus; hardware (hwt_p2) |
| h4-6 | chan (test only) | 0-7 (8+ rejected); which channel's result is tested | oracle; corpus uses 0, and 2 for lowp c10 compares. **hardware** (hwt_p10, `test.fpsub8.lt` with fmt on three C10 channels): 0, 1, 2 = channel 0, 1, 2; 5 = any of them; 6 = all three; 7 = channel 0 or channel 2 (seen: true for TFF, FFT, TFT-like patterns, false for FTF); 3 and 4 never true here (3 is probably the alpha channel, equal in both operands). On an F16 compare only chan 0 tested the value (1-3 were false); on an f32 compare (hwt_p9, no fmt) chan 0, 5 and 7 gave the result and the others false |
| h2-3 | pdst (test only) | p0-p3 | oracle; hardware (hwt_p2) |
| h4-5 | type (testmask only) | 0 `byte`, 1 `word`, 2 `dword`; the vendor assembler produces no other value. The field's value 3 has no name and is written `.tm3`; it encodes as 3 | oracle; dword is the only one the corpus uses. **hardware** (hwt_p7): a passing f32 test writes 0x000000ff / 0x0000ffff / 0xffffffff for byte / word / dword, a failing one 0 |
| h12-15 | mask | write mask (the 3- and 4-component fdp tests set 7 and 0xf). The vendor assembler turns a testmask repeat count n into mask (1<<n)-1 (n <= 4); test rejects a repeat. A test with mask != 1 must write p0 | oracle; corpus: testmask mask=15 for bvec4 compares |
| l20 | dest written | set when the instruction has a destination (the generic dst slot), clear when it has none; then the dest field holds h0 + h19. The text shows that field as `nodst=v` when it isn't the default 0x280: v's bits 0-6 go to l21-27, 7-8 to h0-1, 9 to h19 | oracle; corpus: every `test` has no dest |
| h18 | fmt | set when a source is F16 (float group) or C10 (group-2 ops i8op5, fpadd8, fpsub8; a C10 dest counts there too). Sources and dest then have 6-bit numbers + bit 6 = per-operand format, printed `.f16.lo/.hi` / `.c10` | oracle; corpus (`test.fsub.ge p1, sa10.f16.lo, sa12.f16.lo`, `test.fpsub8.lt p0, pa4.c10, pa5.c10`). **hardware** (hwt_p10): an F16 operand's number counts halves like the float classes' (r0 = (3, 10), r1 = (5, 1), r2 = (7, 0.5): numbers 1 < 0 false, 2 < 1 true = 10 < 3, 5 < 10); a C10 operand is a whole register. The destination's numbering under fmt (testmask) was not drawn; the tools treat it like the sources |
| h21 | f2_0 | 1 bit | oracle; the corpus has it only on the counter test of fixed-count loops (loop4, lp_s4/s32/s100, lp_nest, tx_dep_loop): meaning unknown |
| h22, h23, h20, h24-26 | nosched, skipinv, syncs, pred | as above | oracle |
| dst / src1 / src2 | dst l21-27 h0-1 h19, src1, src2 in the generic slots. One-source ops (frcp ... fexp2, a plain move as fadd) get src2 = #0; fsubflr from a fract repeats src1 | oracle |

No negate/abs bits: the vendor assembler accepts only the format suffixes on
test sources (oracle); a subtract is the `fsub` ALU op.

#### ALU op field (l14-19)

| value | group | sub | mnemonic | tag |
|---|---|---|---|---|
| 0x00 | 0 | 0 | `fadd` | oracle (a + b; a plain move is a + #0); hardware (hwt_p2 `test.fadd.neg/pos`) |
| 0x03 | 0 | 3 | `fsubflr` | oracle |
| 0x04-0x07 | 0 | 4-7 | `frcp frsq flog2 fexp2` | oracle |
| 0x08 | 0 | 8 | `fdp` | oracle |
| 0x09, 0x0a | 0 | 9, 10 | `fmin fmax` | oracle |
| 0x0b, 0x0c | 0 | 11, 12 | `fdsx fdsy` | oracle |
| 0x0d | 0 | 13 | `fmul` | oracle |
| 0x0e | 0 | 14 | `fsub` | oracle, corpus; hardware (hwt_p2) |
| 0x16-0x1b | 1 | 6-11 | `iadd16 isub16 imul16 iaddu16 isubu16 imulu16` | oracle: outside a test the vendor assembler encodes these as ima16 a*1+b, a*1-b, a*b+0; signed/unsigned from the signed-immediate check is a **guess** |
| 0x1c, 0x1d | 1 | 12, 13 | `iadd32 iaddu32` | oracle (outside a test: imae a*1+b); names **guess** |
| 0x20-0x26 | 2 | 0-6 | `i8op0`..`i8op6` | oracle; outside a test i8op0-i8op3 encode as ima8 (add, or swapped-negated sub); i8op4 and i8op6 exist only as tests. Names unknown |
| 0x27 | 2 | 7 | `fpadd8` | oracle (test only, takes C10 operands); name **guess** |
| 0x28 | 2 | 8 | `fpsub8` | corpus: every lowp compare (fl_*_l) is fpsub8 with C10 sources; hardware (hwt_p10); name **guess** |
| 0x30-0x35, 0x37 | 3 | 0-5, 7 | `and or xor shl shr rol asr` | oracle; corpus (`test.and.ne p0, #0, #0` clears p0; `test.shl.pos/neg p2, x, #31` in step_l); hardware (hwt_p13 `test.and.eq` on a mask; hwt_int: `testmask.xor.eq/ne.dword`, `testmask.or.neg/pos/gt/le.dword` on the whole 32-bit result, its sign bit 31, register and `#0` sources; `test.xor.ne` into p0) |

Other values print as `alu<hex>`; the vendor assembler never produces them.

#### Conditions, and what GLSL produces

`cond` is printed by name for the six comparisons and the two plain sign
tests, else as `s<sign>z<zero><a|o>` (e.g. `s1z0o`). The test applies to the
ALU result, so for `fsub a, b` it compares a with b.

| sign | zero | comb | name | meaning on a-b | corpus |
|---|---|---|---|---|---|
| set | nonzero | AND | `lt` | a < b | fl_lt, ic_lt, if_lt, loop counters; `testmask` in lt, ic_ltv |
| set | zero | OR | `le` | a <= b | fl_le, ic_le, dc_gt (discard if a > b writes p1 = a <= b) |
| none | zero | AND | `eq` | a == b | fl_eq, ic_eq, if_eq, ic_eqv (testmask) |
| none | nonzero | AND | `ne` | a != b | fl_ne, ic_ne, dc_eq; `fadd.ne x, c48` = x != 0 (fl_ne0, cv_ftob, bool tests) |
| clear | nonzero | AND | `gt` | a > b | fl_gt, ic_gt, if_gt, bo_* (`greaterThan` via testmask) |
| clear | zero | OR | `ge` | a >= b | fl_ge, ic_ge, step (testmask) |
| set / clear | none | AND | `neg` / `pos` | sign bit | step_l (`shl` by 31 then sign test) |

**hardware** (hwt_p2 `test_*`, `pred_*`): all six comparisons give the
expected predicate on (1, 2), (2, 2), (2, 1) with `test.fsub.<cond> p0`
followed by `(p0) mov`; `test.fadd.neg/pos x, c48` on ±1 and 0 (pos(0) is
true); p1, p2, p3 are written and read by the predicate field, and `!p0` /
`!p1` invert.

So "sign clear" includes +0 (gt needs the extra nonzero), and "none" passes
under AND. The vendor assembler's own compare forms (gt, ge, eq, lt, le, ne)
encode as `test.fsub` with these conditions and no dest, and its compares
with zero as `test.fadd x, #0` with eq/ne (oracle). `if`/`discard`: the
vendor compiler writes p0 (branch) or p1 (discard, with the inverted
condition: keep the pixel when the test passes) (corpus), e.g. dc_lt0
`test.fadd.ge p1, pa4, c48`, dc_always `test.fadd.ne p1, c48, c48` (always
false).

#### test vs testmask

`test` writes predicate pdst from one channel (chan) of the ALU result, or
with mask > 1 one predicate per channel starting at p0. The vendor assembler
forces pdst = p0 then (oracle). **hardware** (hwt_gaps `test_mask15_p*`, open
stack, 2026-11-11): `test.fsub.lt p0, r0, r4 [mask=15]` set pn from channel n,
r0+n against r4+n (1, 5, 1, 5 < 3: p0, p2 set, p1, p3 clear). The ALU result
itself is also written when there is a dest (l20): **hardware** (hwt_gaps
`test_dst_alu_result`: `test.fsub.lt p0, r2, r0, r1` with 2.0, 3.0 left -1.0
in r2).

`testmask` has no predicate: it writes into dst, per enabled channel (mask), a
mask of the given width that is all ones when the test passes and zero
otherwise (corpus: every testmask is followed by `movc.nz.i32 d, d, c52, c48`
= select 1.0/0.0, e.g. step, lt, bo_xor, ic_eqv, and vo_if's `movc.z.i32`
select). **hardware** (hwt_p2 `testmask_*`): `testmask.fsub.lt.dword`
writes 0xffffffff when the test passes and 0 when it fails.

### hw 0a-0e: bitwise

| hw | h3 | mnemonic |
|---|---|---|
| 0a | 0 / 1 | `and` / `or` |
| 0b | 0 | `xor` |
| 0c | 0 / 1 | `shl` / `rol` |
| 0d | 0 / 1 | `shr` / `asr` |
| 0e | - | `rlp` |

**hardware** (hwt_p7 `bit_*`, r0 = 0xf0f0ff00, r1 = 0x3c3c0ff0): and, or,
xor, shl, shr (logical), asr (arithmetic), rol by an immediate, shr by a
register, `and #0xff00 [rot=16]` = & 0xff000000 and `and.inv #1` = &
~1 all give the expected words; a register shift by 33 gives 0 (the count
is not taken mod 32). hwt_int adds asr and shl by a register (0x80000010
asr 4 = 0xf8000001, 0x11 shl 31 = 0x80000000) and `and #0x3f80 [rot=16]`
on ~0 = 0x3f800000.

All **oracle**; no corpus instance (the vendor compiler uses these ALU ops
only under test). Mnemonic names are ours and match the test ALU codes;
rol and asr are **hardware** (above), rlp's semantics a **guess**.

| bits | field | values | tag |
|---|---|---|---|
| h27-31, h3 | opcode, sub-op | table above | oracle |
| h11 | inv (`.inv`) | src2 inverted (src1 takes no modifier) | oracle; hardware (hwt_p7 `and.inv #1`; hwt_int `and.inv` with a register src2, `xor.inv #0` = not) |
| h2 | partial | stops the immediate compression | oracle; "16-bit/partial operation" meaning **guess** |
| h6-10 | rot | immediate rotate-left count. The field exists whatever src2 is; with a register src2, the vendor assembler leaves it 0 | oracle; hardware (hwt_p7 `and #0xff00 [rot=16]`) |
| l0-6, l14-20, h4-5 | src2 immediate | when src2's bank is `#` (l28-29 = 2, h16): a 16-bit value, bits 0-6 / 7-13 / 14-15 | oracle |
| h21, h12-15 | rcnt, rpt/mask | as above | oracle |
| h18, h20, h22, h23, h24-26 | end, syncs, nosched, skipinv, pred | as above | oracle |
| dst / src1 / src2 | generic slots | oracle |

Immediate compression (oracle, checked for 32 values): the vendor assembler
stores a 32-bit immediate v (not rlp, not partial) as (n, rot, inv) with
v = rol(n, rot), n < 0x10000, smallest rot; if no rotation fits, the same
search runs on ~v and inv is toggled (so `and r, r, #0xfffffffe` is
`and.inv … #1`). Not representable -> assembler error. With partial, n = v & 0xffff,
no rotate. The text shows n, rot and inv separately (lossless):
`and r5, r8, #0xff00  [rot=16]` = r8 & 0xff000000.

`rlp dst, src1, pN` (hw 0e): the third operand must be a predicate (p0-p3)
-> l14-15 (oracle); src2's slot is encoded as unused (#0: l28-29 = 2, h16 = 1; field
`unused2` = 0x300: the src2 slot's number, bank and extension read as one
10-bit value, number in bits 0-6, bank in 7-8, extension in 9; the text
shows it only when it isn't 0x300, as `unused2=v`); h3, h4-11 never set. Rotate-left-through-predicate is a
**guess** from the name.

### Open questions

- f2_0 (test h21): only on fixed-trip-count loop counter tests (corpus).
  Meaning unknown.
- partial (bitwise h2): the vendor compiler never sets it (corpus).
- test chan 3, 4 and 7 (see the chan row: drawn, not explained); testmask
  type value 3; the sign/zero test value 3 (never emitted).
- group-2 ALU ops (0x20-0x26) names; whether the ops we call fpadd8/fpsub8
  are packed 8-bit add/subtract.
- rnd 1/2 = down/up is now hardware (hwt_p4); the pck `scale`/`rnd` bits on
  int->int pairs are decoded but the vendor assembler never sets them. Why
  rnd has no effect on f32 -> f16 (truncation) is open: maybe the F16
  rounding mode lives elsewhere (format control?).
- Immediate sign for pck sources is an assembler range check only.
## Integer, fixed-point and multi-unit ops: hw 07, 10-19, 1a

Forms: `hw_int.py`. Checks:
- `check_int.py`: an oracle sweep of 1602 cases, every field value of every form, 0 failures.
- `hwcheck.py`: all corpus words with hw 07 (104 distinct), 10, 11, 12, 14, 15, 18 and 19 decode with no unexplained bits and round-trip.

A crossover fuzz of the check cases also passed: 30000 encodes, 0 unexplained bits, 0 round-trip failures.

Evidence tags:
- **oracle**: a field's position and values, or a form's legality, from encoding through the vendor assembler and decoding the words.
- **oracle (not emitted for SGX545)**: the vendor assembler rejects the form for the SGX545 target but accepts it for other cores of the family. These forms were swept with the target's feature checks altered. The SGX545 has not been tested with them.
- **corpus**: a GLSL shader in c1/c2/c3 produces it.
- **hardware**: an hwt suite on the CE5300 board (see "Hardware checks").
- **capture**: seen in the vendor driver's programs in GPU memory.
- **unconfirmed**: not yet backed by any of the above.
- **guess**: not backed.

### Forms by class

| hw | forms |
|---|---|
| 07 | efo |
| 10 | sop2 |
| 11 | sop3 (h21=0, h10=0), sop3l (h21=0, h10=1), lrp1 (h21=1, h10=0), sop3x (h21=1, h10=1) |
| 12 | sopwm |
| 13 | ima8 |
| 19 | fpma |
| 14 | ima16 |
| 15 | imae |
| 18 | dot3 / dot4 / dot3s / dot4s |
| 16 | adif adifsum ssum16 ssum16i bilin op16s1 |
| 17 | firv; firh |
| 1a | ima32; op1adrc |

sop2, sop3, sop3l, lrp1, sop3x and the dot "s" forms carry an alpha operation (the coissue) in the same 64-bit word. The sop2, sop3, lrp1, sop3x and dot "s" forms require the coissue: the assembler rejects them without it. For dot it is optional (**oracle**).

The assembler rejects ssum16i, firh, op16s1 and op1adrc for the SGX545. They are tagged **oracle (not emitted for SGX545)**.

### Fields shared by hw 10-19 and 1a (oracle)

| bits | field | source |
|---|---|---|
| h27-31 | opcode | constant |
| h25-26 | pred, short form (none, p0, p1, !p0) | the other predicates are rejected |
| h23 | skipinv | |
| h22 | nosched | |
| h18 | end | |
| h12-14 | rpt - 1 (1..8) | sop3 / sopwm have no repeat |

The assembler rejects sync on these classes. It takes sop3 and sopwm only with a one-channel instruction mask; the other forms ignore the mask.

Operand slots are the generic ones ("Operands"):
- dst: l21-27, bank h0-1, ext h19.
- src0: l14-20, bank h2. With no ext it takes r/pa only; an internal register becomes r124+n.
- src1: l7-13, bank l30-31, ext h17 (r o pa sa, ix, c/g, #, i).
- src2: l0-6, bank l28-29, ext h16.
- An index operand number is [bank 2][idx 1][offset 4].

**Operand bit 6.** In sop2/sop3/sopwm/dot/ima8/fpma, while format control's C10 enable is on (`setfc`'s second argument, on at program start; **hardware**, hwt_p6), every number is 6 bits. Bit 6 is then the per-operand format flag (C10 vs U8). This covers r, o, pa, sa and i operands only; indexed, c/g, immediate and idx operands keep their 7-bit fields ("Encoding conventions"). The integer classes not named here (ima16, imae, hw 16, 17, 1a) never read format control.

In efo the same happens for F16 sources. There is no instruction bit that records this; dis.py tracks `setfc` per code block (hw.CTX). **hardware** (hwt_p7, hwt_p8): efo's F16 select is `setfc`'s **first** argument and is **off at program start**; with it on, an F16 operand's number counts halves like the float classes' (`efo r3, r0.f16.hi, r1, c52 [d=a0 a=a0=s0+s2]` with r0 = (3, 2) gives 3.0 = 2 + 1), and the result is f32. With it off the same word reads r64 (per-pixel garbage).

All corpus sop/dot/fpma words come from programs with the C10 enable on. So a backend that wants r64-r127 in these classes must turn the enable off with `setfc`, and then can't use per-operand C10 in them.

Select names (zero, one, asat, s0-s2, s0a-s2a, k18/k19, the x1..x8 scales, add/sub/min/max, u8/s8/o8, u16/s16/u32) are our labels. Each form's table gives the stored values. `one` is stored as `zero` with the complement bit set (**oracle**).

In the forms, a select field's spec ends with its complement bit. Complemented values print as `1-x` (`one`/`zero` for 1-zero/1-one).

### efo (hw 07)

The control fields' meanings are from hardware (hwt_p12, below) except where tagged otherwise.

Units:
- M0, M1: multipliers.
- A0, A1: adders.
- I0, I1: the internal registers they feed back into.

| bits | field | values | tag |
|---|---|---|---|
| h24-25 | pred (short form) | | oracle |
| h23 | skipinv | | oracle |
| h11 | nosched | | oracle |
| h12-13 | rpt-1 (1..4) | | oracle |
| h20-21 | d: what the dest gets | 0 i0, 1 i1, 2 a0, 3 a1 | oracle; corpus; hardware (hwt_p12) |
| h18-19 | i: internal writes | 0 i0=a0 i1=a1; 1 i0=a1 i1=a0; 2 i0=m0 i1=m1; 3 i0=a0 i1=m1 | oracle; hardware (hwt_p12) |
| h16-17 | a: adder inputs | 0 a0=m0+m1 a1=i1+i0; 1 a0=m0+s0 a1=i1+i0; 2 a0=m0+i0 a1=i1+m1; 3 a0=s0+s2 a1=s1+s2 | oracle; hardware (hwt_p12) |
| h14-15 | m: multiplier inputs | 0 m0=s0*s2 m1=s1*s2; 1 m0=s2*s1 m1=s2*s2; 2 m0=s1*s0 m1=s2*s2; 3 m0=s1*i0 m1=s2*i1 | oracle; hardware (hwt_p12) |
| h10 / h9 | wi0 / wi1: write I0 / I1 | | oracle; hardware (hwt_p12) |
| h26 | a0rneg: negate A0's right input | | oracle; corpus `mix`: m0=-x*a, a0=m0-(-x) = x(1-a); hardware (hwt_p12) |
| h22 | a0lneg | set for an add's other source negate (**unconfirmed**) | oracle; name **guess**; hardware (hwt_p12): no effect with `a=a0=s0+s2` |
| h8 | f3_10 | | oracle; meaning unknown |
| h5 h4 h3 | src0/src1/src2 negate | | oracle; corpus (`not()`) |

efo has no end bit (h18-19 is `i`), so it can't be a program's last instruction.

**hardware** (hwt_p12, 2026-11-08), with s0 = 2, s1 = 3, s2 = 5 and
I0 = 7, I1 = 17 beforehand. The unit wiring in the table above holds:
- every `m` mode: mode 2's m1 is `s2 * s2`, and mode 3 reads I0/I1 as they
  were before the word;
- every `a` and `i` mode, and each `d`.
- Without `wi0`/`wi1` the internal registers keep their values.
- `a0rneg` negates A0's right input (s0 - s2 = -3).
- `a0lneg` made no difference with `a=a0=s0+s2`.
- Chaining works as v_mvp's does: an efo with `a=a0=m0+i0` reads the I0 the
  previous efo wrote (10 + 10 = 20).
- **Hazard:** `d=i0` while the same word writes I0 (`wi0`) gave the old value
  on some pixels and the new one on others: don't make `d` name I0 while the
  word writes it.
- `d=i1` with `wi1` read the old I1 uniformly. The vendor driver's programs
  rely on it (capture): in the DDK's Minecraft terrain vertex program
  (`mc-ddk-vertex.md`) the first efo of each `mat4 * v` chain stores the
  previous chain's second result with `d=i1` while its `i=i0=m0,i1=m1` starts
  the new chain, so a chain needs no trailing `mov oN, i1` except the last.
  The previous chain's last efo is then `nosched` too (I1 must survive to the
  next word).

Operands (**oracle**):
- dst: l21-27, bank h0-1 (r o pa ix), no ext.
- src0: l14-20, bank h2, ext h6 (r pa o sa).
- src1: l7-13, bank l30-31, no ext (r o pa sa).
- src2: l0-6, bank l28-29, ext h7 (all kinds).
- Internal registers in the dest, src0 and src1 slots become r124+n.

Earlier cores' EFO use h3-8 as three 2-bit source modifiers, and their control fields differ too (**unconfirmed**). This core uses the h5/h4/h3 + ext layout.

Corpus control words and their GLSL:

| ctl | decoded | shaders |
|---|---|---|
| 0x2b0600 | d=a0, i=m0/m1, a=s0+s2/s1+s2, m=s0*s2/s1*s2 | exp log cos norm3 kmul_*: two mads at once (dst = s0+s2, I0/I1 = the products) |
| 0x230600 | d=a0, i=a0/a1, a=s0+s2/s1+s2 | ftoi and int compares: x+1 for two lanes (`efo pa2, pa8, pa9, c52` then movc.ltz); bo_not: 1-x |
| 0x180600, 0x20600, 0x220200 | m products into I, then a0=m0+i0, a1=i1+m1 | v_mvp: dot-product accumulation across 3 efos |
| 0x42d0600 / 0x43d0600 / 0x14600 | a0rneg | mix, fl_vsel: x(1-a) + y*a |
| 0x200000 | d=a0, a0=m0+m1, no I writes | ix_loop, vix_loop |
| 0x2b8600 | m mode 2 | rv_aa |

### sop2 (hw 10)

The assembler requires the coissue. `dst.rgb = (src1 * c1) cop (src2 * c2)` and `dst.a = (src1.a * a1) aop (src2.a * a2)` (**hardware**, hwt_p6, below). Operands are dst, src1 and src2; there is no src0.

| bits | field | source | tag |
|---|---|---|---|
| h6-8, h24 | c1: 0 zero 1 s1 2 s2 3 s1a 4 s2a 5 asat 6 k18; h24 complement | | oracle; hardware (hwt_p6: zero, one, s2) |
| h3-5, h15 | c2: same, 6 = k19 | | oracle; hardware (hwt_p6: zero, one) |
| l18-19 | cop: 0 add, 1 sub, 2 min, 3 max | | oracle; hardware (hwt_p6: add, sub) |
| l20 | cs1c: complement src1 (color) | | oracle |
| h20-21, h11 | a1: zero s1a s2a k18 + complement | | oracle; hardware (hwt_p6: zero, s2a) |
| h9-10, h2 | a2: zero s1a s2a k19 + complement | | oracle; hardware (hwt_p6: one) |
| l16-17 | aop: 0 add, 1 sub, 2 min, 3 max | | oracle |
| l15 | as1c: complement src1 (alpha) | | oracle |
| l14 | aneg: negate alpha result | | oracle |

The only corpus use is `mov_l`/`fl_gt_l`: `sop2 d, a, b [c1=one c2=zero a1=zero a2=one]`, a lowp select or move.

**hardware** (hwt_p6), U8 operands and destination: `[c1=one c2=zero
a1=zero a2=one]` gives rgb = src1's bytes 0-2, alpha = src2's byte 3
(0x11335599, 0xaabbccdd -> 0xaa335599); `c1=one c2=one cop=add` is a
per-byte saturating add, `cop=sub` a per-byte subtract clamped at 0, and
`c1=s2 c2=zero a1=s2a` a per-byte multiply a*b/255 (0x80 * 0x40 -> 0x20).
So the SOP2 formula above holds. C10 sources into a U8 destination: each
channel (v/255) is clamped to a byte (0x100 -> 0xff, 0x080 -> 0x80; C10
0x3ff = -1/255 subtracts 1); channels 0, 1, 2 go to bytes 0, 1, 2 (hwt_p8
`sop2_c10_bytes`: (0x011, 0x022, 0x044) -> 0x00442211). The **alpha of a
32-bit C10 source is its channel 0** (hwt_p7/p8: bits 30-31 make no
difference; r1 channel 0 = 0x010 -> alpha byte 0x10). A C10 destination
(`sop2 r3.c10, ...`) gets the three 10-bit channels at bits 0-29 (0x04020100
for (0x100, 0x080, 0x040)), bits 30-31 zero.

### sopwm (hw 12)

`dst = (src1 * c1) cop (src2 * c2)` per byte, with a write mask. There is no coissue and no repeat; the assembler takes only a one-channel instruction mask.

| bits | field | source | tag |
|---|---|---|---|
| h12,h13,h14,h11 | mask, in order x y z w (stored rotated); default 15 | a partial mask needs a non-indexed dest | oracle |
| h6-8, h24 | c1: 0 zero 1 asat 2 s1 3 s1a 6 s2 7 s2a (4, 5 unused) + complement | | oracle |
| h3-5, h15 | c2: same | | oracle |
| h20-21 | cop: 0 add, 1 sub, 2 min, 3 max | | oracle |
| h9-10 | aop: same values | | oracle |

h2 and l14-20 are unused. Corpus: lowp add/sub/min/max (`sopwm d, a, b [c1=one c2=one cop=sub aop=sub]`) and lowp mix (`[c1=s1a c2=1-s1a]`, add, add).

### sop3 family (hw 11)

All forms take dst, src0 (r/pa), src1 and src2. cop and aop are 0 add, 1 sub in every form of the family. Selects for c1/c2 (h6-8 + h24, h3-5 + h15) are 0 zero, 1 asat, 2 s1, 3 s1a, 4 s0, 5 s0a, 6 s2, 7 s2a (**oracle**). h21 picks the color op and h10 the coissued alpha op.

**sop3** (h21=0, h10=0; alpha op asop, or arsop with arev):

| bits | field | tag |
|---|---|---|
| h20 | cop: 0 add, 1 sub | oracle |
| h12-13, h14 | a: alpha select zero s0a s1a s2a + complement | oracle |
| h9 | aop: 0 add, 1 sub | oracle |
| h11 | arev: arsop instead of asop | oracle. Meaning **unconfirmed**: it negates the alpha result. In asop the other alpha select is c2's alpha; arsop swaps the roles (b - a) |

**sop3l** (h21=0, h10=1; alpha op alrp):

| bits | field | tag |
|---|---|---|
| h20 | cop | oracle |
| h13, h9 | a1: zero / s1a + complement | oracle |
| h12, h14 | a2: zero / s2a + complement | oracle |
| h11 | cneg: color result negate; only allowed with alrp | oracle |

**lrp1** (h21=1, h10=0; alpha op asop):

| bits | field | tag |
|---|---|---|
| h13, h20 | l1: zero / s1 + complement | oracle |
| h12, h14 | l2: zero / s2 + complement | oracle |
| h11 | cs: s0 / s0a | oracle |
| c1/c2 fields | the coissued asop's two selects, with the family's c1/c2 select values above | oracle |
| h9 | aop | oracle |

Color = lerp(l1, l2, cs) is a **guess** from the name.

**sop3x** (h21=1, h10=1; alpha op alrp): c1 and c2 as above, plus the alrp alpha (a1, a2 as in sop3l).

h11 and h20 (field p, h11 the low bit) encode a pair of selects: 0 `s1.s2`, 1 `s1.1-s2`, 2 `one.s2`, 3 `1-s1.s2`. The pair 1-s1, 1-s2 is rejected (**oracle**). The name and the meaning are **guess**; no corpus word is a sop3x.

Corpus: lowp `mul_l`/`div_l`/`pow_l`/`mad_l` are `sop3l d, x, y, z [c1=s0 c2=zero a2=s2a]`. `mod_l` uses `sop3 [cop=sub c1=one c2=s0 a=one aop=sub]`.

### dot3 / dot4 (hw 18)

| bits | field | source | tag |
|---|---|---|---|
| h24 | 4-vector (dot4) | | oracle |
| h6 | f6 | | oracle; meaning unknown |
| h7-8 | cs: color scale x1 x2 x4 x8 | | oracle |
| h11 / h15 | src1 / src2 complement | | oracle |
| h5 | src0 present: coissue (forms dot3s/dot4s) | | oracle |
| h4 | f4 | | oracle; meaning unknown |
| h20-21 (+h2 when h5=0) | a1: zero s1a s2a, 3 = x3 (+ complement) | | oracle |
| h9-10, h3 | a2: zero s1a s2a, 3 = x3 + complement | with no coissue the vendor tools write 3, but the field holds any value | oracle |
| l16-17 (h5=0) | as: alpha scale | | oracle |
| l15 (h5=0) | s1ac: complement of the alpha op's fixed src1 alpha | | oracle |
| l14-20 (h5=1) | src0 (r/pa) | | oracle |

Corpus: dp3_l/dp4_l/norm3_l use `dot3/dot4 d, a, b [a1=zero a2=x3]`, the lowp dot product with no alpha op.

### ima8 (hw 13) / fpma (hw 19)

`dst = c0 * c1 + c2` per channel and `alpha = a0 * s1a + s2a`. Operands: dst, src0 (r/pa, negate h3), src1, src2.

| bits | field | source | tag |
|---|---|---|---|
| h20-21, h10 | c0: s0 s1 s0a s1a + complement | | oracle |
| h5, h24 | c1: s1 / s1a + complement | | oracle |
| h4, h15 | c2: s2 / s2a + complement | | oracle |
| h9, h6 | a0: s0a / s1a + complement | | oracle |
| h7 / h8 | a1c / a2c: complement s1a / s2a | | oracle |
| h11 | sat | | oracle; name **guess** |

Corpus: lowp `mad_l`/`sin_l` use `fpma`.

### ima16 (hw 14)

`dst = src0 * src1 + src2`, 16-bit. src0 is r/pa only.

| bits | field | source | tag |
|---|---|---|---|
| h11 | sgn (.sgn) | | oracle; corpus |
| h10 | sat | | oracle; name **guess** |
| h3-4 | f2_15 | 0..3 | oracle; meaning unknown |
| h24 | dflag1 | | oracle; meaning unknown |
| h6-7 / h8-9 | f1 / f2: src1 / src2 format: 0 16-bit, 1 u8, 2 s8, 3 o8 | | oracle; names **guess** |
| h20 / h5 | s1hi / s2hi: byte select for 8-bit formats | the assembler requires a high or low choice for a register | oracle |
| h21 | src2 negate | | oracle |

Immediates are 7-bit signed unless the format is u8 (**oracle**). The vendor compiler uses u8 exactly when the source is an immediate (**unconfirmed**). Corpus: indexing, e.g. `ima16.sgn idx1, r0, #4, #0 [f1=u8 f2=u8]` scales an array index into an index register.

### imae (hw 15)

`dst = src0 * src1 + src2`, 16x16 + 32 with carry. src0 is r/pa only.

| bits | field | source | tag |
|---|---|---|---|
| h11 | sgn | | oracle; corpus |
| h10 | sat | | oracle; name **guess** |
| h3-5 | f2_15 | 0..7 | oracle; unknown |
| h6-7 | t2: src2 type u16 / s16 / u32 | | oracle; corpus (u32) |
| h24 / h21 / h20 | s0hi / s1hi / s2hi: high 16 bits of src0/1/2 (the assembler requires a high or low choice, or an immediate; a u32 src2 takes neither) | | oracle |
| h8 | cout | | oracle; name **guess** |
| h9 | cin | | oracle; name **guess** |
| h15 | ci: carry internal register i0/i1 (when cin or cout) | | oracle |

src1 immediates are signed when sgn is set. src2 immediates are signed whenever `t2` isn't 0: s16, u32 and the unnamed 3 (`t2=x3`) (**oracle**). So `imae o72, pa36, r58, #-63 [t2=u32]` is legal, and the 7-bit number holds -63 in two's complement. Corpus: dynamic array addressing, `imae.sgn pa0, pa1, #16, sa9 [t2=u32]`.

### hw 16 / 17 / 1a (no corpus words; oracle or oracle (not emitted for SGX545))

The common fields are as above. firv and op16s1 have only h12-13 for the repeat (1..4); firh accepts 1..6. The names are our labels; apart from ima32 they are **guesses**.

**adif / adifsum** (16, h20-21=0):
- h6 = 1 makes it adifsum, with src0 (r/pa). adif has no src0 (l14-20 and h2 are 0).
- h15 = s0f13 (meaning unknown).
- Probably |src1 - src2| (+ src0) (**guess**).

Operand slots: the generic dst, src0 (bank h2, no extension), src1 and src2 unless an entry says otherwise. `fmt` (h8-9) in bilin, ssum16, firv and firh: 0 u8, 1 s8, 2 o8 (ssum16i is the separate form with h8-9 = 3). It is written as a mnemonic suffix, `bilin.s8`, `ssum16.o8`, `firh.s8`; u8 has none. `[fmt=s8]` in the brackets means the same. Every field prints as its stored value.

**op16s1** (16, h20-21=1; oracle (not emitted for SGX545)):
- f2_15 h3-6.
- s8 h7.
- x38 h8.
- x1c h9.
- x39 h10-11 (0-2).
- i h15 (i0/i1).
- The operation is unknown.

**bilin** (16, h20-21=2):
- fmt h8-9 u8/s8/o8 (s8/o8 make immediates signed).
- ileave h24.
- src23 h11.
- dst23 h10.
- rnd h15.
- The names are **guess**.

**ssum16** (16, h20-21=3):
- src0 has no bank bits: it is read in src1's bank (the assembler requires src0 and src1 of the same r/o/pa/sa kind), and src1 has no ext.
- fmt h8-9.
- x1b h24.
- x1c h7.
- x1d h10.
- nf16 h15 (the text prints `nf16` when the bit is set).

**ssum16i** (h8-9 = 3; oracle (not emitted for SGX545)): src0 is an internal register, its number written raw in l14-20 with no bank bits; there is no src1 (l7-13, l30-31 are 0).

**firv** (17, h2=0):
- src0 is read in src1's bank: its number in l14-20 with no bank bits (as ssum16).
- fmt h8-9.
- a5 h6-7 (immediate 0-2).
- coef h3-5 (0-7).
- shift: 5-bit two's complement, bits 0-1 in h10-11, 2-3 in h14-15, 4 in h24 (printed raw 0-31).
- a8 h20-21 (0-3).
- The destination is the generic dst slot (g<n> as everywhere: number + 0x40 on ext bank 1).

**firh** (h2=1; oracle (not emitted for SGX545)): repeat count - 1 in h12-14. The dest is an internal register, its number written raw in l21-27 with no bank bits. Shift bits 2-3 move to h0-1 (bits 0-1 stay in h10-11, bit 4 in h24). h19 = f2_27 (meaning unknown).

**ima32** (1a, h20=0): `dst = src0*src1 + src2`, 32-bit.
- sgn h9 (it also makes immediates signed).
- hi h4-6: 0 none, 4 i0, 5 i1, 6 i2. The high 32 bits go there: **hardware**
  (hwt_gaps `ima32_hi_*`, open stack, 2026-11-11: 0x10000 * 0x10000 gave 0 in
  the destination and 1 in i1).
- cin h10-11: carry-in from i0-i2: 0 none, 1 i0, 2 i1, 3 i2.
- negp h8 = src0 neg xor src1 neg.
- src2 neg h7.
- **hardware** (hwt_int, open stack, 2026-11-11): the destination is the
  low 32 bits of `src0 * src1 + src2`, wrapping (0x7fffffff + 1 =
  0x80000000, 0xffffffff + 2 = 1, 0x12345678 * 0x9abcdef = 0xe242d208,
  -3 * 5 = -15); src2 negate subtracts (`ima32 d, a, #1, -b`: 5 - 7 = -2,
  0x80000000 - 1 = 0x7fffffff), on an immediate too (`-#7`); a 7-bit
  immediate without `.sgn` is unsigned (`#100`); `a * c56` (~0) is -a.
  The high word in i1 is the unsigned product's (0xffffffff² gives
  0xfffffffe) and, with `.sgn`, the signed one's (-1 * -1 gives 0, -2 * 3
  gives 0xffffffff).
- src0 r/pa/o/sa via ext h15.
- The assembler writes isub32 (signed or unsigned) as `a*#1 + -b`.

**op1adrc** (1a, h20=1; oracle (not emitted for SGX545)):
- dest r/pa (h7), raw number.
- drc h0 (0-1).
- Sources src1, src2. The operation is unknown; the DRC operand suggests a long-latency op.

### What GLSL integer code compiles to (c3 corpus, ef_c3.txt)

The vendor compiler has no integer path for GLSL ES 1.0 `int`/`bool` (**corpus**). They are floats:
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
  - 32-bit: `ima32` (signed/unsigned via h9; the high word to an internal register; carry-in from i0-i2). `isub32` = `ima32 d, a, #1, -b`.
  - 16-bit: `ima16` (iadd16 `a*#1+b`, isub16 `a*#1-b`, imul16 `a*b+#0`, each signed or unsigned).
  - `imae` = 16x16 + 32 with carry in/out through an internal register (iadd32 = `a*#1+b` with a u32 src2).
- **compares**: `test` with integer ALU ops (hw 09: `test.isub16.<cond>`, `test.iadd32.<cond>`, see notes_pack).
- **shifts and logic**: hw 0a `and`/`or`, 0b `xor`, 0c `shl`/`rol`, 0d `shr`/`asr` (hw_pack.py / notes_pack). There is also `test.and/shl...` for flag tests.
- **conversion**:
  - float to int: `pck.s16.f32`, `pck.u16.f32`, `pck.s8.f32`, ...
  - int to float: `pck.f32.s16`, `pck.f32.u16`, ...
  - pck has no 32-bit integer format. A 32-bit conversion needs two halves, or the float path above.
- **division**: there is no hardware integer divide; use frcp + multiply + fix-up. op1adrc is unknown and not emitted for SGX545.

### Open questions

- **efo**:
  - h22 (a0lneg) is a name guess, and its effect is unconfirmed.
  - h8 (f3_10) is unknown.
- **sop3x**: its operation.
- **dot**: f6 (h6), f4 (h4) and a1/a2 = 3 (x3) are unknown.
- **sat** (h11 in ima8/fpma, h10 in ima16/imae) is a name guess.
- ima16/imae **f2_15** and ima16 **dflag1** are unknown.
- **hw 16/17/1a**: the x1b/x1c/x1d and x38/x39 fields, coef, drc, the adifsum and ssum16 h15 flags (s0f13, nf16), firv a5/a8, firh f2_27, the operations of op16s1 and op1adrc, and whether the SGX545 runs ssum16i, firh, op16s1 and op1adrc at all.
## smp, ld/st, and the special class (hw 1c, 1d, 1e, 1f)

Forms: `hw_mem.py`. Check: `check_mem.py` (about 290 directed oracle cases, every
corpus word of these classes, and a mutation sweep: about 13,000 instructions
the vendor assembler accepts, made by changing one input at a time (a flag, or
an operand's type, number, flags or index) from a valid instruction of each
form. All of them decode with no unexplained bit and round-trip through the text).
`hwcheck.py` reports no hw 1c-1f word without a form, no unexplained bits and
no round-trip failures.

Evidence tags are as defined in "How each fact is established".

What the vendor assembler allows for the SGX545 in these classes (all
**oracle**):

- smp: the mask / repeat field (h12-15, h21) exists, but only write mask 1
  and no repeat are accepted.
- smp: PCF (h5/h6) is accepted.
- smp: h14/h15 (rdata / rinfo) are accepted.
- ld/st: eld exists (hw 1d with h22 = 1); nosched on ld is rejected.
- ld/st: the cache bits h1/h19 are accepted.
- st: h0 (f3_13) is accepted.
- ld/st: preinc/postinc are not accepted with address mode l.
- br/ba: extended sync-end (xsyncend, h12) is accepted.
- br/ba: h10 (f1_22) is accepted; op0xd7 takes n below 16.
- br/ba and phas.i: 18-bit targets.
- lock/release: n below 8.
- MOE control: the register forms (h19) are accepted.
- emit: the t1 and t3 targets and a 3-bit n are accepted; emit.t0 is accepted.
- op0x98, nop's l1 (f3_5), phas / phas.i, op0xfd and op0xfe-0x100 are
  accepted.

Encoding every vendor assembler opcode with generic operands finds no form in
hw 1c-1f other than the ones below (oracle).

---------------------------------------------------------------------------

### hw 1c: smp

`smp.<dim>[.rdata|.rinfo][.pcf|.pcf16][.f16|.c10][.bias|.lod|.grad] dst, coord, state[, lod]  [drc=n ...]`

Operands: dest, coordinate, texture state, [LOD source], DRC (0/1).

| bits | field | values | evidence |
|---|---|---|---|
| h27-31 | class | 0x1c | oracle |
| h24-26 | pred | 3-bit predicate (PRED3) | oracle |
| h23 | skipinv | | oracle |
| h22 | nosched | not h11 as in the ALU classes | oracle |
| h21 | rcnt | set with a repeat count; the SGX545 target rejects any repeat | oracle |
| h20 | syncs | sync-start | oracle |
| h12-15 | mask, or rpt - 1 when h21 is set | write mask (text `mask=`, default 1); the vendor assembler accepts only mask 1 and no repeat for the SGX545, so the vendor compiler's samples always have h12 set, but the field holds any value. `ret` below is ORed into bits 14-15 of the same field | oracle |
| h14 | ret = 1: `.rdata` | | oracle; name guess (raw sample data) |
| h15 | ret = 2: `.rinfo` | h14 and h15 together are rejected | oracle; name guess (sample info) |
| h10-11 | dim | 0 1D (`smp.1d`), 1 2D (`smp.2d`), 2 3D, cube maps (`smp.3d`) | oracle, corpus (2D for texture2D, 3D for samplerCube); exercised by the open stack (dEQP-GLES2) |
| h8-9 | lod | 0 none, 1 bias, 2 replace (texture2DLod), 3 gradients | oracle, corpus (bias, lod, grad seen in ef_c3); exercised by the open stack (dEQP-GLES2) |
| h7 | dst bank | 0 r, 1 pa (no other destination) | oracle |
| h5 | pcf | percentage-closer compare, F32 coordinates | oracle (accepted only with an F32 coordinate); meaning **unconfirmed** |
| h6 | pcf16 | the same with F16 coordinates. Not with C10 coordinates | oracle; meaning **unconfirmed** |
| h3 | f16 | coordinate is F16 | oracle, corpus (mediump tx_*_m); exercised by the open stack (dEQP-GLES2) |
| h4 | c10 | coordinate is C10 | oracle, corpus (lowp tx_*_l); exercised by the open stack (dEQP-GLES2) |
| h2, h18 | coord bank | r 00, pa h2, o h18, sa h2+h18. Internal registers become r124+n; no indexing, no constants/immediates | oracle |
| l14-20 | coord | first register of the coordinate | oracle |
| l7-13, l30-31, h17 | state | src1 slot, any source kind (r o pa sa c g i # indexed) | oracle |
| l0-6, l28-29, h16 | lod source | src2 slot. With lod 0 it holds the unused-source pattern (#0: l29 + h16) | oracle |
| l21-27 | dst | first result register | oracle |
| h0 | drc | dependent-read counter the result signals (0/1); a later `wdf drc` waits on it | oracle, corpus (ef_c3 tx_dep*); exercised by the open stack (dEQP-GLES2) |
| h1 | - | never set by the vendor assembler | oracle |

Result format: nothing in the word selects it (oracle); the texture state
(src1) does. **hardware** (hwt_smp, open stack, CE5300 board, 2026-11-11): the same
dependent `smp.2d r0, r8, r4` on an RGBA8888 texel (bytes 0x10, 0x20, 0x40,
0x80) returned one register, 0x80102040 (BGRA byte order, as T1 format 0x0d
gives it), with T3 = 0x00800800. It returned two registers with T3 =
0x01028800: r0 = 0x30042c04 (R lo, G hi as F16) and r1 = 0x38043404 (B lo,
A hi), the non-dependent F16 return's layout. The `state` operand is a placeholder in the USP
binary: dependent `smp` words sit in USP sample blocks, which the patcher
turns into code with the real state registers at finalize time (capture;
ef_c3 logs them with `#0`). Projective reads have no smp bit: the compiler
divides the coordinate first (corpus, tx_dep_proj*).

### hw 1d: ld, h22 = 0

`ld.<a|l|t>.<b|w|d> dst, base, off[, range]  [drc=n preinc postinc range moe rpt=n ...]`

Operands: dest, base, offset, range (only with the range flag h6; otherwise a
placeholder), DRC.

| bits | field | values | evidence |
|---|---|---|---|
| h27-31 | class | 0x1d | oracle |
| h24-26 | pred | PRED3 | oracle, corpus (the `(p0) ld.a.d` of every secondary program) |
| h23 | skipinv | | oracle, corpus |
| h22 | 0 | the vendor assembler rejects nosched on loads; h22 = 1 is eld | oracle |
| h21 | moe | set by default (so the vendor compiler's loads nearly always have it): a repeated load steps its operands by the MOE increments (same bit as the ALU classes). The text prints `moe` when it is set | oracle; meaning by analogy (guess) |
| h20 | syncs | | oracle |
| h19 | f2_3 | one assembler option sets h19 and h1 together | oracle; data-cache control (guess) |
| h18, h2 | base bank | src0 slot: r, pa (h2), o (h18), sa (h2+h18) | oracle, corpus (base sa1 = local memory base) |
| h17, h16 | src exts | of offset / range | oracle |
| h12-15 | rpt | repeat count - 1 (1-16) | oracle, corpus (st rpt=16) |
| h10-11 | am | 0 "a" (the secondary program's uniform loads), 1 "l" (spills, temp arrays), 2 "t" (the offset must be a register; no pre/post flags) | oracle, corpus (a, l); exercised by the open stack (a, l, t; dEQP-GLES2); names a/l/t guess (absolute / local / tiled) |
| h8 | preinc | | oracle; name guess |
| h9 | postinc | not both | oracle; name guess |
| h7 | dst bank | 0 r, 1 pa | oracle |
| h6 | range | src2 holds a 4th source | oracle; name guess |
| h4-5 | size | 0 d (dword), 1 w, 2 b, 3 `q` (the vendor assembler rejects it, but the field holds it; the text is `ld.<mode>.q` and it encodes as 3) | oracle, corpus (d); **hardware** (hwt_gaps `ld_w_*`, `ld_b_*`, open stack, 2026-11-11): 1 is a halfword, 2 a byte, each zero-extended (see Address) |
| h3 | f2_9 | | oracle; unknown |
| h1 | f2_2 | | oracle; data-cache control (guess) |
| h0 | drc | DRC the loaded data signals | oracle, corpus (ix_tmp*: loads alternate drc 0/1, each followed later by `wdf`) |
| l21-27 | dst | | oracle |
| l14-20 | base | | oracle |
| l7-13, l30-31 | off | any source kind, including #0-127 and indexed | oracle, corpus |
| l0-6, l28-29 | range | without h6: the unused pattern | oracle |

**Address** (hardware, `tools/iso` sanity_st and p2_ld_ukcode
2026-11-02): `ld.a.d` and `st.a.d` with an immediate offset `#i` access the
dword at base + 4·(i + 1). `st.a.d sa0, #0` wrote word 1 of the buffer whose
address was in sa0, `#2` wrote word 3, and `ld.a.d #2` read word 3 back.
Microkernel code read with `#0`-`#7` lands as instruction pairs (low word,
then the high word with the class) only under that reading: `wdf`'s high word
0xf9200000 came from an odd word. The other sizes follow the same rule in
their own unit (**hardware**, hwt_gaps, open stack, 2026-11-11, a buffer of
bytes 0x00, 0x01, ...): `ld.a.w #i` reads the halfword at base + 2·(i + 1)
into the low half, zero-extended (#0 -> 0x0302, #3 -> 0x0908); `ld.a.b #i`
reads the byte at base + (i + 1), zero-extended (#0 -> 0x01, #5 -> 0x06).
`ld.a.d #0`/`#2` read words 1 and 3 again. Untested: register offsets, modes
l/t, and offsets past #127.

### hw 1d: eld, h22 = 1

`eld.<d|q> dst, src1, src0, #imm11, src2  [drc=n ...]` (present on the
SGX545; oracle).

| bits | field | values | evidence |
|---|---|---|---|
| h24-26 | pred | PRED3 | oracle |
| h23 | skipinv | | oracle |
| h21 | f2_3 | the bit ld has at h19 | oracle |
| h20 | syncs | | oracle |
| h12-15 | rpt | count - 1 | oracle |
| h5-11, h16-19 | imm | 11-bit immediate: low 7 bits h5-11, high 4 h16-19, so no source has an extension bit (r/pa/o/sa only) | oracle |
| h4 | dst bank | r / pa | oracle |
| h3 | size | 0 d, 1 q | oracle; what each size loads is **unconfirmed** |
| h2 | src0 bank | r / pa | oracle |
| h1 | f2_2 | | oracle |
| h0 | drc | | oracle |
| l21-27, l14-20, l7-13 + l30-31, l0-6 + l28-29 | dst, src0, src1, src2 | | oracle |

nosched is rejected (oracle).

### hw 1e: st

`st.<a|l|t>.<b|w|d> base, off, data  [...]` (operands: base, offset, data).
Same layout as ld, with these differences (oracle): no destination (l21-27
and h7 are 0), no DRC (h0 = f3_13; unknown), h22 = nosched (allowed on
stores), the data is the src2 slot (any source kind, including immediates).
A store has no range operand: with the range flag (h6) set, src2 still holds
the data. Corpus: ix_tmpw (`st.l.d sa1, pa1, sa9`, dynamic array writes;
`st.l.d sa1, sa13, pa0 [rpt=16]`) and ix_comp; both stores are also
exercised by the open stack (dEQP-GLES2).

---------------------------------------------------------------------------

### hw 1f: the special class

The class splits on h20-22 (oracle):

| h22 h21 h20 | group | op field |
|---|---|---|
| 0 0 0 | flow control | h6-8 |
| 0 0 1 | MOE control | h24-26 |
| x 1 0 | other (h22 is data in emit and nosched in limm) | h24-26 |
| x 1 1 | visibility test (h22 is data in atst8) | h24-26 |
| 1 0 0 | phase / 0xfe-0x100 (h23 = 0) | h24-26 |

So unlike the ALU classes the predicate lives in h24-26 only in flow control;
elsewhere h24-26 is the op and the predicate (where there is one) moves:
limm h9-11 (PRED3), depthf h9-10 (PRED2). Everything else rejects a
predicate (oracle).

#### flow control (h20-22 = 0)

| form | fixed | fields | evidence |
|---|---|---|---|
| `ba #target` | h6-8 = 0, h19 = 0 | target l0-17: label address + the program's load offset (encode()'s `offset`), unsigned 18 bits | oracle; **hardware** (hwt_ba): the target is the absolute instruction index from the USE code base the task runs from (DOUTU's base 0 = 0x37000000): `ba #0x1077` reached the instruction at 0x37008388 + 6*8. From base 1 (0x37080000), `ba #0x1090` lands at 0x37088480, and `ba #0x11090` at 0x37108480, so the origin is the task's code base and not the heap; from base 0, `ba #0x11090` reaches 0x37088480, past the first window (hwt_window) |
| `br #target` | h6-8 = 1, h19 = 0 | target l0-17: label + label-argument offset - this instruction's index, signed 18 bits (-0x20000..0x1ffff) | oracle, corpus; **hardware** (hwt_p2 `br_1..3`, `loop_br`, `br_pred_*`): `br #n` goes to this instruction + n; `#-2` loops back; a predicated br falls through when its predicate is false |
| `lapc` | h6-8 = 2 | return to the saved link address | oracle, corpus (fn_call); **hardware** (hwt_p7 `call_lapc`): `br #3 [link]` ... `lapc` returns to the instruction after the br |
| `nop` | h6-8 = 5 | h23 syncs, h18 end, l0 = f2_26, l1 = f3_5; no predicate, h12 and h24-26 are 0 | oracle, corpus (secondary-program prologue `nop [nosched]`, discard placeholder) |
| `setl src` | h6-8 = 3 | set the link address from a register: src in the src1 slot (l7-13, bank l30-31, extension h17) | **hardware** (the open microkernel saves and restores the link around a nested call with `savl r23` ... `setl r23` and runs on the CE5300 board and CE5300); capture (the DDK microkernel calls through a table with `setl` + `lapc` and nests calls the same way). The vendor assembler has no form for it, so there is no oracle; exercised by the open stack (dEQP-GLES2) |
| `savl dst` | h6-8 = 4 | save the link address to a register: dst in the destination slot's number and bank (l21-27, h0-1), no extension bit | as `setl` |
| `op0x98` | h6-8 = 0, h9 = 1, h19 = 1 | no operands; accepted for the SGX545 | oracle; meaning unknown |

Fields of ba/br/lapc/setl/savl (oracle): h24-26 pred (PRED3; pn rejected), h23
syncend, h12 xsyncend (extended sync-end: the vendor assembler sets h12 and
h23 together for it, and never together with plain syncend), h11 nosched, and
for ba/br h9 link (save the return address; a call is `br [link]`: corpus
fn_call, hardware hwt_p7) and h10 f1_22 (meaning unknown).

Branch targets: in the USP binaries every branch has low word 0: the branch
is stored in a branch block with its label number, and the patcher fills the
target at finalize. `lapc` is stored in a branch block with label 0xffff
(capture).

#### MOE control (h20-22 = 1)

h18 = nosched for the whole group; no predicate. h19 = register form for ops
0-3 (the values come from src1/src2 instead of immediates),
printed `smoa.r`, `smr.r`, `smlsi.r`, `smbo.r`: two operands, the generic
src1 and src2 slots with their banks and extension bits, and nothing else in
the word (oracle).

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `smoa v0, v1, v2, v3 [am0..am3]` | four signed 10-bit values: v0 l30-31 + h0-7, v1 l20-29, v2 l10-19, v3 l0-9; four 2-bit modes: am0 h14-15, am1 h12-13, am2 h10-11, am3 h8-9 | oracle; name and meaning guess (MOE operand addressing) |
| 1 | `smr v0..v3` | four 12-bit: v0 h4-15, v1 l24-31 + h0-3, v2 l12-23, v3 l0-11 | oracle; name guess |
| 2 | `smlsi v0..v3 [lim0 lim1 lim2]` | per operand (dest, src0, src1, src2) an 8-bit value: v0 l24-31, v1 l16-23, v2 l8-15, v3 l0-7; flag h3/h2/h1/h0 set = the value is a swizzle (four 2-bit selects, printed `#0.xyzw`), clear = a signed increment; lim0 h12-15, lim1 h8-11, lim2 h4-7 = the limit / 4 (the vendor assembler takes multiples of 4, up to 60) | oracle, corpus (every swizzled operation is bracketed by `smlsi`); exercised by the open stack (dEQP-GLES2); limits' meaning guess (temp / pa / sa wrap limits) |
| 3 | `smbo v0..v3` | as op 1 | oracle; MOE base offsets (the USP code block keeps 4 MOE base offsets) — guess |
| 4 | `imo v0..v3` | as op 1, signed (the vendor assembler takes -32..31); no register form | oracle; name guess (increment MOE offsets) |
| 5 | `setfc #a0, #a1` | a0 l0, a1 l8 (each #0 or #1) | oracle, corpus (`setfc #1, #1` in tx_dep_loop, cos_l, sin_l). **hardware** (hwt_p6): **a1 (l8) is the color-class C10 enable** and is **on at program start**: `sop2 r3, r0.c10, r1.c10` reads C10 with no setfc, after `setfc #1, #1` and after `setfc #0, #1`; after `setfc #0, #0` or `#1, #0` the same word reads r64/r65 (garbage, different per pixel, no fault). **a0 (l0) is the efo F16 enable** and is **off at program start** (hwt_p8: an efo with operand bit 6 reads r64 with no setfc or after `setfc #0, #1`, and the F16 half after `setfc #1, #1`) |
| 6 | `op0xfd dst, #n` | dst l21-27 + h0-1 + h19 (destination slot), n h11-14 (0..8), h23 skipinv | oracle; meaning unknown |

The USP code block header's four MOE words are this state at block entry:
0x8000 | increment (low 8 bits signed) or 0x4000 | swizzle, per operand (for
example after `smlsi #1, #1, #0.zyxw, #1` the next block has 0x8001 0x8001
0x40c6 0x8001; corpus).

#### other (h20-21 = 2)

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `idf [drc=d path=p]` | h0 DRC, h14 path (#0/#1) | oracle; exercised by the open stack (dEQP-GLES2); idf = issue data fence (guess) |
| 1 | `wdf [drc=d]` | h0 DRC; waits until the loads/samples that signal that DRC have written their results | oracle, corpus (after every uniform load and before the first use of a loaded or sampled register); exercised by the open stack (dEQP-GLES2) |
| 2 | `op0xd7 #n` | n l0-3 (below 16) | oracle; unknown |
| 3 | `emit.<t0..t3> src0, src1, src2 [n f10 f12 f13 f3_20 imm end]` | tgt h14-15 (0-3, printed t0-t3); f12 h12; f13 h13; f10 h10; l21 = f3_20; n h0-1 + h9 (0..5); h18 end; h11 nosched; imm: a 14-bit immediate (bits 0-5 in l22-27, 6-11 in h3-8, 12-13 in h22-23). The forms the vendor assembler emits for the SGX545: t0 with src0 (extension h19) + src1 (+ src2), with or without the immediate (without it, h8 is set: immediate 0x800); t2 with src0 + src1 (+ src2) and the immediate; t1 with src1 + src2; t3 with src2 only. f10 on t1 and t3, f12 on t1, t2 and t3, f13 on t1 and t2. Unused sources hold #0. In the word all three source slots exist whatever the form (src0: l14-20, bank h2, extension h19; src1, src2 generic), the text always shows all three, and `imm=` and `n=` are the raw fields (n: bits 0-1 in h0-1, bit 2 in h9) | oracle; t1 is a geometry-shader vertex emit and f13 on t1 is "emit and cut" (**unconfirmed**); target names unknown |
| 4 | `limm dst, #imm32` | dst = destination slot (l21-27, h0-1, h19); imm l0-20 (bits 0-20), h4-8 (21-25), h12-17 (26-31); a label operand encodes its absolute address (+ offset); h9-11 pred (PRED3); h23 skipinv; h22 nosched; h18 end | oracle, corpus (`limm r0, #4096` before indexed stores); **hardware** (every hwt test loads its inputs with it: 0x12345678, 0xdeadbeef exact) |
| 5 | `lock #n` / `release #n` | l0 = 0 / 1; n l4-6 (below 8) | oracle; names guess (mutexes) |
| 6 | `ldr dst, addr [drc=d]`, `str addr, data` | h19 = str; dst l21-27 + h7 (r/pa); addr = src2 slot whose immediate form has 14 bits (low 7 l0-6, high 7 l14-20, and the slot's immediate bank: l29 + h16); data = src1 slot; h0 DRC (ldr); h23 skipinv; h11 nosched | oracle; names guess |
| 7 | `op0xd6 #n` | n l0-2 (0..5), l3 = f3_6 | oracle; unknown |

h11 = nosched for all of them except limm (h22) (oracle).

#### visibility test (h20-21 = 3)

Sources: src0 (l14-20, bank h2, extension h19), src1, src2. Select bits:
h5 (`sel34`), h3 (`sel36`), h4 (`sel37`); none set is the default. h18 = end,
h11 = nosched, h23 = syncs (atst8, depthf). All oracle.

| op | form | layout | evidence |
|---|---|---|---|
| 0 | `pcoeff src0, src1, src2`, `ptoff` (h15, no sources: every source field is 0) | | oracle; exercised by the open stack (dEQP-GLES2, pcoeff); names guess (plane coefficients / punch-through off) |
| 1 | `atst8 dst, pdst, src0, src1, src2` | dst l21-27 + h0-1 without extension (r o pa); pdst = predicate p0-p3 in h7-8, h6 = write it (clear: printed `p1.nw`); h22 = f2_25; h3/h4/h5 sel. The vendor driver's own atst8 (`f9340406`, capture) also sets h10, which the vendor assembler never does (oracle): h9-10 is a PRED2 predicate as in depthf (2 = p1); a false predicate kills the pixel (capture; hardware: "Phases, visibility feedback and discard") | oracle; exercised by the open stack (dEQP-GLES2, with h10); name guess |
| 3 | `depthf src0, src1, src2` | h9-10 pred (PRED2); h4/h5 sel (h3 rejected) | oracle; exercised by the open stack (dEQP-GLES2); name guess |

#### phase / 0xfe-0x100 (h20-22 = 4, h23 = 0)

| op | form | layout | evidence |
|---|---|---|---|
| 2 | `phas src1, src2` (h18 = 0) | h19 end, h11 nosched | oracle; name guess |
| 2 | `phas.i #pc, #temps4 [rate wait sel43 end]` (h18 = 1) | pc l0-17 (18 bits, absolute from the task's USE code base, as `ba`; no load offset added); temps4 h0-7 = the temporaries / 4 (multiple of 4, up to 0x3fc); rate h11-12 (0, 1, 2); wait h8-10 (0, 1, 2, 7); sel43 h13; h19 end | oracle; exercised by the open stack (dEQP-GLES2: `wait=7`, `rate=1`, `end`); pc and temps4 as the next phase's address and temporaries: **hardware** (the two-phase discard program, "Phases, visibility feedback and discard"); rate and wait as execution rate and wait condition are a guess |
| 0 | `op0xff` / `op0xfe` / `op0x100 src1, src2, #n` | h12-13 = 1 / 2 / 3; n h8-9 (0..3); h15 = f3_11, h14 = f3_12; h11 nosched | oracle; unknown |

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
  program into two phases (capture; see "Phases, visibility feedback and
  discard").
- **calls** (fn_call): the function body is its own labeled region ending
  in `lapc` (a branch block with label 0xffff); each call site is `br
  [link]` to that label; arguments and results go
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
  (f3_13); smp h14/h15 (rdata/rinfo); the MOE ops other than smlsi/smbo;
  op0x98, op0xd6, op0xd7, op0xfd, op0xfe-0x100; emit targets; the
  visibility-test names and their select bits; phas fields.
- br/ba h10 (f1_22) and op0xd7's limit are tied to the same core feature
  (**unconfirmed**); what either does is unknown.
- smp's coordinate count (how many registers the coordinate spans) is not in
  the word; it follows from the dimension and the projective/PCF modes.
- The discard path is traced (see "Phases, visibility feedback and
  discard"); the visibility-test operands are not.
## USP texture-sample blocks and the driver's sample finalizer

How a texture read gets from GLSL to USSE code on SGX545 (Cedarview DDK 1.7):
the vendor compiler writes USP blocks 5 and 6 in place of code, and the
vendor GLES driver's USP finalizer turns them into instructions once the
texture formats are known.

Evidence tags: **capture** (the vendor compiler's USP binaries: `usp.py` and
`smpblock.py` parse every one in c1-c3, and a field is capture when it is
present and consistent across them; also the vendor driver's programs in GPU
memory), **corpus** (c1/c2/c3 and `ef_c3.txt`), **hardware** (A/B: the vendor
driver runs, on the CE5300 board, USPs that our writer `tools/usc/sgx_usp.c` builds
with that field; or open stack: the open compiler and driver emit it and pass
dEQP-GLES2 on the CE5300 board), **unconfirmed**, **guess**.

Tools: `smpblock.py` parses the three block types; `usp.py` calls it
(`block["sample"]`). `python3 -I smpblock.py corpus/c3` prints every sample
block of a corpus.

### Where the blocks come from

The vendor compiler writes one block 5 per texture read: a non-dependent read
(iterated coordinate) or a dependent one in one of four LOD modes (0 normal,
1 bias, 2 explicit LOD, 3 gradients; the mode is the template smp's h8-9).
Block 6 is the read's unpack, block 9 a texture or image write. (**capture**)

The driver's loader also treats block types 0 and 8 as an end of the stream
(**unconfirmed**); the corpus ends every stream with type 7.

Every sample block sits where the instruction stood in the program, between
code blocks (**capture**). The generated code goes at that point (**hardware**,
open stack: the open driver inserts its smp and `wdf` there).

#### Shared sub-records

- **Format control + MOE state, 18 bytes** (also the start of code blocks):
  u16 format control (b0: F16 format select enabled, b1: C10 format select
  enabled; what `setfc` sets), 4 u16 per-operand MOE mode (`0x8000|inc` =
  increment, `0x4000|swz` = four 2-bit swizzle selects, what `smlsi` sets;
  default `0x8001`), 4 u16 MOE base offsets (what `smbo` sets). (**capture**:
  the defaults everywhere, `tx_dep_loop` format control 3 and increments
  +255, +1, +0, +1; **hardware**, A/B: the defaults; the `0x4000` swizzle
  form **unconfirmed**)
- **Per-instruction flags (iflags), u16** (the same word as a code block's
  per-instruction flags): b0 internal registers live at this instruction
  (**capture**: set exactly where the block's ilive is non-zero); the driver
  then saves and restores i0..i2 around code it inserts (**unconfirmed**). b2
  the instruction lacks skip-invalid (**capture**: `tx_dep_chain`); when it is
  clear, every instruction the driver generates gets skipinv
  (**unconfirmed**). b4-7 are written only under a compile option; their
  meaning is **unknown** (**unconfirmed**).
- **Register types.** Two USP enums: 2-valued fields 1 = r, 2 = pa; 4-valued
  fields 1 = r, 2 = o, 3 = pa, 4 = i. (**capture** for r and pa; o and i
  **unconfirmed**)

### Block 5: SAMPLE (56 bytes)

| off | type | field | values / meaning | tag |
|---|---|---|---|---|
| 0 | u16 | flags | b0 non-dependent, b1 projected (only with b0), b2 matched against the iterator entry's third key (**guess**: centroid) | capture (proj4 sets b1; b2 never set); hardware (A/B: b0, b1); b2 **unconfirmed** |
| 2 | 18 B | fmt ctrl + MOE | see above; c3 is always `2, 8001×4, 0×4` except `tx_dep_loop` | capture; hardware (A/B) |
| 20 | u32 | sample id | pairs the block with its unpack (block 6); 0, 1, ... per shader | capture; hardware (A/B) |
| 24 | u16 | iflags | see above | capture |
| 26 | u16 | sampler | texture unit, 0..15 (the loader rejects >15: **unconfirmed**) | capture, corpus (`tx_nd_two` 0/1, `tx_nd_same2` 0/0); hardware (A/B) |
| 28 | u16 | swizzle | 4 × 3 bits: result channel i ← texel channel sel_i (0-3 = xyzw; ≥4 not seen). Bits 12-15 must be 0 (**unconfirmed**). The compiler folds a following channel reorder into it: `0x60a` = zyxw when the texel goes straight to the BGRA color output, `0x688` = xyzw otherwise | capture, corpus; hardware (A/B: `0x688`) |
| 30 | u16 | dest type | 2-valued: where the smp writes the raw texel data | capture; hardware (A/B) |
| 32 | u16 | dest number | | capture; hardware (A/B) |
| 34 | u16 | usrc type | 4-valued; equal to the unpack's source in every corpus block. The driver compares this register with the unpack's source and first destination and unpacks in place when they match (**unconfirmed**) | capture; meaning **guess** |
| 36 | u16 | usrc number | | capture; hardware (A/B) |
| 38 | u16 | scratch type | 2-valued: first temporary the driver may use for its own code (iReg saves, state loads, coordinate fix-ups) | capture; hardware (A/B); use **unconfirmed** |
| 40 | u16 | scratch number | c3: r20..r83 fragment, r294/r302 vertex | capture |
| 42 | u16 | ilive | mask of internal registers live across the sample | capture (3 where the coordinate is in i0: `tx_nd_proj3`, `tx_dep_proj3/4/b`, `tx_dep_loop`, `vtx_projlod`) |
| 44 | u16 | ilive C10 | the subset holding C10 data (saved with two instructions instead of a mov) | capture (always 0); meaning **unconfirmed** |
| 46 | u16 | forced format | result format override: 8 = none (values map n-1 → 1 f32, 2 f16, 3 c10, 4 u8, 6, 7, else none); written ≠ 8 only under a compile option | capture (always 8); hardware (A/B: 8); other values **unconfirmed**; 6/7 **unknown** |
| 48 | 8 B | dependent: smp template | the smp the vendor compiler encodes, with the destination zeroed and the texture state `#0`. Identical to the smp words logged in `ef_c3.txt` for every dependent shader | capture, corpus; hardware (A/B; open stack: the template with its destination and state filled in) |
| 48 | 4 u16 | non-dependent | coordinate iterator + 1 (1..10 accepted: **unconfirmed**), coordinate dimension (≤3; 2 = 2D, 3 = cube), coordinate register type (4-valued), number. The driver substitutes the template `0x20000000 / 0xe0411000` (**unconfirmed**) | capture, corpus; hardware (A/B) |

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
| 0 | 18 B | fmt ctrl + MOE | as above | capture; hardware (A/B) |
| 18 | u32 | sample id | must match a block 5 | capture; hardware (A/B) |
| 22 | u16 | iflags | | capture |
| 24 | u16 | channels | channels to write; a subset of the next field | capture; hardware (A/B) |
| 26 | u16 | mask | live channels of the destination | capture; hardware (A/B) |
| 28 | 4 u16 | per-channel register type | 4-valued (0 for unwritten channels) | capture; hardware (A/B) |
| 36 | 4 u16 | per-channel register number | base + i / (channels per register) | capture; hardware (A/B) |
| 44 | 4 u16 | per-channel format | 1 f32, 2 f16, 3 c10, 4 u8 | capture (highp f32, mediump f16, lowp to the color output u8; c10 not seen); hardware (A/B: u8) |
| 52 | 4 u16 | per-channel byte offset | 0 for f32; 0/2 for f16; 0..3 for c10/u8 (the loader checks the bound per format: **unconfirmed**) | capture; hardware (A/B) |
| 60 | u16+u16 | source | 4-valued type, number: where the unpack expects the raw data | capture; hardware (A/B) |
| 64 | u16+u16 | smp dest | 2-valued: block 5's dest | capture; hardware (A/B) |
| 68 | u16+u16 | scratch | 2-valued: temporaries for the conversion code | capture; hardware (A/B); use **unconfirmed** |
| 72 | u16 | ilive | internal registers live here | capture |

### Block 9: TEXTURE WRITE (82 bytes; was 78 in usp.py)

20 u32 then one u16, in this order: an id; eight (register number, operand
format) pairs, one per operand (operand format codes 1-5); a word; a format
(one of 1, 2, 3, 4, 7, 6); a word; iflags. GLSL ES never produces it, so the
whole layout is **unconfirmed**; the field meanings beyond the layout are
**unknown**.

### The finalizer

All of this section is **unconfirmed** except where a tag says otherwise. Per
sampler, the driver hands the finalizer a texture format descriptor, and for
some formats the texture's width and height.

**Linking and planning**:
1. Each sample finds its unpack by sample id (the ids pair one to one in the
   corpus: **capture**).
2. From the format descriptor and the swizzle, which *chunks* of the texel are
   needed. A texture format is split into up to 4 chunks; chunk c returns
   `ceil(bits/32)` dwords at a dword offset in the raw area. Channels can also
   be constant 0/1, which need no chunk. A YUV-like format (channels 0-2 all
   from one chunk) adds a FIRH color conversion later.
3. The final **iterator list** (what GL's PDS generator turns into DOUTIs;
   PDS.md "DOUTI"): for non-dependent samples it adds sample entries (key:
   coordinate iterator, dimension, block 5 flag b2) within a PA budget of
   `0x80 -` header 0x42. Only the **first** needed chunk of a non-dependent
   sample can come from the PDS (**capture**: one DOUTT per non-dependent
   sample in PDS.md's tex1/tex2).
4. Per non-dependent sample: a *plain* iteration of the coordinate is added
   when some chunk still needs a USSE smp, and the PDS-delivered chunks are
   placed. Then **texture state**: per (sampler, chunk) 4 consecutive dwords,
   placed in SA registers after the program's own or, when they don't fit
   below header 0x3c + 0x3e, in memory. The finalized program carries the
   table for GL to fill. Dependent samples get the same allocation.
   (**hardware**, open stack: a dependent sample's 4 state words in SA after
   the program's constants.)

**Code generation**: per sample (the non-dependent ones first, then the
dependent ones), then per unpack. For each sample, in order:
1. If the MOE state at the sample isn't the default: `smbo` 0,0,0,0 and/or
   `smlsi` 1,1,1,1.
2. If internal registers are live and some chunk needs a USSE smp: save each
   live i_n to scratch (`mov`; C10 ones with a `sopwm` + `pck` pair), after a
   `setfc` if C10 ones are live.
3. Texture state held in memory: `ld` 4 dwords per chunk into scratch (hi
   `0xe8800000 | (n-1)<<12`, base = an SA register from the program header +
   offset; which field: **unconfirmed**), merging adjacent chunks up to 16
   dwords, then `wdf`.
4. When the format setup flags the sampler: a coordinate fix-up before the
   smp: `fsubflr` (wrap) unless a second flag is set, `limm` 1/w (and 0.5/w),
   a hw 6 multiply-add; the same for h. **Guess**: NPOT/rectangle emulation.
5. Per needed chunk not delivered by the PDS: copy the template;
   non-dependent: set h10/h11 from the dimension and src0 = the plain
   iteration's PA register; set src1 = the chunk's 4 state registers; set the
   destination to dest + chunk offset. (**hardware**, open stack, for the
   dependent case: template, src1 and destination filled in this way.)
6. After the last smp: `wdf drc` (hi `0xf9200000 | drc`, drc = template
   h0-1). (**hardware**, open stack: the same word after each smp.)
7. For one texture format (not identified): `and dst, dst, #0x7fffffff` on
   the result.
8. Restore the saved internal registers.

Then, per sample: chunks the PDS delivered are found in PA and, if needed,
moved; the YUV conversion; and **the unpack**: per output channel, by result
format (block 6, or the forced format): f32: `pck.f32.<src>` with normalize
(src by the channel's texel format: u8, s8, u16, s16, f16, f32), or `mov` when
the channel is already f32; f16: `pck.f16.<src>`; c10/u8: pck to c10/u8, or in
place when the raw data already have that layout (with the swizzle); formats
6 and 7 take other paths (`limm 0xffff8000`, test). The pck format fields are
hw 08's (dst h6-8, src h9-11; **oracle**). **Hardware**: `pck.f32.u8 [scale]`
gives b/255 (hwt_p4) and the open stack unpacks with it and `pck.f16.u8`; the
u8 in-place path runs in the A/B harness. Finally the MOE state is put back
(smbo/smlsi/setfc) if it was changed.

### The smp instruction, as the vendor compiler writes it

| field | bits | tag |
|---|---|---|
| opcode | h27-31 = 0x1c | oracle; corpus |
| predicate | h24-26 | oracle |
| skipinv / syncstart / nosched | h23 / h20 / h22 | oracle; corpus (skipinv) |
| write mask, or repeat | h12-15 mask; with h21 set, repeat-1. SGX545: write mask 1 only, no repeat | oracle |
| LOD mode | h8-9: 0 none, 1 bias, 2 LOD, 3 gradients | oracle; corpus |
| dimension | h10-11 = dim-1 | oracle; corpus |
| dest | l21-27; h7 = pa, else r (only r and pa allowed) | oracle |
| coordinate | src0 l14-20, bank h2, ext h18 (r, pa, o, sa) | oracle; corpus |
| texture state | src1 l7-13, bank l30-31, ext h17 | oracle |
| bias / LOD / gradients | src2 l0-6, bank l28-29, ext h16 (none: `#0`) | oracle; corpus |
| coordinate format | h3 F16, h4 C10 | oracle; corpus |
| h5 | an assembler option (hw 1c: pcf) | oracle (position); meaning **unknown** |
| h14-15 path | assembler options on a path the SGX545 enables (hw 1c: `.rdata` / `.rinfo`) | oracle (position); meaning **unknown** |
| drc | h0 (0 or 1; h1 is never set) | oracle; corpus |

### The texture-sample sequence a compiler must emit

A compiler that does not go through the vendor finalizer has to do all of
this itself. The open compiler and driver do it for dependent 2D and cube
reads, bias, state words in SA, `wdf`, the u8 unpack and non-dependent PDS
reads, and pass dEQP-GLES2 on the CE5300 board (**hardware**, open stack). LOD reads,
gradients, vertex texture fetch, state words in memory and multi-chunk
formats are what the vendor compiler emits (**corpus**) or what its finalizer
does (**unconfirmed**); the open stack does not emit them yet. Field names as
above; exact bit forms belong to `hw_mem.py`.

**State words (all cases).** For each (sampler, chunk) the texture unit needs
4 control words, the same 4 a PDS `DOUTT` sends (PDS.md: word 1 = width-1 /
height-1, word 2 = base address, the rest format/filter/wrap; **capture**).
Put them in 4 consecutive SA registers (load them in the secondary update
program or by PDS `DOUTD` into the SA bank; **hardware**, open stack), or keep
them in memory and `ld` them into 4 temps followed by `wdf`
(**unconfirmed**). One chunk covers any ≤32-bit texel format (**hardware**
for the formats the open stack samples; the chunk split itself is
**unconfirmed**).

**Dependent 2D read** `c = texture2D(s, uv)`, uv computed in r2..r3, c wanted
as f32 in r8..r11 (**hardware**, open stack):
1. (MOE state default, internal registers not live; otherwise save them.)
2. `smp2d drc0 r4, r2, sa[S]` — opcode 0x1c, h10-11 = 1, h8-9 = 0, dest r4,
   src0 r2 (2 consecutive F32 coordinates; h3/h4 for F16/C10 ones), src1 =
   first of the 4 state SAs, src2 `#0`, mask 1 (one dword for RGBA8).
3. `wdf drc0` before the first use of r4 (the open driver puts it right after
   the smp).
4. Unpack r4: four `pck.f32.u8` with normalize, one per channel, each
   picking byte `swizzle[i]` of r4 into r8+i (or `pck.f16.u8` two per
   register for mediump; for lowp output to the color register, the raw
   RGBA8 can be written to the output as is with the channel order chosen
   by the swizzle: **corpus**).

**Cube read** `textureCube(s, d)`: as 2D with h10-11 = 2 and a 3-register
coordinate (the direction); the state words describe the cube texture
(**hardware**, open stack).

**Bias read** `texture2D(s, uv, b)`: h8-9 = 1 and src2 = the bias (any F32
source: pa, sa, r, or a hardware constant such as c52 = 1.0; **corpus**;
**hardware**, open stack).

**LOD read** `texture2DLod(s, uv, l)`: h8-9 = 2 and src2 = the LOD
(**corpus**). Gradients (`h8-9 = 3`, what the vendor compiler emits for a
sample inside non-uniform flow) take src2 = the first of the derivative
registers (**corpus**, one case).

**Vertex texture fetch** (**corpus**): the same smp with h8-9 = 2;
`texture2D` in a vertex shader uses LOD c48 (0.0) since there are no
derivatives; the vendor compiler also sets skipinv. No PDS sampling in vertex
programs: every read is a USSE smp. Result and unpack as above (c3's vertex
reads come back as f16 pairs).

**Non-dependent fragment read** (coordinate = an untouched varying): no
instruction. Emit an iterator entry with a sample: PDS `DOUTI` with the
coordinate set + 1 in bits 0-4, 2-D (0x200) or projected (0x400), followed by
the `DOUTT` with the 4 state words; the texel lands in the next PA registers
in raw format, and only the unpack runs in the USSE (**capture**, PDS.md
tex1/tex2; **hardware**, open stack). A texel format needing more than one
chunk gets its other chunks by smp from a plainly iterated coordinate
(template `0x20000000 / 0xe0411000`: src2 `#0`, nosched, mask 1;
**unconfirmed**). The PDS side (DOUTI/DOUTT bit layouts) is in `docs/PDS.md`.

### Open

- Block 5 flag b2 and the "usrc" pair: guessed meanings.
- iflags b4-7; smp h5 and the h14-15 path; forced formats 6 and 7.
- How the driver writes the texture-state table into SA registers at draw
  time: **unconfirmed**.
- The finalizer's chunk planning and its unpack for formats 6/7:
  **unconfirmed**.
- Block 9 field meanings; never produced by GLSL ES.
- Gradient operand layout (how many registers, which order): one corpus case.

## The USP container

The vendor compiler's shader binary. **Confirmed** (**capture**): `usp.py`
sizes every list from the header alone and parses all 552 shaders of c1-c3;
every one ends exactly on the end block at the declared size. Our writer
`tools/usc/sgx_usp.c` builds USPs to this layout, and the vendor driver runs
them on the CE5300 board (**hardware**, A/B: the fields the writer fills).

Fields are packed with no padding, so a u16 field leaves the next u32
unaligned (**capture**).

### Header

Tags: capture (the field's values across c1-c3), corpus (what a shader
feature changes), hardware (A/B: our writer fills the field and the vendor
driver runs the result), unconfirmed.

| off | type | field | meaning | tag |
|---|---|---|---|---|
| 0x00 | u32 | magic | `0x55535020` ("USP ") | capture; hardware (A/B) |
| 0x04 | u32 | version | `0x23` (the loader rejects anything else: **unconfirmed**) | capture; hardware (A/B) |
| 0x08 | u32 | size | size of the binary **minus 12** | capture; hardware (A/B) |
| 0x0c | u32 | block type | 1: PC shader header, always first | capture; hardware (A/B) |
| 0x10 | u32 | program type | 0 fragment, 1 vertex, 2 geometry (2 **unconfirmed**) | capture; hardware (A/B) |
| 0x14 | u32 | flags A | 2: meaning unknown; 4: the finalizer then lets the color stay in pa0 in output mode 3 (**unconfirmed**); **8 = feedback split**: the compiler split the program at a feedback point; 0x10: **unconfirmed**. Corpus: 2 most texture shaders, 8 dc_* except dc_always, else 0 | capture, corpus; hardware (A/B: 2, 8); meaning of 2/4 unknown |
| 0x18 | u32 | compile flags | the flags the GLSL compiler passes to the vendor compiler. b5 (0x20) = the driver will insert visibility feedback (**unconfirmed**); discard shaders also get b11 (0x800); b3 with dynamically indexed uniforms (ix_n, vix_*); b29/b30 vary | capture, corpus; hardware (A/B); other bits unknown |
| 0x1c | u32 | flags B | 1 = all dc_*; 2 = the last instruction cannot take `end` (**unconfirmed**); 4 = the draw-time builder sets `phas` h13 (**unconfirmed**); 8 / 0x10: meaning **unconfirmed** (0x10: the finalizer counts one more color register); 0x20: unknown; **0x40 = no secondary update program**. The finalizer adds 0x100 when a branch is out of range (**unconfirmed**) | capture, corpus; hardware (A/B: 1, 0x40) |
| 0x20 | u32 | (unnamed) | always 9 (1..10 accepted: **unconfirmed**) | capture; hardware (A/B); meaning unknown |
| 0x24 | u32 | (unnamed) | always 0x2793 | capture; hardware (A/B); meaning unknown |
| 0x28 | u32 | workaround count | u32 ids after the header; none in the corpora. The ids 0x54c1, 0x54d1, 0x5518 (21697, 21713, 21784) under compile options (**unconfirmed**) | capture; hardware-erratum (BRN) numbers is a guess |
| 0x2c | u32 | flags C | which SA areas exist: 1 = list B non-empty (**unconfirmed**); 4 = memory area 0x34/0x60 (**unconfirmed**); 8 = constants in SA (0x3c, 0x7e); 0x10 = indexable temporaries in memory (0x40, 0x4a). Corpus: 8 with uniforms, 0x18 with temp arrays | capture, corpus; hardware (A/B: 8) |
| 0x30 | u16 | (unnamed) | 0; tied to list B (**unconfirmed**) | capture |
| 0x32 | u16 | | 0 | capture |
| 0x34 | u16 | SA of memory area | 0xffff in the corpora; an SA register when flags C 4 (**unconfirmed**) | capture; hardware (A/B) |
| 0x36 | u16 | | 0xffff | capture; hardware (A/B) |
| 0x38 | u32 | in-memory constants | 0 in the corpora; meaning **unconfirmed** | capture |
| 0x3c | u16 | constant SA base | first SA register holding constants (9 fragment, 10 vertex; `tx_dep_bias` reads its uniform from sa9). List C register numbers are relative to it | capture, corpus; hardware (A/B) |
| 0x3e | u16 | constant SA count | SA registers available from 0x3c (0x77, 0x1f6); the finalizer puts texture state after the program's SAs and moves it to memory when it doesn't fit below 0x3c + 0x3e (**unconfirmed**) | capture; hardware (A/B) |
| 0x40 | u16 | scratch SA | SA register with the address of the indexable-temporary memory (1 in ix_comp/ix_tmp*), else 0xffff | capture, corpus; hardware (A/B: 0xffff) |
| 0x42 | u16 | (unnamed) | 0; the finalizer's PDS budget is `0x80 -` this (**unconfirmed**) | capture; meaning unknown |
| 0x44 | u16 | PA count | primary attribute registers the main program uses (fragment: iterated inputs, vi_4 = 16; vertex: attribute registers) | capture, corpus; hardware (A/B) |
| 0x46 | u16 | temp count | temporaries (r) of the main program (0 straight-line, 5 loops, 13 fl_big); the finalizer raises it for its own code (**unconfirmed**) | capture, corpus; hardware (A/B) |
| 0x48 | u16 | secondary temp count | temporaries of the secondary update program (mod_m: r0-r4 = 5, sign 4, div_m 1) | capture, corpus |
| 0x4a | u32 | scratch size | bytes of indexable-temporary memory (ix_comp 0x1000, ix_tmparr 0x4000) | capture, corpus |
| 0x4e | u16 | list A entries | iterated inputs (fragment only; vertex 0) | capture; hardware (A/B) |
| 0x50 | u16 | list C records | constants in SA registers | capture; hardware (A/B) |
| 0x52 | u32 | list B records | constants in memory (0 in the corpora) | capture |
| 0x56 | u16 | list D entries | 3-u16 entries (always 0) | capture |
| 0x58 | 2 u32 | sampler mask | one bit per sampler; set only by the vertex texture shaders (vtx_*) | capture, corpus; meaning unknown |
| 0x60 | u32 | memory area size | 0 in the corpora; size of the flags C 4 area (**unconfirmed**) | capture |
| 0x64 | u16 | | 0 | capture; unknown |
| 0x66 | u16 | color type | fragment: register type of the color result, 4-valued (1 r, 2 o, 3 pa): 3 | capture, corpus; hardware (A/B) |
| 0x68 | u16 | color register | fragment: its number (pa0; pa2 in dc_gt) | capture, corpus; hardware (A/B) |
| 0x6a | u16 | color format | fragment: 1 f32, 2 f16, 3 c10, 4 u8 (4 everywhere: packed BGRA8) | capture, corpus; hardware (A/B) |
| 0x6c | u16 | color registers | fragment: 1 | capture; hardware (A/B) |
| 0x6e / 0x70 / 0x72 | u16 | color alternatives | fragment: the r, pa and o register the finalizer may move the color to (output modes 2, 3, 1 below; **unconfirmed**). The pa one equals 0x68 in the corpora, the others are 0 | capture |
| 0x74 | u16 | start label | label at the main program's start (3, fn_call 4). The loader opens a code block there and the finalizer puts its placeholders in it (**unconfirmed**) | capture, corpus; hardware (A/B) |
| 0x76 | u16 | feedback label | label just before the discard `nop` (the pre-feedback driver epilog), 0xffff if none | capture, corpus (dc_* 4); hardware (A/B) |
| 0x78 | u16 | phase-1 label | label after the epilog, where the post-feedback phase starts | capture, corpus (dc_* 5); hardware (A/B) |
| 0x7a | u16 | fourth label | 0xffff in the corpora | capture; hardware (A/B); guess: the sample-rate split point |
| 0x7c | u16 | secondary instructions | length of the secondary update program | capture |
| 0x7e | u16 | SA constant count | SA registers from 0x3c holding constants (flags C 8; tx_dep_bias 1) | capture, corpus; hardware (A/B) |
| 0x80 | u16 | input mask bits | vertex: the mask's bit count | capture; hardware (A/B) |
| 0x82 | u16 | output mask bits | always 0x80 | capture; hardware (A/B) |

The color *output mode* is set per draw: 0 when blend or color-mask code
will be appended (the color stays where it is), 1 otherwise (moved to the o
register); the finalizer adds the move (**unconfirmed**). The move itself is
**capture**: the vendor driver's finalized probe program ends `mov o0, sa9`.

### Lists and blocks

After the header, in this order (**capture**):

1. **Workaround ids**: header 0x28 u32s.
2. **List A** (fragment): 12 bytes (6 u16) per iterated input, in PA
   order: flags (4 or 0xc for a plain iteration; 0 for a sample entry, 1
   when projected (`tx_nd_proj4`), with the sampler in the next field; b1
   meaning unknown), sampler, iterator (texture coordinate set + 1 for
   varyings; 0xb/0xc, 0xd, 0xe, 0xf for the compiler's other input kinds),
   components, format (1 f32, 2 f16, 3 c10: highp, mediump, lowp in the
   corpus), registers (PA registers it takes: 4 for a highp vec4, 2 for a
   mediump one). A highp `a` gives `(4, 0, 1, 4, 1, 4)`. (**capture**,
   **corpus**; **hardware**, A/B.) The draw-time builder iterates 0xb and 0xc
   from DOUTI source 0x20, the color, and the rest from source 0
   (**unconfirmed**). gl_FragCoord is iterator 14, DOUTI source 0 (PDS.md
   "DOUTI"); gl_PointCoord is set 0 of a sprite; gl_FrontFacing is not an
   iterated input ("Fragment shaders").
3. **List B** then **List C**: constant records whose first u16 is a kind.
   Kinds 1, 2 and 4 are 8 bytes; kinds 3 and 5 are 10. Kind 1 `(1, n, n, 0)`
   is one uniform component; 2 is two (F16 halves, `(n & 3) != 3`), 3 a C10
   triple, 4 a 32-bit value. List B holds constants the driver leaves in
   memory, list C those it loads into SA registers from header 0x3c on. Only
   list C occurs in the corpora, with kinds 1, 2, 3 and 5. (**capture**;
   **hardware**, A/B: kinds 1-3; kind 4 and list B **unconfirmed**)
4. **List D**: header 0x56 entries of 3 u16 (the loader checks ≤ 16, ≤ 4:
   **unconfirmed**); never written (**capture**).
5. **Secondary update program**: header 0x7c instructions, 8 bytes each.
   **Confirmed** separate from the main program: the compiler moves all
   uniform-only math here (`gl_FragColor = -u` puts the negate here and
   leaves one instruction in the main program). It runs once per draw and
   its pa registers are the main program's sa (**hardware**, docs/NOTES.md).
6. Vertex only: the input mask, `ceil(header 0x80 / 32)` u32: the primary
   attribute registers the program reads (the PDS fetches those; corpus
   vo_*; **hardware**, A/B).
7. **Output mask**, `ceil(header 0x82 / 32)` = 4 u32, always
   `ffffffff ffffffff 0 0` (**capture**; **hardware**, A/B). The vendor
   compiler numbers vertex outputs by its set bits (output n → popcount of
   the bits below n; **unconfirmed**), so with all 64 bits set the numbering
   is the identity. Its meaning beyond that is a **guess** (the output
   registers that exist).
8. **Block stream**, each block a u32 type and a body, until type 7:

| type | body | meaning |
|---|---|---|
| 2 | u16 n, u16 flags, 4 u16 MOE state, 4 u16 MOE base offsets, n u16 per-instruction flags, n × 8-byte instructions | straight-line code. flags = format control at block entry (bit 0 EFO F16, bit 1 C10; 2 unless the code uses `setfc`; **corpus**). MOE state per operand: 0x8000 \| increment or 0x4000 \| swizzle |
| 3 | 8-byte instruction, u16 label | branch (the offset field is 0 in every corpus branch; the target is patched at finalize) |
| 4 | u16 label | label; header 0x74-0x7a name the ones the driver patches at |
| 5 | 56 bytes | texture sample: non-dependent (iterated coordinate) or dependent (the vendor compiler's encoded smp as a template); see the sample-block section |
| 6 | 74 bytes | sample unpack: texel data to the shader's registers and formats |
| 9 | 82 bytes | texture write (not produced by GLSL ES) |
| 7 | none | end |

No `smp` appears in USP code (**capture**): the vendor driver's finalizer
generates the sampling code from the sample blocks at link time, for
dependent and non-dependent reads alike.

Each instruction is 64 bits, stored as (low word, high word). The disassembler
names the bits l0..l31 and h0..h31.

## Phases, visibility feedback and discard

A USP fragment program is not runnable as it stands: the vendor driver wraps
it in phase instructions when it finalizes the program and at draw time (the
same step builds the DOUTI words of docs/PDS.md), and for `discard` adds the
visibility feedback. Tags: **capture** (the vendor driver's finalized
programs in GPU memory: the probe's in docs/NOTES.md, and STATE.md "The DDK's
fragment programs for a flat color and one sample"), **hardware** (the
discard draws on the CE5300 board below, and the open stack's discard programs, which
use the same words and pass dEQP-GLES2 on the CE5300 board), **oracle**,
**unconfirmed**, **guess**.

### What the finalizer prepares

- The program starts at the USP header's start label (0x74). The driver
  puts one phase instruction there, or two (`phas` and `pcoeff`) when the
  draw needs feedback (one: **capture**; two: **hardware**, the open
  stack's discard programs).
- The feedback label (header 0x76) is at the USP's discard `nop`; the phase-1
  label (header 0x78) is where phase 1 starts (**capture**, the dc_* corpus
  binaries).
- USP header flags B 2 says the program's last instruction cannot carry
  `end`; the driver then appends `nop [end]`. Flags B 4 sets `phas` h13.
  In a feedback program (flags A 8) whose blend code gets a phase of its
  own, the driver adds one more `phas`. (**unconfirmed**)
- The color register the driver's code uses (atst8's c below) is where the
  color is after the finalizer's move (the probe's `mov o0, sa9`:
  **capture**).

### The draw state

The vendor driver picks the ISPA pass type (STATE.md: b25-27) from whether
the linked fragment shader contains `discard`, and from the primitive:

| when | ISPA pass type |
|---|---|
| the shader doesn't discard | 0 opaque / 1 translucent (blend) |
| discards, triangles | 4 punch-through, or 2 when blending |
| discards, points or lines | 5 |

(No discard: **capture**, STATE.md's `01d00200` and `03d00200`. Triangles:
**hardware**, the open stack's discard draws use 4 / 2, and STATE.md "Alpha
test (ES1)" finds 4 works like the DDK's 1. Points and lines:
**unconfirmed**. The pass-type names are STATE.md's.)

### What the draw-time builder emits

Instruction words as written (low high), decoded with `dis.py -x`:

| words | decodes as | role |
|---|---|---|
| `00000000 fa440700` | `phas.i #0, #0 [wait=7]` | single phase: no next phase |
| `pc>>3 fa440800 \| t` | `phas.i #pc, #t` | phase 0 → phase 1 at `pc` (byte address relative to the task's USE code base, /8: the DOUTU base's window, not the heap); t = (temps + 3) / 4 in h0-7; h13 = header flags B 4; h11 (`rate=1`) when the blend code runs in a phase of its own |
| `00000000 fa4c0700` | `phas.i #0, #0 [end wait=7]` | last phase ends here |
| `00000000 f8040140` | `nop [end]` | appended when header flags B 2 says the last instruction can't take `end` |
| `n<<14 \| (n+1)<<7 \| b0000004 f8300004` | `pcoeff pa<n>, ...` | n = PA registers taken by the program's iterated inputs: the first register after them |
| `c<<21 \| c<<14 \| f0000184 f9340406` | `atst8 pa<c>, p0.nw, pa<c>, ... [end]` with h10 set | c = the color register |
| `a0000000 28a10002` | `mov pa0, pa0 [skipinv rcnt]` | a no-op in pcoeff's place (points and lines) |
| `(n+2)<<14 \| f0000184 fb340404` | `(p1) depthf pa<n+2>, ... [end]` | built for points and lines, see below |

(`phas.i #0, #0 [wait=7]`: **capture**. The phase-0 `phas`, `pcoeff`,
`atst8` and closing `phas`: **hardware**, the open stack's discard
programs, except that their phase-0 `phas` is `phas.i #pc, #t [wait=1]`
(hi `fa440100 | t`, h11 clear).
`mov pa0, pa0` and `depthf`: **hardware**, the `depthf_patch.py` draws
below. `nop [end]` as an append, and the h13 / h11 conditions:
**unconfirmed**.)

Without feedback:
- placeholder 0 = `phas.i #0, #0 [wait=7]`, then the program, with `end`
  set (h18) on its last instruction, or `nop [end]` appended (header flags
  B 2). **capture**: the probe's finalized main program is
  `phas.i #0,#0 [wait=7]`, `mov o0, sa9 [end]`, and its secondary program
  starts with the same `phas`.
- Blend and color-mask code (the mask is the
  `sopwm ... 0x90b00601 | mask<<11` of STATE.md) is appended after the
  program (**capture**, STATE.md "Blending and the color mask are shader
  code"). In some draws it gets a phase of its own (a `phas` to it, then
  `phas.i #0,#0`) (**unconfirmed**; which draws is not known).

With feedback (discard on triangles):

```
[start]    phas.i #phase1, #temps4             (phase 0 runs up to the feedback)
[start+1]  pcoeff pa<n>, ...                   (PDS-iterated value, see below)
           ... the shader up to the discard test (writes p1 = keep) ...
[feedback] atst8 pa<c>, p0.nw, pa<c>, ... [end] (replaces the USP's nop; h10 set)
[phase 1]  the rest of the shader (color packing)
           phas.i #0, #0 [end wait=7]          (appended after the program)
```

- The PDS program gets one more plain iteration after the shader's own:
  DOUTI iterate half `0x7811000` (source 0x22, 3 components, 0x6000000);
  it fills pa<n>..: that is what `pcoeff` reads (**hardware**: the open
  stack's discard programs iterate source 0x22, 3 components, the same
  way). In the draws whose blend code gets a phase of its own it is
  `0x7c11000` (4 components) (**unconfirmed**). That source 0x22 is the triangle's depth plane, so
  `pcoeff` = hand the ISP its plane coefficients, is a **guess** from the
  name.
- **Which predicate kills**: atst8's h9-10, a PRED2 predicate (0 none,
  1 p0, 2 p1, 3 !p0, as in depthf). The vendor assembler never sets it
  (**oracle**; `{?h10}` in dis.py); the driver's word has 2 = p1, the keep
  predicate the USP's `test` computed. A pixel whose atst8 predicate is
  false is killed. **hardware** (`tools/isa/discard/`, the left half
  of a triangle kept by `if (q.x > 0.0) discard`): the stock driver keeps
  exactly the left half (21600 of 43200 pixels); with a driver patched to
  put 0 in h9-10 nothing is killed; with p0 / !p0 about 83 % / 19 %
  survive, scattered over both halves (p0 is whatever the shader left
  there; the two are close to complementary).
- atst8's other operands (src1 = sa3, src2 = sa4 with bank bits l28-31 =
  0xf) are fixed, and the color register is both its source and
  destination. sa3 / sa4 hold the depth and alpha-test words of STATE.md
  "Alpha test (ES1)" (**capture**). The color's alpha does not matter: the
  same draw with alpha 0 keeps the same 21600 pixels (**hardware**), so
  whatever alpha test atst8 can do is off in GL's ISP state. The other
  operand meanings are not known.

For discard on points or lines, the stock driver adds a second extra
iteration (`0x7800000`, source 0, 3 components), puts `mov pa0, pa0` in
pcoeff's slot, and builds `(p1) depthf pa<n+2>, ... [end]` (hi `fb340404`,
predicated on p1) for the discard nop's place, but does not store it in the
program: the discard `nop` stays, and the program's temporary count becomes
0xfb340404 (**unconfirmed**; the hardware result below, points ignoring the
discard, agrees with it).

On hardware (2026-10-28; `tools/isa/discard/results.txt`):

| draw | stock DDK 1.7 | `depthf_patch.py` | `pt_patch.py` |
|---|---|---|---|
| triangle, discard right half | left half kept | same | same |
| 4 points (size 8), discard right two | **all 4 drawn**: discard ignored | left two kept | left two kept |
| 3 lines, discard right half | **frame broken**: 88 % black, no clear, no SGX recovery | same | same |
| 3 lines, discard never taken (`q.x > 5.0`) | frame broken | | same |
| line strip, or a pbuffer | | | frame broken |

- `depthf_patch.py` patches the stock driver to write that depthf into the
  program (and leave the temp count alone): points then discard correctly
  in the depth-only pass (type 5) with `(p1) depthf`. So depthf's predicate
  kills like atst8's (**hardware**).
- `pt_patch.py` instead patches it to send points and lines down the
  triangle path (pass type 4/2, atst8): points discard correctly that way
  too.
- **Lines with any discarding shader render a broken frame** on every
  path, also when the discard is never taken, so it is the feedback
  program with lines (pcoeff's plane-coefficient iteration, or the ISP's
  line handling), not the kill. Lines without discard draw normally. Not
  explained; avoid discard on lines (or draw lines as thin triangles).

### What a compiler must emit to discard

Nothing is "killed" in the USSE: a discarding pixel shader is a two-phase
program whose phase 0 ends in a visibility test, and the ISP decides.

1. Compute the keep condition into a predicate (the vendor compiler uses
   p1: corpus).
2. Start the program with `phas.i #<phase-1 address>, #<temps/4>` and
   `pcoeff pa<n>, ...` (with the PDS iterating the extra 3-component
   source-0x22 input into pa<n>.. after the varyings).
3. Run everything the feedback depends on in phase 0, then
   `atst8 pa<c>, p0.nw, pa<c>, sa3, sa4 [end]` with h9-10 = 2 (p1):
   the words `f0000184 | c<<21 | c<<14`, `f9340406` (the vendor driver's,
   and the open stack's: hardware).
4. Phase 1 (the color output) follows, ending with
   `phas.i #0, #0 [end wait=7]`.
5. ISPA pass type 4 (2 when blending) for triangles, and also for points
   (hardware). Points also work with pass type 5 and
   `(p1) depthf pa<n+2>, sa3, sa4 [end]` in atst8's place (a patched
   vendor driver, `depthf_patch.py`: hardware). Lines with a feedback
   program break the frame either way (hardware): don't discard on lines.

Open: the atst8 / depthf / pcoeff operand meanings, the 0x22 iterator
source, why lines break, and whether an unconditional discard
(dc_always: USP header flags A 8 clear) behaves differently.

## What a compiler needs

A summary for a backend (a future Mesa gallium driver). Everything here is
backed above; this section only collects it.

### Register files and operand limits

- **Files**: see "Registers". Allocatable temporaries are r0-r123: r124-r127
  are the internal registers i0-i3 (efo, fddp, fdual and the sample code the
  driver generates use them; the vendor compiler saves live ones around
  samples: corpus).
- **Slots**: a destination takes r/o/pa/indexed, or with its extension bit
  sa/g/idx/i (some forms have no extension bit: fddp, fdual, efo). In the
  float classes src0 is r or pa only; constants (c, g), immediates and
  internal registers go in src1/src2. src0 can't be indexed.
- **Immediates**: 7 bits in a source (signed in some integer forms); a
  bitwise op's src2 takes a 16-bit rotated immediate; anything bigger is
  `limm` or a secondary attribute.
- **Format select** (float classes, h22; test, h18): any F16 source switches
  the whole instruction to 6-bit numbers, so every source of it must be
  r0-r63 (etc.); an F16 source's number counts halves (`2*reg + half`,
  written `r0.f16.lo/.hi`), so F16 sources must live in r0-r31 / pa0-pa31,
  and a repeat steps them by halves (hardware). efo's F16 operands under
  format control work the same way.
- **Rounding** (hardware): fmad rounds the product to f32 before the add
  (not fused); fmad16 and its f32 -> F16 swizzle round to nearest;
  `pck.f16.f32` truncates; `pck.f32.f32 [rint]` takes rnd (nearest-even,
  down, up, zero).
- **Format control** (`setfc`, USP code block flags): while on (the
  compiler's default state is C10 on, EFO F16 off), number bit 6 of the
  color/integer classes' operands (efo, sop2/sop3/sopwm, dot, ima8, fpma)
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
- **Constants past sa127**: an operand names sa0-sa127, so a program's
  constants from sa9/sa10 (and a fragment program's dependent texture state
  after them) must end there. Ours keeps dynamically indexed and the most-used
  constants in SA and loads the rest from a memory block per use: `ld.a.d
  rN, sa<base>, #i`, `wdf`, then rN in place of the SA operand (same half,
  lane and format). One base register reaches 128 dwords (see "Address"
  above for the +1). An indexed array that doesn't fit whole keeps its
  index (clamped to the block with `fminmax`) and adds it to the base:
  `pck.s16.f32 t, x`, `imae.sgn a, t, #4, sa<base> [t2=u32]`, then `ld.a.d
  rN, a, #i` per component, so only immediate offsets are used. The DDK's equivalent is list B (USP header 0x2c flag
  1, 0x38, 0x52; **unconfirmed**); none of the corpora use it.
- **Constants**: the hardware constant table covers 0, 1, powers of two,
  1/2^n, e, π fractions, √2, F16 (1, 1) (c45-c47), 0xffffffff (c56-c59)
  (hardware); anything else is a uniform in sa or a `limm` (hardware: any
  32-bit value into r). Negation and abs are free on most float sources (not
  on src0's negate: fold it into another source as the vendor compiler
  does, corpus).
- **Scalar functions** (hardware, hwt_p3): frcp/frsq/fsqrt/flog2/fexp2 are
  correctly rounded or within an ulp on the values tried (1/3 =
  0x3eaaaaab, rsq(2) = 0x3f3504f3); 1/±0 = ±inf, rsq(0) = inf, log2(0) =
  -inf, rsq/log2/sqrt of a negative = 0x7fffffff, exp2(128) = inf,
  exp2(-126.5) = 0 (denormal results flush to zero). **fsin(t) = sin(t·π/2)
  and fcos(t) = cos(t·π/2)** with period 4 in t (fsin(1) = 1, fsin(2) = -0,
  fsin(4.5) = sin(π/4)); they are only about 16 bits accurate (fsin(0.5) =
  0.7071228 vs 0.7071068), so the compiler's range reduction to [-2, 2)
  then `* c36` (2/π) is what GLSL sin(x) needs.

### Program structure and calling conventions

- A shader is a **secondary update program** (runs once per draw: loads
  uniforms from memory with `ld`, waits with `wdf`, and does uniform-only
  math; its pa registers are the main program's sa) plus the **main
  program**. Every secondary program starts `nop [nosched]`,
  `test.and.ne p0, #0, #0`, `(p0) ld ...`, `wdf`.
- The last instruction of a finalized program has `end` (h18; hardware).
  USP code leaves that to the finalizer.
- **Branches** (hardware): `br` offset = target index - this instruction's
  index, signed 18 bits, in instructions; `ba` is absolute: the instruction
  index from the task's USE code base (the program's load offset / 8 plus
  the label). Conditions are a `test` writing p0-p3, then a predicated `br`.
- **Calls**: `br [link]` to the function's label; the function returns with
  `lapc`; arguments and results in ordinary registers.
- **Loops**: do-while with a float counter (`fmad r, r, c52, c52` = +1) and
  `test.fsub.lt` against the bound; a guard branch when the trip count may
  be zero; `break` = predicated forward branch; `continue` = predication.
  The vendor compiler unrolls nothing and uses no hardware loop counter
  (corpus).
- **if/else**: the vendor compiler if-converts (both sides, predicated
  `mov`), even for large bodies; branches only around texture reads
  (corpus).
- **discard**: a `test` writes the keep predicate (p1, the inverted
  condition); the USP holds a `nop` at the feedback label. At draw time the
  program becomes two phases: `phas` + `pcoeff` at the start, the nop
  replaced by `atst8 ... [end]` predicated on p1 (h9-10 = 2; a false
  predicate kills, hardware), the rest in phase 1, and ISPA pass type 4.
  Not on lines: any feedback program breaks a lines draw (hardware). See "Phases, visibility feedback and
  discard" for the words and what is a guess.
- **gl_FragDepth** (not in GLES 2, so no corpus): the DDK's points path
  builds `(p1) depthf pa<z>, sa3, sa4 [end]` for the feedback nop's place,
  pa<z> the iterated z (above; with it in place, points discard on
  hardware). Our compiler leaves
  gl_FragDepth, clamped to [0, 1], in a pa register at the feedback nop for
  a `depthf` there, pass type 5. **depthf's src0 is the depth written**
  (**hardware**, test H10, 2026-11-12, below); what sa3/sa4 mean beyond
  carrying the depth test words is not traced.
  - The words for it: lo `0xf0000184 | depth_pa << 14` (src0 = pa<depth_pa>
    in l14-20, src1/src2 = sa3/sa4 as in atst8), hi `0xfb340404` for
    `(p1) depthf` (h9-10 = 2). In a program without a discard the compiler
    computes no p1, so `(p1)` would test whatever p1 holds: such a program
    uses h9-10 = 0, hi `0xfb340004`, and every pixel is kept (**hardware**,
    H10).
  - The program around it (the open driver's, `upload_fs` in the winsys):
    phase 0 = `phas.i #phase1, #t [wait=1]`, then `pcoeff` when the program
    also discards, otherwise `mov pa0, pa0 [skipinv rcnt]` in its place (as
    the vendor driver's points path); the shader up to the feedback nop;
    `depthf` [end] there; phase 1 = the colour out and `phas.i #0, #0 [end]`.
    The draw's ISPA pass type is 5 for triangles too; sa3/sa4 hold the same
    depth words as a discard's atst8 (STATE.md "Alpha test (ES1)").
  - **H10** (a CE5300 board, `mesa/tests/sgx_fragdepth.c`): a 128×64 FBO
    with 24-bit depth; a full quad at depth 0.5 writes gl_FragDepth =
    x / 128 (from gl_FragCoord, or from a varying), depth test ALWAYS,
    green; a second quad at 0.5 with LESS, red. Read back colour and depth
    (ZLS). Without depthf every pixel stores 0.5 and the red quad covers
    nothing. With it all three cases are exact, 8064 of 8064 pixels each
    (the middle two columns left out), worst depth error 0: the ramp left of
    the middle, 0.5 under the red right half; with a discard of the top 16
    rows (`(p1) depthf`), those rows keep the clear and then take the red
    quad. piglit's `glsl-1.10-fragdepth`, `glsl-bug-22603`, `drawpix-z`,
    `early-z` and the `depthstencil-default_fb-copypixels` and
    `-drawpixels-float-and-ushort` tests at 1, 2 and 4 samples all pass
    (they failed without it); no GPU resets.
  - The clamp runs before the colour is written: the colour may be packed
    over an input the depth reads (pa0), so depth_pa is past every register
    the colour can take, `max(inputs, 1) + 1`.
  - The test: piglit `glsl-1.10-fragdepth` and `glsl-bug-22603` (a shader
    depth against a depth-tested second draw, read back as colour); the
    `*-copypixels` / `*-drawpixels` depth tests also need depth loaded and
    stored between renders (ZLS), so they test both.
- **Derivatives** (corpus c3 dsx, dsy, fwidth): `fdsx d, x, x` / `fdsy d,
  x, x`, one per component, never repeated. The vendor compiler multiplies
  dFdy's operand by a driver constant first (the target's y direction; Mesa's st
  does the same). What feeds a derivative runs with skipinv clear (that
  fmad, and tx_dep's coordinate math, where the LOD needs the neighbors):
  a 2x2 neighbor outside the primitive is an invalid instance, and a
  skipinv instruction leaves its register stale. The derivative itself
  keeps skipinv. `d` must not be a register `x` uses: the instances of a
  quad don't all read before any writes, so a neighbor can read the
  derivative instead of `x` (the DDK never overlaps them). **Hardware**
  (piglit): with skipinv on everything, dFdx of an iterated value was
  right while dFdy of an `fmul` and either derivative of an `abs` were
  wrong. With the feeding math skipinv-clear, `glsl-fwidth` passes, and
  every remaining failure was a derivative whose `d` was `x` (`fdsy r4, r4,
  r4`), while each that wrote another register was right. The allocator now
  makes a derivative's destination live from its own instruction's start
  (as a repeat's is), so `d` is never one of its sources. `glsl-derivs` and
  `glsl-fwidth` pass on hardware (2026-11-10), and so do `glsl-derivs-abs`,
  `-abs-sign`, `-sign`, `-swizzle` and `-varyings` (2026-11-11, a CE5300 board).
  `fs-dfdx-accuracy` and `fs-dfdy-accuracy` give Warn: they expect
  `dFdx(x * y)` of gl_FragCoord to be exactly `y` under the NICEST hint,
  which only per-pixel differencing gives. That fits fdsx/fdsy differencing
  per 2x2 quad (coarse; **guess**, not yet read back per pixel), which GL
  allows. No fine form is known.
- **Logic ops** (GL 1.1 `glLogicOp`; piglit `gl-1.0-logicop`, 16 subtests,
  `gl-1.1-xor`, `gl-1.1-xor-copypixels`): there is no fixed-function logic
  op, as there is no blender. Like blending they are code after the colour,
  in a translucent pass where o0 starts as the tile's colour (STATE.md
  "Blending and the color mask are shader code"), on the packed BGRA8 word.
  The bitwise class has and, or and xor with src2 optionally inverted
  (`.inv` on the mnemonic; src1 takes no modifier), each **hardware**
  (hwt_p7 `bit_*`, `and.inv #1`), and an immediate src2 is inverted too.
  With S = pa<c> (the colour) and D = o0:

  | GL | code |
  |---|---|
  | CLEAR | `and o0, o0, #0` |
  | AND | `and o0, pa<c>, o0` |
  | AND_REVERSE (S & ~D) | `and.inv o0, pa<c>, o0` |
  | COPY | nothing (the plain colour write) |
  | AND_INVERTED (~S & D) | `and.inv o0, o0, pa<c>` |
  | NOOP | `mov o0, o0` (or write nothing: colour mask 0) |
  | XOR | `xor o0, pa<c>, o0` |
  | OR | `or o0, pa<c>, o0` |
  | NOR | `or r, pa<c>, o0`; `xor.inv o0, r, #0` |
  | EQUIV (~(S ^ D)) | `xor.inv o0, pa<c>, o0` |
  | INVERT | `xor.inv o0, o0, #0` |
  | OR_REVERSE (S \| ~D) | `or.inv o0, pa<c>, o0` |
  | COPY_INVERTED | `xor.inv o0, pa<c>, #0` |
  | OR_INVERTED (~S \| D) | `or.inv o0, o0, pa<c>` |
  | NAND | `and r, pa<c>, o0`; `xor.inv o0, r, #0` |
  | SET | `or.inv o0, o0, #0` |

  Every row assembles and round-trips (`tools/isa/asm.py`; with c = 0,
  XOR is `58041001 90000000`). The last instruction carries `end`. The ops:
  **hardware**; that a bitwise
  op may write o0 (its dst slot takes o: oracle) and read the tile there in
  a translucent pass as sop2 does: **guess**, to be confirmed by a DDK ES1
  capture with `glLogicOp` (`tools/probe/probe1.c` PROBE_LOGICOP) or by the
  piglit tests on the open driver. With a colour mask the logic op goes
  first, then the masked move. Only for unsigned normalized colour buffers
  (GL ignores the logic op for float ones).
- **User clip planes** are clipped by the TA (STATE.md "Clip planes, shading,
  polygon and line state": capture `cp1`, `cp2`, `cp6`). For plane n the DDK's
  ES1 vertex program ends with `fdpc o(7+n), cp<n>, pa0, c52 [rpt=2 inc0
  inc1]` (words `1081d001 90e04034` for n = 0): pa0 holds the distance
  (computed by efos from the eye-space position and the planes, which arrive
  as constants), pa1 = 0, and c52-c53 are 1.0, so the dot product is the
  distance. fdpc writes only its last iteration's destination, as fdp does
  (hardware, hwt_p7), so plane n's result is o(8+n), and output select's
  dword count, 8 + n, agrees. Output select also gets b0-5 = the planes.
  - The compiler: st keeps lowering the planes and gl_ClipVertex to clip
    distances in the vertex shader (`nir_lower_clip_vs`, driver
    `clip_planes` 0: planes 0..n-1, a disabled one below the last 0.0), and
    each distance becomes `fdpc o<clip_o + p - 1>, cp<p>, (d, 0), c52
    [rpt=2]` after every other output (`sgx_shader.n_clip`, `clip_o`). The
    driver sets output select b0-5 to planes 0..n_clip-1 and counts n_clip
    more dwords. Lines clip too: no discard.
  - **Guesses** until a hardware draw: that a plane enabled with a 0.0
    distance keeps everything (a point on the plane is inside); that the
    result registers follow the enabled bits in order.
  - `sgx_lower_clip_fs` (the per-pixel fallback) stays for a core without
    the TA path.
- **Polygon stipple**: `nir_lower_pstipple_fs` (a 32x32 texture read at
  gl_FragCoord and a discard): nothing new for the compiler.
- **Phases**: every finalized fragment program (main and secondary) starts
  with a `phas.i` (`phas.i #0, #0 [wait=7]` for a one-phase program;
  hardware) and ends with `end` on its last instruction or an appended
  `nop [end]`; a feedback program ends with `phas.i #0, #0 [end wait=7]`.
- **Loads and samples** set a DRC (drc0/drc1): `wdf drc` before the first
  use of the result; alternate DRCs to keep two in flight.

### Comparisons and conversions

- Float compare: `test.fsub.<cond> pN, a, b` with lt/le/eq/ne/gt/ge on a-b
  (hardware; F16 operands are half-numbered, chan 0);
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

- **gl_FrontFacing** comes from special register g16: bit 0 is set for a
  primitive that is clockwise in memory (**capture**, one draw shape,
  y-flipped window; other bits not examined). The DDK reads it with
  `test.and.ne p0, g16, #1` and a per-draw constant in sa9 (only 0 captured;
  nonzero presumably inverts). Our compiler: `xor r, g16, sa[front_sa]; and r,
  r, #1; movc.nz.i32 f, r, c52, c48`, with the driver loading sa[front_sa]
  bit 0 = 1 when counter-clockwise is front. Not yet drawn by our own programs.

- Varyings arrive iterated in pa (USP list A: one entry per iterated input,
  with its precision); non-dependent texture results also land in pa (PDS
  DOUTI/DOUTT, docs/PDS.md).
- The color is built in pa0 as packed BGRA8: two `pck.u8.f32 ... scale`
  with destination byte masks (byte 0 B, 1 G, 2 R, 3 A: hardware); the
  finalized program writes the color to o0 (hardware, the probe). The USP
  header (0x66-0x6c) says where the compiler left it; blending and the
  color mask are code the driver appends, reading it from there.
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
  dword, when the shader writes it (**corpus**; the open stack's programs
  use this layout and pass dEQP-GLES2: **hardware**). The driver builds
  the TA words from the vertex shader's sets: output select =
  `dwords << 16 | 0x3000`, `| 0x100` with point size (dwords = 4 + sizes +
  1); TSP word 0 = 3 bits per set (1, 2, 3, or 5 for 4 components); TSP
  word 1 = the sets with precision value < 3 (**capture**, docs/STATE.md
  "Output select and TSP words" and "Lines and points"). The USP has none
  of this: its header has no output count.
- **Varyings to sets**: the vendor compiler has 10 sets × 4 components. It
  takes the active varyings by type, mat4, mat2, vec4, mat3, vec3, vec2,
  float, within a type the largest first (by array size: guess), and gives
  each row a new set while sets remain; past 10 it packs rows into free
  components of sets with the same precision (**unconfirmed**; corpus:
  vo_vall's o0, o1, o2 (vec2), o3 (float) land at o4, o8, o12, o14 = sets
  0-3 of sizes 4, 4, 2, 1). Our `sgx_pack_varyings` does this without the
  precision rule: each stage is placed on its own, and their precisions
  may differ. Unwritten varyings get no set.
- **gl_PointSize**: when a vertex shader writes it, the vendor compiler
  reserves set 0 *before* allocating varyings: enabled, 2 components
  (**corpus**; precision 1: **unconfirmed**). The shader never writes it,
  so o4-o5 are a hole and varyings start at o6, with the point size last
  (vo_ps: o6; vo_psv: vec4 at o6-o9, size at o10) (**corpus**). Guess: set
  0 is where the hardware puts the point-sprite coordinate (gl_PointCoord)
  for points. The size is clamped first: the vendor compiler emits
  `fminmax oN, x, x, saK.f16.lo` against the driver's range in sa, a min
  against 511.0 (**corpus** vo_ps; STATE.md "Lines and points").
- **Matching the fragment shader**: each shader is allocated on its own;
  the vendor GL linker matches varyings by name (type, size and
  invariance must agree) and keeps a per-program table from each
  fragment-side set to the vertex-side set. The draw-time DOUTI for a
  varying takes source = the vertex-side set + 1, so the fragment program
  reads the vertex shader's set into its own PA order (**unconfirmed**). A
  compiler that compiles both stages can give a varying the same set on
  both sides and needs no table.
- **What a compiler must produce**: position in o0-o3; each varying in
  the o registers of its set (4 + the sizes of the lower sets); the point
  size, if written, after the last set, with set 0 reserved (2 dwords,
  unwritten) ahead of the varyings; and to the driver/TA: the set mask,
  sizes and precisions (output select and TSP words above). Outputs can be
  read back as sources.
- Vertex texture fetch is always a USSE `smp` with LOD (c48 = 0.0 for
  `texture2D`).

## Gaps

Facts above tagged **unconfirmed**, each with the hardware test or capture that would
confirm it. Shipping code must not rely on one until it is confirmed. The USP
container items matter only to the A/B harness that runs our programs under the
vendor driver; the open stack doesn't use the container.

### Registers and encoding

- Registers: the pa budget is 0x80 minus USP header +0x3c → a hardware draw whose program uses pa registers right up to (and one past) that bound, or a capture of the DDK's PDS word 1 / attribute count for a program near it.

### Float, moves, dual issue, pack, test, bitwise

- Common fields: skipinv (h23) skips invalid instances → hwt draw over a partly covered triangle (helper/invalid instances) with an observable side effect (st to memory, or a predicate feeding o0), with and without h23.
- Common fields: syncs (h20) is a sync start → hwt draw with a derivative or dependent sample after a divergent branch, with and without h20, checking the result per pixel.
- Common fields: nosched (h11 in the float classes, h22 in pck/test/bitwise) stops the scheduler from descheduling → hwt run with an internal-register write, a long-latency op (smp or ld) and a read, with and without nosched, many pixels.
- Float classes, fdpc: writes a clip-plane result to cp<n> (l15-17) → capture a vendor vertex program with user clip planes and its state, or an hwt vertex draw that toggles the cp operand and watches clipping.
- Float classes, fddp/fddpc: what the two dot products sum (the second goes to the i<n> in h19: hardware), and fddpc's writes to the two cp operands → hwt fddp with vector inputs and a repeat; fddpc with distinct cp operands.
- Moves, op0xf3 (h8-10 = 6, 7): writes an output → hwt draw of each form, reading o and r registers back.
- fdual: internal registers don't survive a deschedule (not reproduced across loads and a wdf) → hwt draw that writes i0, runs a dependent smp, then reads i0, many pixels.
- fdual, fssq: each source channel feeds the same destination channel → hwt fssq with rpt=2 (or a mask) over distinct registers, checking each destination.

### Integer, fixed-point and multi-unit ops

- efo: h22 (a0lneg) negates A0's left input (set for an add's other source negate) → hwt draw with `a` modes whose A0 left input is a product or I0 (a=0, 1, 2), a0lneg on and off, nonzero inputs
- efo: earlier cores' EFO use h3-8 as three 2-bit source modifiers, with different control-field meanings → oracle sweep with a pre-SGX545 target, plus hardware on such a core
- sop3: h11 (arev / arsop) negates the alpha result, swapping the roles to b - a → hwt_p6-style U8 draw of sop3 with arev on and off, distinct src alphas
- ima16: the vendor compiler picks u8 format exactly when the source is an immediate → corpus/capture check of every ima16 with an immediate or register source (any with a non-u8 immediate or u8 register?)
- hw 16 ssum16i, firh, op16s1, op1adrc (oracle, not emitted for SGX545): whether the SGX545 executes them at all, and what they compute → hwt draws of each word (watch for a hang/HWR), compare against candidate formulas

### smp, ld/st and the special class

- hw 1c: smp: pcf (h5) / pcf16 (h6) are a percentage-closer compare → hwt sample of a depth texture with compare state set, pcf on and off, F32 and F16 coordinates; compare the result to the reference compare.
- hw 1d: eld: size 0 = dword, 1 = quad (what each size loads) → hwt eld.d / eld.q from a known buffer, count the registers written.
- hw 1f: emit: t1 is a geometry-shader vertex emit and its f13 is "emit and cut" → capture of a vendor program using emit.t1, or a hwt draw on a core that runs geometry; low priority on the SGX545 (GLES2 has no geometry stage).
- Open questions: br/ba h10 (f1_22) and op0xd7's limit are tied to the same core feature → hwt br with h10 set vs clear, and op0xd7 n sweep, looking for any behavior difference.

### Phases, discard, constants and vertex shaders

- What the finalizer prepares: USP header flags B 2 (last instruction can't carry `end`) makes the driver append `nop [end]` → a capture of the finalized program of a corpus binary with flags B 2 set.
- What the finalizer prepares: USP header flags B 4 sets the phase-0 `phas` h13 → a capture of a finalized feedback program from a binary with flags B 4, and a hardware draw with h13 set and clear.
- What the finalizer prepares: in a feedback program (flags A 8) whose blend code has a phase of its own, the driver adds one more `phas` → a capture of the DDK's program for a blended discarding shader.
- What the finalizer prepares / The draw state: atst8's c is the color register after the finalizer's move → a capture of a DDK discard program whose USP color register is not pa0 (dc_gt: pa2).
- The draw state: discard on points or lines gets ISPA pass type 5 from the stock driver → a capture of the ISPA word of a stock points+discard draw.
- What the draw-time builder emits: blend and color-mask code sometimes gets a phase of its own (a `phas` to it, then `phas.i #0,#0`), and which draws do it → captures of the DDK's finalized programs for blended draws with and without discard, color mask on and off.
- What the draw-time builder emits: h11 (`rate=1`) on the phase-0 `phas` when the blend code runs in a phase of its own → the same captures; the open stack uses `[wait=1]` without h11, so a hardware A/B of the two words.
- What the draw-time builder emits: the feedback iteration is DOUTI `0x7c11000` (4 components) instead of `0x7811000` in the draws whose blend code has its own phase → a capture of the PDS primary of such a draw.
- What the draw-time builder emits: for points or lines the stock driver adds a second iteration (`0x7800000`, source 0, 3 components), puts `mov pa0, pa0` in pcoeff's slot, leaves the discard `nop` in the program and corrupts its temporary count to 0xfb340404 → a capture of the stock driver's program, DOUTU and PDS words for a points+discard draw.
- Constants past sa127: the DDK's list B (USP header 0x2c flag 1, 0x38, 0x52) is its way to keep constants in memory → a corpus shader with more uniforms than fit in SA, or a hand-built USP binary with list B entries finalized and drawn by the DDK, then the capture.
- Vertex shaders: the vendor compiler assigns sets by type order (mat4, mat2, vec4, mat3, vec3, vec2, float), largest first within a type, and past 10 sets packs rows into free components of same-precision sets → corpus shaders with mixed types and with more than 10 varying rows at mixed precisions, decoded for their output registers.
- Vertex shaders: the reserved gl_PointSize set 0 has precision 1 → the TSP word 1 bit 0 in a capture of a point draw (`ptattr`, `ptvar`).
- Vertex shaders: the vendor GL linker maps each fragment-side set to the vertex-side set, and the DOUTI source is the vertex-side set + 1 → a capture of a draw whose vertex and fragment shaders declare the same varyings in different orders or types, reading the DOUTI sources.

### The USP container and the vendor driver's finalizer (A/B harness only)

- Where the blocks come from: the driver's loader treats block types 0 and 8 as an end of the stream → A/B: a USP whose stream ends with type 0 or 8 instead of 7, and check that it loads and draws.
- Shared sub-records: MOE mode `0x4000|swz` (four 2-bit swizzle selects) → A/B: a USP with a swizzle-mode MOE word in a code block, and the vendor driver's finalized program captured.
- Shared sub-records: iflags b0 makes the driver save and restore i0..i2 around inserted code → capture the finalized program of a corpus shader with ilive 3 (tx_dep_proj3) and look for the saves.
- Shared sub-records: with iflags b2 clear, every driver-generated instruction gets skipinv → capture finalized tx_dep_chain vs tx_dep and compare skipinv on the generated smp/wdf/pck.
- Shared sub-records: iflags b4-7 (meaning unknown, written only under a compile option) → A/B: set them in our USP and diff the finalized program.
- Shared sub-records: 4-valued register types 2 = o and 4 = i → A/B: a block 6 whose destination is o or i, and check the result on the CE5300 board.
- Block 5: flags b2 is matched against an iterator key (centroid guess) → A/B: set b2 on a non-dependent sample and capture the PDS DOUTI the driver builds.
- Block 5: sampler > 15 rejected → A/B: a USP with sampler 16 and check the load fails.
- Block 5: swizzle bits 12-15 must be 0; selects >= 4 unseen → A/B: swizzles with a select of 4-7 and with bit 12 set, capture the finalized unpack.
- Block 5: usrc equal to the unpack's source and destination means unpack in place → A/B: usrc different from the unpack source, capture the finalized program.
- Block 5: scratch pair is the first temporary the driver's own code uses → capture a finalized sample with live internal registers and check which r registers the saves use.
- Block 5: ilive C10 subset saved with two instructions instead of a mov → A/B: a sample with a C10 value live in i0, capture the finalized program.
- Block 5: forced format values other than 8 (f32, f16, c10, u8, 6, 7) → A/B: each value with an RGBA8 texture, read back the result.
- Block 5: non-dependent coordinate iterator accepted range 1..10 → A/B: iterator 11 and check the load fails.
- Block 5: the driver substitutes template `0x20000000 / 0xe0411000` for non-dependent chunks done by smp → capture the finalized program of a non-dependent read of a texel format wider than 32 bits.
- Block 6: per-format byte-offset bounds checked by the loader → A/B: an out-of-range offset (f16 offset 1, u8 offset 4) and check the load fails.
- Block 9: the whole 82-byte layout and its format codes → no GLES2 source; only an A/B with a hand-built block 9, or leave unsupported.
- The finalizer: texture formats split into up to 4 chunks of ceil(bits/32) dwords; constant 0/1 channels need none → capture finalized programs for a dependent read of a 64-bit and a 128-bit format (e.g. RGBA16F, RGBA32F) and of LUMINANCE/ALPHA.
- The finalizer: a YUV-like format adds a FIRH color conversion → capture a finalized program sampling a YUV texture (EGL image).
- The finalizer: the PDS sample budget is `0x80 -` header 0x42 → A/B: header 0x42 non-zero with many non-dependent samples, capture the iterator list.
- The finalizer: only the first needed chunk of a non-dependent sample comes from the PDS → capture a non-dependent read of a multi-chunk format (PDS program and USSE program).
- The finalizer: texture state goes to memory when it doesn't fit below header 0x3c + 0x3e, then `ld` (hi `0xe8800000 | (n-1)<<12`) + `wdf`, merging chunks up to 16 dwords; which header field gives the base SA → A/B: a small 0x3e with dependent samples, capture the finalized program and the memory it reads.
- The finalizer: non-default MOE state at a sample gets `smbo 0,0,0,0` / `smlsi 1,1,1,1` first and is restored after → capture finalized tx_dep_loop.
- The finalizer: coordinate fix-up (`fsubflr`, `limm` 1/w and 0.5/w, multiply-add) for some samplers (NPOT/rectangle guess) → capture finalized programs for a POT and an NPOT texture with repeat wrap.
- The finalizer: one texture format gets `and dst, dst, #0x7fffffff` on the result → capture finalized programs across the texture formats and find which one.
- The finalizer: unpack paths for s8, u16, s16, f16, f32 texel channels, `mov` for f32, c10 targets, formats 6/7 (`limm 0xffff8000`, test) → capture finalized programs for each texel format and result precision.
- The finalizer: how the driver writes the texture-state table into SA registers at draw time → capture the secondary program / PDS DOUTD of a draw with a dependent read.
- The texture-sample sequence: LOD reads, gradient operand layout (how many registers, which order) → hwt: an smp.2d.grad with known derivatives against a mipmapped texture, read the level chosen.
- The texture-sample sequence: vertex texture fetch with LOD c48 and skipinv → hwt or open-stack vertex texture test on the CE5300 board.
- Header: version other than 0x23 rejected → A/B: version 0x22.
- Header: program type 2 = geometry → none on GLES2; leave unsupported.
- Header: flags A 4 (color may stay in pa0 in output mode 3) and 0x10 → A/B: set each and capture the finalized program.
- Header: compile flags b5 = the driver inserts visibility feedback → A/B: a discard shader without b5, capture the finalized program.
- Header: flags B 2 (last instruction cannot take `end`), 4 (draw-time `phas` h13), 8 / 0x10 (one more color register), finalizer's 0x100 on an out-of-range branch → A/B: toggle each and capture the finalized program and phas words.
- Header: 0x20 accepted range 1..10 → A/B: value 11.
- Header: workaround ids 0x54c1, 0x54d1, 0x5518 under compile options → A/B: a USP with one id, capture what changes in the finalized program.
- Header: flags C 1 (list B) and 4 (memory area at 0x34/0x60), and fields 0x30, 0x38, 0x60 → A/B: a USP with a list B record and a memory area, capture where the constants land.
- Header: 0x3e moves texture state to memory when exceeded → as the finalizer's memory fallback above.
- Header: the finalizer raises 0x46 for its own code → capture the finalized program's temporary count (DOUTU) for a dependent read.
- Header: color alternatives 0x6e/0x70/0x72 and output modes 0-3 (blend/color mask keep the color; otherwise moved to o) → A/B: draws with and without blending, capture the finalized color move.
- Header: the loader opens a code block at the start label for the finalizer's placeholders → capture the finalized program's first instructions for a discard and a non-discard shader.
- Lists and blocks: list A kinds 0xb/0xc iterate from DOUTI source 0x20 (color), 0xd-0xf from source 0 → capture the PDS program for a shader with each kind.
- Lists and blocks: list C kind 4 (32-bit value, 8 bytes) and list B → A/B: a USP with a kind 4 record and one in list B.
- Lists and blocks: list D entry bounds (<= 16, <= 4) → A/B: a USP with one list D entry.
- Lists and blocks: vertex outputs renumbered by the output mask's set bits → A/B: a vertex USP with a sparse output mask, read the varyings.
