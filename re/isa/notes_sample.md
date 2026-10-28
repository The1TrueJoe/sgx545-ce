# USP texture-sample blocks and the driver's sample finalizer

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

## Where the blocks come from

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

### Shared sub-records

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

## Block 5: SAMPLE (56 bytes)

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

## Block 6: SAMPLE UNPACK (74 bytes)

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

## Block 9: TEXTURE WRITE (82 bytes; was 78 in usp.py)

20 u32 then one u16, in this order (writer case 0xa6, reader 0x513c7): IR
params +0x2c (an id); eight (register number, operand format) pairs for IR
args 0..7 (format code from `FUN_00103f30`: USEASM format 0→1, 2→2, 3→3,
6→4, 7→5; anything else is an error); IR params +4; a format from table
0x136c58 (IR params +0x70: 0..5 → 1, 2, 3, 4, 7, 6); `FUN_0009af00()`;
iflags. The reader keeps them in a 0x324-byte record (list at shader +0x34).
GLSL ES never produces it; the field meanings beyond the layout are
**unknown**.

## The finalizer (libGLESv2)

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

## The smp instruction, as libusc's FUN_0010a3f0 writes it

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

## The texture-sample sequence a compiler must emit

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

## Open

- Block 5 flag b2 (IR sample +0x9c) and the "usrc" pair: guessed meanings.
- iflags b4-7; smp h5 and the h14-15 path; forced formats 6 and 7.
- Which GL code writes the exported texture-state table into SA registers
  (not traced past `FUN_00054da0`, which builds the finalized program).
- `FUN_00066760` (1121 lines) and `FUN_0005fcc0` cases 6/7 were only skimmed.
- Block 9 field meanings; never produced by GLSL ES.
- Gradient operand layout (how many registers, which order): one corpus case.
