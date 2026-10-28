# SGX545 PDS programs

The PDS (programmable data sequencer) runs a small program before each USSE
task. It fetches vertex data by DMA, sets up the iterators that interpolate
varyings, hands texture state to the texture unit for non-dependent reads,
and then starts the USSE program. This file is what we know of its programs,
their data segments and their instruction set on the Cedarview DDK 1.7 SGX545.

`dis.py` disassembles every PDS program in a capture:

    python3 re/pds/dis.py CAPTURE.bin [--kick N] [--raw]
    python3 re/pds/dis.py --words 87000000 90000006 07000203 af000000

The status tags used below:

- **confirmed:** read from the DDK code that writes the field (the generator
  formula or a builder in libGLESv2), and seen in captures to mean the same
  thing. A few are confirmed from captures alone, and say so.
- **strong:** consistent across every capture and every generator use, but
  nothing names it.
- **guess:** a reading that fits what we have. It could be wrong.

## Where the knowledge comes from

- **The DDK's PDS generator.** It is one library, linked statically into
  libsrv_um, libGLESv2, libGLES_CM and libOpenVG; their copies are identical.
  Ghidra headless decompiled it (Ghidra 12.1.2, openjdk@21, script
  `DumpDecomp.java`, which prints every function in an address range).
  Addresses here are libsrv_um virtual addresses; libGLESv2 has the same
  functions at +0x1e4d0. The instruction words are built from literal bases
  ORed with shifted fields, so the encoders give the field positions directly.
- **The static programs in libsrv_um .rodata** (0x229f0–0x23420): the
  pixel-event program, plus the transfer-queue programs.
- **libGLESv2 callers.** These show how GL fills the DOUTU, DOUTI and VDM
  words: the clear path at 0x1c560, the DOUTI builder at 0x36b00, the
  VDM-info helper at 0x7c20, and the VDM state emitters at 0x38eb0 and
  0x397f0.
- **13 captures:** the six old ones (`base red clr mov two ref`) and seven
  made with `probe-pds` (listed at the end).

## Program layout (confirmed)

A program is a **data segment** followed by **code**:

    addr + 0x00: data row 0:  DS0[0]  DS0[1]  DS1[0]  DS1[1]
    addr + 0x10: data row 1:  DS0[2]  DS0[3]  DS1[2]  DS1[3]
    ...
    addr + 16*rows: code, 32-bit instructions, ending in HALT (af000000)

- **Two data stores, DS0 and DS1.** Each is a bank of 32-bit words, and their
  words are interleaved two at a time in memory. DS0 word *n* sits at memory
  word `(n>>1)*4 + (n&1)`; DS1 word *n* sits at `(n>>1)*4 + 2 + (n&1)`.
  - The generator's allocator (0x1e340) hands out the next free word from
    whichever bank is shorter, so a run of words fills memory in order.
  - The data size helper is 0x1e290.
- **The data segment is rounded up to 16 bytes,** and the code follows it
  directly (generators: `code = data + (ndwords*4 + 15 & ~15)`).
- **Temporaries.** Words 0x30 and up of each bank are temporaries: they are
  never in the data segment, and the programs write them. The disassembler
  prints them as `ds0[0x30]` and so on.
  - DS0 0x30 and 0x31: the generators' scratch registers (strong).
  - DS1 0x30 and 0x32: the vertex index × stride product and the pixel-event
    scratch (strong).
- **Inputs.** DS0 words 0x40 and up are inputs, printed as `inN` (strong).
  - Vertex programs: `in0` is the vertex index and `in1` the instance index.
    This is confirmed: the generator picks `in1` exactly when the stream is
    per-instance.
  - Pixel-event program: `in0` holds the tile position (it is shifted and
    masked into the EOT task word), `in1` holds the event flags (it is tested
    against 8, 2, 4 and 0x100), and `in2`/`in3` are written out with DOUTA.
    All guess, from how the program uses them.

## How programs are referenced

| referrer | format | status |
|---|---|---|
| VDM stream word, type 7 or f (vertex fetch) | `type<<28 \| addr>>4`, then an info word: bits 0–6 = data rows (16 B); bits 7–21 = ceil(input attribute dwords / 4); bits 22–23 = min(n, 3), a batching value from 0x7c20; bits 24–31 = ceil(output dwords / 4) | rows confirmed (clear path); the size fields strong (0x7c20's formula, and which is input and which output from tex1, attr3 and the clear) |
| VDM stream word, type 4 or 6 (state program) | `type<<28 \| addr>>4`, then an info word: bits 26–31 = data rows; low bits `0xe100 \| (n+3)>>2` or `0x2022401` and similar | rows confirmed (0x38eb0, 0x397f0); the rest guess |
| TA/3D state word (per-object pixel program, its secondary program, the background object) | `data_dwords<<24 \| (addr − 0x2e000000)>>4`, so the PDS pixel heap base is the origin | strong: 7 programs in 13 captures, data size always right |
| the 3D command in kernel data (pixel event) | raw address `2e006000` next to `00000030`; the data size is not given and is inferred | guess |

- **In the state block, the pair is `<secondary> <word> <primary>`.**
  - The triangle's is `0800061a 160f0001 04001802`. `0x2e0061a0` is the
    secondary program (it DMAs the uniforms, then runs the secondary USSE
    program). `0x2e018020` is the primary.
  - The clear's is `04001800 00030001 08000612`.
- The background object uses `04001800 00030001 0c00060e`, from the 3D
  command.
- The middle word's meaning is unknown.

## Instruction encoding

Every instruction has these fields (confirmed for all classes below):

| bits | field |
|---|---|
| 28–31 | opcode |
| 27 | sub-op (`s`): selects between two operations of an opcode |
| 24–26 | condition: 7 = always. BRA uses 0, meaning "if p0". MOVS also uses 3; see below |

Register operands come in three numberings:

| name | width / units | space | status |
|---|---|---|---|
| **U** (unified) | 8-bit, 32-bit units | 0x00–0x3f DS0, 0x40–0x7f DS1, 0x80+ inputs. Used by ALU destinations and shift sources | strong |
| **S1** | 8-bit, 16-bit units | DS0 side: word = f>>1, half = f&1; DS0 words 0x40+ are the inputs, so vertex index lo/hi = 0x80/0x81 and instance index lo/hi = 0x82/0x83 | confirmed (MUL generators) |
| **S2** | 7-bit, 16-bit units | DS1 side: word = f>>1, half = f&1 | confirmed (MUL generators) |

So `ds1[0x30]` is U 0x70 and S2 0x60, and `ds0[0x30]` is U 0x30 and S1 0x60.

### MOVS: the DOUT instruction (opcode 0 or 2)

    31..28 op  27 s  26..24 cc  23..18 src1 row  17..13 src2 row  12..5 swizzle  4..0 dest

- **src1 row** (bits 18–23) is a DS0 64-bit pair: words 2r and 2r+1. Rows
  0x18+ are the temporaries; rows 0x20+ are the inputs (row 0x20 = in0:in1).
  Confirmed.
- **src2 row** (bits 13–17) is a DS1 pair. Row 0x18 is DS1 0x30:0x31, and
  row 0x19 is DS1 0x32:0x33. Confirmed.
- **swizzle** has four 2-bit selects, one per output word: out0 is bits
  11–12, out1 bits 9–10, out2 bits 7–8, out3 bits 5–6. The select values are
  0 = src1.lo, 1 = src1.hi, 2 = src2.lo, 3 = src2.hi. Confirmed: the helper
  at 0x1e2c0, which sends 4 consecutive data words from any word offset,
  uses all four phases.
- **dest** (bits 0–4) is the DOUT target; only 2–6 have been seen. Confirmed
  for 2–5; 6 is a guess.

| dest | name | words consumed | meaning | status |
|---|---|---|---|---|
| 2 | DOUTI | 2 | iterator setup: interpolate a varying into USSE PA registers and/or issue a texture coordinate for a sample | confirmed |
| 3 | DOUTD | 2, or 3 with the `x` bits | DMA from memory into USSE attribute registers | confirmed |
| 4 | DOUTT | 4 | texture state for the sample started by the preceding DOUTI | confirmed |
| 5 | DOUTU | 3 | start the USSE task | confirmed |
| 6 | DOUTA | 2 (pairs) | write a word (value, ctrl) to an attribute/output slot. Programs repeat it in threes (the VDM type-6 program, the transfer-queue programs), so it may feed a per-vertex FIFO | guess |

The variant bits:

- **Top nibble 2 with bit 27 set (`0x2f…`), printed `.x2.s27`: the DOUT
  also consumes word 2, which DOUTD adds to its address.** Confirmed on
  hardware:
  - A replay of ref.bin with the vertex program's `2f032343` changed to
    `07032343` (`mkreplay.py --poke 0x3040d844:0x07032343`) renders no
    triangle at all: every vertex fetched vertex 0. The unchanged replay is
    pixel-exact.
  - Which of bits 29 and 27 does it is not known. The single-bit replays ran
    on a GPU already wedged by the next experiment, so they say nothing.
  - Also seen on DOUTA pairs (`2f800466`), meaning unknown.
- **Top byte `03`, condition field 3, printed `.cc3`.** It is used only on the
  closing DOUTU of vertex-fetch programs, and every other DOUTU uses `07`.
  - **Do not set bit 26 there.** A replay with the triangle's `030403e5`
    changed to `070403e5` locked up the TA. SGX hardware recovery then failed
    in a loop (`SGXInitialise failed (24)`) until a reboot (EA3, 2026-10-28).
  - So the bit matters for vertex tasks. What it means is still a guess:
    something that a vertex task must not wait on or signal.

### Arithmetic and logic (opcodes 4, 6, c, d)

    31..28 op  27 s  26..24 cc  23..16 src1 (S1)  15..9 src2 (S2)  8..7 (0)  6..0 dst (U)

| op | s | name | evidence |
|---|---|---|---|
| 4 | 0 | ADD (32-bit) | confirmed: address + index×stride; the reciprocal-divisor sequence |
| 6 | 0 | MUL16: src1.half × src2.half, 16×16→32 | confirmed: the generator splits 32-bit products into 16-bit halves and recombines them with SHL 16 + ADD when the vertex count > 0x10000 or the stride > 0xffff |
| c | 1 | AND | strong: pixel event masks with 8, 2, 4, 0x100, 0xff8, 0x3ff000 |
| c | 0 | OR | strong: pixel event assembles the EOT word |
| d | 1 | XOR? | guess: only `df820070` = in1 op ds1[0] (= 0), then AND 0x100 |
| 4 | 1, 6 | 1, d | 0 | not seen |

### TST (opcode 8)

    31..28 8  26..24 cc  23..16 src1 (S1)  15..9 src2 (S2)  8 sel  7..0 (0)

- **Sets p0 = (src == 0),** where src is src2 if `sel` is set, else src1.
  Strong guess: the state program skips its DMA when the address word is 0,
  and the pixel event program branches when an event bit is clear.
- Examples: `87000000` tests ds0[0], `87040000` tests ds0[2], and `8700c100`
  tests ds1[0x30].

### BRA (opcode 9)

    31..28 9  26..24 cc (0 = if p0)  23..0 target instruction index (from the first code word)

Strong: all 4 targets in the pixel-event program and the VDM state programs
land on instruction boundaries, and the programs make sense.

### Shifts (opcode f)

    31..28 f  27 s (0 SHL, 1 SHR)  26..24 cc  23..16 src (U)  15..8 amount  7..0 dst (U)

Confirmed: `ff800030 | n<<8` is the instance divisor's power-of-two shift,
`f7311031` is T1 <<= 16 in the 32-bit multiply, and `ff300f30` is >> 15 in
the reciprocal divide.

### HALT

`af000000`: opcode a, s 1, cc 7. Confirmed: every program ends with it. A
program may hold several HALTs, as branch targets.

## DOUT word layouts

### DOUTU: start a USSE task (3 words)

**Word 0:**

- bits 0–3: USE code base register (`EUR_CR_USE_CODE_BASE_n`). Confirmed.
- bits 4–21: code offset / 8 inside that base's 512 KB window. Confirmed.
  - GL computes `off = pc - heap_base`, then `w0 = (off>>19) + base_index |
    (off*2 & mask)`.
  - Pixel programs use bases 0–9 at 0x37000000 + n·512K; vertex and state
    programs use bases 10+ at 0x3c000000 + (n−10)·512K.
  - Confirmed: every DOUTU in 13 captures lands on a captured USSE buffer.
- bits 24–31: `(n+3)>>2` for a per-shader count from GL, probably the
  temporary register count in fours. Guess.

**Word 1:**

- bit 24 = the pixel program has plain iterations; bit 25 = it has texture
  samples. Confirmed: 0x36b00 sets them, and the captures match: tex1
  0x02000004, vary2 0x01000004, tex2 0x03000004, and the untextured
  triangle 0x00000004.
- bit 2 is always set on pixel tasks. Guess: pixel pipe, or a sample-rate
  bit.
- Vertex and state tasks have 0.
- The pixel-event program computes word 1 at run time: an OR of the tile
  position fields, 0x00800000 and 0x00400000.

**Word 2:** 0 in every capture.

### DOUTD: DMA into USSE attributes (2 words, +1 with `x`)

- **Word 0:** the source address. Confirmed.
- **Word 1:**
  - bits 0–7: dwords − 1. Confirmed: generator `((bytes+3)>>2)-1`; 2/3/4/1
    for the vec2/vec3/vec4/float streams.
  - bits 8–21: destination attribute register. Confirmed: attr3 puts the
    streams at registers 0, 12, 4 and 8, the compiler's choice, and the
    shader reads them there.
  - bits 22–24: 1/2/3 on the state and uniform DMAs, 0 on vertex fetch.
    Unknown.
  - bit 25: set on state and uniform DMAs (secondary attributes?). Guess.
  - Generator 0x1fc30 writes `(n-1) | (m-1)<<4 | reg<<8 | (n-1)<<22`, a
    2-D form not seen in captures.
- **Word 2 (with `x`):** the byte offset added to word 0 (index × stride).
  Strong.

### DOUTI: iterator setup (2 words)

These are built by libGLESv2 0x36b00 from the compiler's iterated-input list
(the USP "list A"; see `re/isa/usp.py`). Each DOUTI has two halves, and the
pixel-program generator 0x1e970 merges a texture entry into a preceding plain
entry by copying `w0 & 0x400007ff` and `w1 & 0x7f`. Confirmed.

**Sample half** (bits 0–10, bit 30; w1 bits 0–6):

| bits | meaning | status |
|---|---|---|
| 0–4 | texture coordinate set + 1 to sample with (0 = no sample) | confirmed |
| 9 / 10 | 2-D (0x200) / projected (0x400) sample | confirmed from GL, which sets 0x200 unless the input is flagged projected; captures only show 2-D |
| 30 | last sample of the program | confirmed (0x36b00 marks the last texture entry) |
| 13, 16 (0x12000) | set on pure-sample entries (the iterate half is unused) | confirmed constant, meaning unknown |

**Iterate half** (bits 11–29):

| bits | meaning | status |
|---|---|---|
| 11–16 | source: texture coordinate set + 1 (TC slot from the GL varying map), 0x20 = colour (`0x1c10000`); 0x22 seen in a GL path not captured | confirmed |
| 21 | colour iterator at precision < 2 | confirmed (set by GL); meaning of the precision values guessed |
| 22–23 | components − 1 (forced 4 for colour) | confirmed |
| 24 | plain iteration enabled | confirmed |
| 25–26 | always set (0x6000000) | confirmed constant |
| 27 / 28 | precision 2 / precision 1 from the USP entry; mediump varyings give bit 28 | confirmed field, guess that 1 = F16 |
| 29 | last plain iteration | confirmed |

- Iterations fill the USSE PA registers in program order. The DOUTI does not
  name a destination register (strong).
- w1 bits 4/5 (0x10/0x20) are set by GL for a coordinate-replacement case
  (point sprites?), not captured.
- Examples:
  - `37c00800`: tc0 ×4, f16, last.
  - `17c00800 37401000`: tc0 ×4, then tc1 ×2 (last).
  - `37c00a02 46012203`: tc0 ×4 merged with a sample of tc1, then a sample
    of tc2 (last sample).
  - `21c10000`: the clear's colour ×4.
  - `40012001`: background object, sample tc0 with type bits 0.

### DOUTT: texture state (4 words)

- These go to the texture unit for the sample of the preceding DOUTI.
- Word 2 is the texture base address. Confirmed: 0xf0001000 and 0xf0001100
  in the texture heap; 0x3e265000, the back buffer, for the background
  object.
- Word 1: bits 0–11 = height − 1, bits 12–23 = width − 1. Confirmed:
  `cc2cf1df` holds 479 and 719 for the 720×480 back buffer, and `0d007007`
  holds 7 and 7 for an 8×8 texture.
- The rest are TPU control words (format, filter, stride), not decoded here.
- GL builds them at 0x37b90 as `(sampler word & mask) | override`, per
  texture unit.

### DOUTA (2 words, guess)

- (value, ctrl). ctrl bits 8+ look like a slot number: the static
  transfer-queue programs write rows `(0, 0x100)`, `(0, 0x200)`, …
  three times each.

## What the DDK generates

| function (libsrv_um) | program | seen in captures |
|---|---|---|
| 0x1f3d0 | **vertex fetch**: per stream `MUL16 ds1[0x32], in0/in1 . lo, stride`, then per element `MOVS.x DOUTD (addr, ctrl, ds1[0x32])`, then `MOVS.cc3 DOUTU`, HALT. Optional instance divisor (reciprocal MUL/SHR/ADD), power-of-two shift (SHR), 32-bit index/stride splits, and DOUTA of in0/in1 | yes: all vertex programs |
| 0x1ef40 | vertex fetch where the stream bases come from DS0 0x34+s (not in the data segment; preset?) via ADD, then a plain DOUTD | no |
| 0x1fb90 | patches stream base addresses into a 0x1f3d0 program | no |
| 0x1e970 | **pixel program**: DOUTU, then per iterated input a DOUTI and, for samples, a DOUTT (merging a sample into the preceding iteration when it can) | yes |
| 0x1ed20 / 0x1ee40 | **secondary / state**: optional DOUTD (uniforms, state), then DOUTU | yes: 0x2e0061a0, 0x30401120 |
| 0x1fc30 | n × DOUTD (2-D form) + DOUTU | no |
| 0x1fd60 | VDM type 6: DOUTA (0x2000, 0), 3 × DOUTA (x, 0x100), DOUTU | yes: 0x30400000 |
| 0x1e4b0, 0x1e710 | loopback and auxiliary programs: DMA pairs, DOUTA of in0, DOUTI with the constant `0x20011000`, DOUTU | no |
| 0x1e3a0 / 0x1fe00 | copy the static **pixel-event** program (0xc4 bytes from .rodata 0x23280 / 0x23360) and patch its data words | yes: 0x2e006000 |
| static (VDM state) | `tst ds0[0]; bra; doutd; tst ds0[2]; bra; doutd; doutu`: two optional state DMAs and a task. 0x3040e000 holds hundreds of these 0x80-byte slots | yes |

The pixel-event program (0x2e006000) decoded:

- **in1 bit 3 (8) set:** run the task at ds0[0] (0x37008200).
- **else bit 1 (2) set:**
  - if bit 8 (0x100) of `in1 xor ds1[0]` is set: DOUTA (in2, ds1[0]), then
    build the EOT task word from in0/in1 (an OR of shifted, masked fields)
    and start the EOT program 0x37004000 with it as DOUTU word 1;
  - otherwise run the task at ds0[2] (0x37008280).
- **else bit 2 (4) set:** run the task at ds0[2].
- **else:** nothing.

The event-bit names (end of tile, end of render, …) are guesses.

## Captures

The new captures were made with `probe-pds`, which is `re/probe/probe.c`
with these new knobs:

- `PROBE_ATTRS`: extra float attributes a1… with n components each;
- `PROBE_INTERLEAVE`;
- `PROBE_TEX`: n 8×8 RGBA checker textures, samplers t0….

The `cap` pattern ran on the EA3, 2026-10-27. All runs rc=0.

| capture | what | PDS shows |
|---|---|---|
| pbase | probe defaults | same programs as base.bin |
| tex1 | `texture2D(t0, v)`, v = a1 (vec2) | DOUTI sample tc0 + DOUTT (0xf0001000, 8×8); DOUTU w1 bit 25 |
| tex2 | two textures, v, w = a1.yx, c = a2 | iteration merged with sample tc1, then sample tc2; two DOUTTs |
| dep1 | `texture2D(t0, v*v+0.1)`: a dependent read | plain DOUTI tc0 ×2, no DOUTT (the USSE samples) |
| attr3 | 4 streams: vec2, vec4, vec3, float | 4 MUL16/DOUTD pairs, strides 8/16/12/4, attr regs 0/12/4/8 |
| attri | same, interleaved client array | identical PDS: GL de-interleaves client arrays into separate VB copies |
| vary2 | vec4 + vec2 varyings | two DOUTIs, tc0 ×4 then tc1 ×2 (last) |

## Open questions

- What bit 26 (MOVS `03`) means: setting it on a vertex DOUTU hangs the TA.
  Which of bits 27 and 29 makes a DOUT consume word 2.
- **Experiments on hardware:** `re/decode/mkreplay.py --poke va:value`
  rewrites a word before every kick. Change only things a wedge can't make
  worse, and run one replay at a time, checking dmesg for `Hardware Recovery`
  between runs. A failed recovery needs a reboot.
- DOUTD ctrl bits 22–25. The DOUTU w0 top byte and w1 bit 2.
- The remaining DOUTI bits 5–8, 17–20 and 25–26, and what precision values
  0/1/2 are.
- The meaning of DOUTA (dest 6), and why it is issued in threes.
- The pixel-event inputs and event bits. The data size of a raw-pointer
  program is inferred, not read.
- The VDM info word bits 8–21 for state programs (`0e022403`, `0800e103`).
- Opcodes and sub-ops not seen: 1, 3, 5, 7, b, e; ADD/MUL with s = 1; d with
  s = 0.
