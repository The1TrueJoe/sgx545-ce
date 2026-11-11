# SGX545 PDS programs

The PDS (programmable data sequencer) runs a small program before each USSE
task. It fetches vertex data by DMA, sets up the iterators that interpolate
varyings, hands texture state to the texture unit for non-dependent reads,
and then starts the USSE program. This file is what we know of its programs,
their data segments and their instruction set on the Cedarview DDK 1.7 SGX545.

`dis.py` disassembles every PDS program in a capture:

    python3 tools/pds/dis.py CAPTURE.bin [--kick N] [--raw]
    python3 tools/pds/dis.py --words 87000000 90000006 07000203 af000000

Each fact carries one of these tags:

- **hardware**: run on the GPU. Either a replay of a captured frame with one word
  changed (`tools/decode/mkreplay.py --poke`), or the open stack: the winsys
  (`mesa/src/gallium/winsys/sgx/drm/sgx_frame.c`) and `srvinit` build every program
  kind marked so from this page, and they run (dEQP-GLES2 on the EA3, desktop sessions
  on the EA1, cold boot of the microkernel).
- **capture**: seen in captures of the DDK's programs and consistent across all of
  them: 13 targeted captures (listed at the end), and the `caps`, `cube` and `ff` sets
  (315 programs found through their pointers).
- **unconfirmed**: stated but not yet backed by the above; listed in "Gaps" with the
  test that would confirm it.
- **guess**: a reading that fits what we have. It could be wrong.

## Evidence

- **Captures** of the DDK's programs in GPU memory: the six original ones
  (`base red clr mov two ref`), seven made with `probe-pds` (see "Captures"), and the
  `caps`, `cube` and `ff` sets.
- **Replays** of captured frames with words poked, on the EA3.
- **The open stack's own programs:**
  - pixel, secondary, vertex-fetch, VDM state, TA terminate, pixel-event and
    end-of-tile programs (the winsys);
  - the five microkernel-entry programs (`srvinit`, docs/SRVINIT.md).

  They are encoded through `mesa/src/sgx/pds` (isaspec, written from this page).

## Program layout

Hardware (the open stack lays out every program this way) and capture. A program is a **data segment** followed by **code**:

    addr + 0x00: data row 0:  DS0[0]  DS0[1]  DS1[0]  DS1[1]
    addr + 0x10: data row 1:  DS0[2]  DS0[3]  DS1[2]  DS1[3]
    ...
    addr + 16*rows: code, 32-bit instructions, ending in HALT (af000000)

- **Two data stores, DS0 and DS1.** Each is a bank of 32-bit words, and their
  words are interleaved two at a time in memory. DS0 word *n* sits at memory
  word `(n>>1)*4 + (n&1)`; DS1 word *n* sits at `(n>>1)*4 + 2 + (n&1)`.
  - The DDK fills the two banks alternately, so a run of words fills memory in
    order (capture).
- **The data segment is rounded up to 16 bytes,** and the code follows it
  directly: `code = data + (ndwords*4 + 15 & ~15)`.
- **Temporaries.** Words 0x30 and up of each bank are temporaries: they are
  never in the data segment, and the programs write them. The disassembler
  prints them as `ds0[0x30]` and so on.
  - DS0 0x30 and 0x31: the DDK's scratch registers (capture).
  - DS1 0x30 and 0x32: the vertex index × stride product and the pixel-event
    scratch (capture; the open stack uses them the same way: hardware).
- **Inputs.** DS0 words 0x40 and up are inputs, printed as `inN` (capture).
  - Vertex programs: `in0` is the vertex index (hardware: the open vertex fetch
    multiplies it by the stride). `in1` is the instance index (**unconfirmed**:
    no captured or open draw is instanced).
  - Pixel-event program: `in0` holds the tile position (it is shifted and
    masked into the EOT task word), `in1` holds the event flags (it is tested
    against 8, 2, 4 and 0x100), and `in2`/`in3` are written out with DOUTA.
    The open pixel-event program uses them this way and runs (hardware); what
    each bit means is a guess.

## How programs are referenced

| referrer | format | status |
|---|---|---|
| VDM stream word, type 7 or f (vertex fetch) | `type<<28 \| addr>>4`, then an info word: bits 0–6 = data rows (16 B); bits 7–21 = ceil(input attribute dwords / 4); bits 22–23 = min(n, 3), a batching value; bits 24–31 = ceil(output dwords / 4) | hardware (the open stack's draws); capture (which field is input and which output: tex1, attr3 and the clear). The batching value's formula is **unconfirmed** |
| VDM stream word, type 4 or 6 (state program) | `type<<28 \| addr>>4`, then an info word: bits 26–31 = data rows; low bits `0xe100 \| (n+3)>>2` or `0x2022401` and similar | rows: hardware (open stack), capture; the low bits: capture, meaning guess |
| TA/3D state word (per-object pixel program, its secondary program, the background object) | `data_dwords<<24 \| (addr − 0x2e000000)>>4`, so the PDS pixel heap base is the origin | hardware (open stack); capture (7 programs in 13 captures, data size always right) |
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

Every instruction has these fields (capture, all classes below; hardware for the classes the open stack emits):

| bits | field |
|---|---|
| 28–31 | opcode |
| 27 | sub-op (`s`): selects between two operations of an opcode |
| 24–26 | condition: 7 = always, 0 = only if p0 (hardware: the open pixel-event and microkernel-entry programs branch on p0; capture). MOVS also uses 3; see below |

Register operands come in three numberings:

| name | width / units | space | status |
|---|---|---|---|
| **U** (unified) | 8-bit, 32-bit units | 0x00–0x3f DS0, 0x40–0x7f DS1, 0x80+ inputs. Used by ALU destinations and shift sources | hardware (open stack); capture |
| **S1** | 8-bit, 16-bit units | DS0 side: word = f>>1, half = f&1; DS0 words 0x40+ are the inputs, so vertex index lo/hi = 0x80/0x81 and instance index lo/hi = 0x82/0x83 | hardware (the open vertex fetch's MUL16 of in0.lo); capture |
| **S2** | 7-bit, 16-bit units | DS1 side: word = f>>1, half = f&1 | hardware (open stack); capture |

So `ds1[0x30]` is U 0x70 and S2 0x60, and `ds0[0x30]` is U 0x30 and S1 0x60.

### MOVS: the DOUT instruction (opcode 0 or 2)

    31..28 op  27 s  26..24 cc  23..18 src1 row  17..13 src2 row  12..5 swizzle  4..0 dest

- **src1 row** (bits 18–23) is a DS0 64-bit pair: words 2r and 2r+1. Rows
  0x18+ are the temporaries; rows 0x20+ are the inputs (row 0x20 = in0:in1).
  Hardware (open stack); capture.
- **src2 row** (bits 13–17) is a DS1 pair. Row 0x18 is DS1 0x30:0x31, and
  row 0x19 is DS1 0x32:0x33. Hardware (the open vertex fetch reads row 0x19);
  capture.
- **swizzle** has four 2-bit selects, one per output word: out0 is bits
  11–12, out1 bits 9–10, out2 bits 7–8, out3 bits 5–6. The select values are
  0 = src1.lo, 1 = src1.hi, 2 = src2.lo, 3 = src2.hi. Hardware: the open
  programs use all four selects (the pixel program's DOUTT reads `2, 3, 0, 1`);
  capture.
- **dest** (bits 0–4) is the DOUT target; only 2–6 have been seen. Hardware
  for 2–6 (the open stack uses each). Other values print as `dout<n>`.

| dest | name | words consumed | meaning | status |
|---|---|---|---|---|
| 2 | DOUTI | 2 | iterator setup: interpolate a varying into USSE PA registers and/or issue a texture coordinate for a sample | hardware |
| 3 | DOUTD | 2, or 3 with the `x` bits | DMA from memory into USSE attribute registers | hardware |
| 4 | DOUTT | 4 | texture state for the sample started by the preceding DOUTI | hardware |
| 5 | DOUTU | 3 | start the USSE task | hardware |
| 6 | DOUTA | 2 (pairs) | write word 0 into the USSE primary attribute register named by ctrl bits 8+ | hardware (the microkernel-entry and TA terminate programs; see DOUTA below) |

The variant bits:

- **Top nibble 2 with bit 27 set (`0x2f…`), printed `.x2.s27`: the DOUT
  also consumes word 2, which DOUTD adds to its address.** Hardware (the open
  vertex fetch uses it), and:
  - A replay of ref.bin with the vertex program's `2f032343` changed to
    `07032343` (`mkreplay.py --poke 0x3040d844:0x07032343`) renders no
    triangle at all: every vertex fetched vertex 0. The unchanged replay is
    pixel-exact.
  - **Neither bit does it alone; they act as one field.** Rerun after a power
    cycle (2026-10-28; the control was pixel-exact before and after):
    - `27032343` (bit 29 only) and `0f032343` (bit 27 only) both render the
      same wrong triangle: 75765 pixels differ, with one corner at clip
      (0,0) and the other two off-screen to the left and below. Neither
      locked up.
    - Even vertex 0 is not at its captured position, so either single-bit
      form changes the fetch address itself, not only the per-vertex
      offset. What those two forms do is not decoded.
    - Both bits (`2f`): correct. Neither (`07`): every vertex is vertex 0.
  - Also seen on DOUTA pairs (`2f800466`), meaning unknown.
- **Top byte `03`, condition field 3, printed `.cc3`.** It is used only on the
  closing DOUTU of vertex-fetch programs, and every other DOUTU uses `07`.
  - **Do not set bit 26 there.** A replay with the triangle's `030403e5`
    changed to `070403e5` locked up the TA. SGX hardware recovery then failed
    in a loop (`SGXInitialise failed (24)`) until a reboot (2026-10-28).
  - So the bit matters for vertex tasks. What it means is still a guess:
    something that a vertex task must not wait on or signal.

### Arithmetic and logic (opcodes 4, 6, c, d)

    31..28 op  27 s  26..24 cc  23..16 src1 (S1)  15..9 src2 (S2)  8..7 (0)  6..0 dst (U)

| op | s | name | evidence |
|---|---|---|---|
| 4 | 0 | ADD (32-bit) | capture: address + index×stride; the reciprocal-divisor sequence |
| 6 | 0 | MUL16: src1.half × src2.half, 16×16→32 | hardware (the open vertex fetch: index × stride); capture. A 32-bit product is split into 16-bit halves and recombined with SHL 16 + ADD when the vertex count > 0x10000 or the stride > 0xffff (**unconfirmed**) |
| c | 1 | AND | hardware (the open pixel-event program); capture: masks with 8, 2, 4, 0x100, 0xff8, 0x3ff000 |
| c | 0 | OR | hardware (the open pixel-event program assembles the EOT word with it); capture |
| d | 0 | XOR | hardware (the microkernel event entry `pds3`: after `t = x & m`, `x op t` then a test for zero, i.e. "no bits other than m"); capture. That XOR rather than ANDN computes it is a guess |
| d | 1 | `op0xd.s1` | only `df820070` = in1 op ds1[0] (= 0), then AND 0x100; XOR-like there, NOR/NOT fit too. The open pixel-event program uses it the same way (hardware), which doesn't tell them apart |
| 4, 6 | 1 | `op0x4.s1`, `op0x6.s1` | not seen |

- **dst is 7 bits (0–6).** Capture: bits 7–8 are always 0,
  and the inputs (U 0x80+) are never written. The highest seen is 0x72.
- **The shift's dst is 8 bits (0–7)** by its encoding, but only values below
  0x80 have been seen. Its src does read inputs (U 0x80+).

### TST (opcode 8)

    31..28 8  26..24 cc  23..16 src1 (S1)  15..9 src2 (S2)  8 sel  7..0 (0)

- **Sets p0 = (src == 0),** where src is src2 if `sel` is set, else src1.
  Hardware: the open pixel-event and VDM state programs branch on it this
  way; capture.
- The source field `sel` doesn't pick is 0 in every TST seen (capture).
- Examples: `87000000` tests ds0[0], `87040000` tests ds0[2], and `8700c100`
  tests ds1[0x30].

### BRA (opcode 9)

    31..28 9  26..24 cc (0 = if p0, 7 = always)  23..0 target instruction index (from the first code word)

Hardware: the open pixel-event, VDM state and microkernel-entry programs
branch this way. The entry program `pds1` is `tst in0 == 0; bra p0, 3; bra 4;
3: doutd; 4: doutu`, so `97000004` (condition 7) is unconditional (hardware,
srvinit).

### Shifts (opcode f)

    31..28 f  27 s (0 SHL, 1 SHR)  26..24 cc  23..16 src (U)  15..8 amount  7..0 dst (U)

SHL: hardware (the open pixel-event program). SHR: capture
(`ff800030 | n<<8` is the instance divisor's power-of-two shift; `ff300f30` is
>> 15 in the reciprocal divide). `f7311031` is T1 <<= 16 in the 32-bit multiply
(**unconfirmed**).

### HALT

`af000000`: opcode a, s 1, cc 7. Hardware and capture: every program ends with it. A
program may hold several HALTs, as branch targets. Opcode a with s = 0, or
with another condition, has not been seen.

### Text notation

`tools/pds/dis.py` prints this, and an assembler takes it.

- **Numbers:** below 10 in decimal, otherwise lowercase hex with `0x`. This
  holds for every number: data-store words (`ds1[0x30]`), inputs (`in9`,
  `in0xa`), the `[r1=]`/`[r2=]` row fields (`[r2=5]`), branch targets and
  shift amounts. Immediates (branch targets, shift amounts) start with `#`.
- **Registers:** `ds0[n]` and `ds1[n]` for the data-store words, and `in<n>`
  for DS0 word 0x40 + n.
  - A U operand is `ds0[u]` below 0x40, `ds1[u − 0x40]` below 0x80, else
    `in<u − 0x80>`.
  - An S1 or S2 operand names word `f >> 1` of its bank (S1 the DS0 side, S2
    the DS1 side), with `.hi` added when `f` is odd.
- **Condition:** cc 7 prints nothing, cc 0 is a `(p0) ` prefix, and any
  other cc is a `.cc<n>` suffix on the mnemonic.
- **MOVS:** `<dout>[.x2][.s27][.cc<n>] w0, w1, w2, w3`.
  - `<dout>` is `douti`, `doutd`, `doutt`, `doutu` or `douta` for dest 2–6,
    else `dout<n>`.
  - `.x2` is opcode 2 (bit 29), and `.s27` is bit 27.
  - `wN` is the word swizzle select N reads.
  - A row field (bits 18–23 or 13–17) that no select reads is written after
    the operands as `[r1=<n>]` or `[r2=<n>]`, only when it is nonzero.
- **ALU:** `add|mul16|or|and|xor dst, src1, src2`, with dst a U operand,
  src1 an S1 operand and src2 an S2 operand. An opcode and sub-op that the
  table doesn't name print as `op0x<op>.s<s>`, e.g. `op0xd.s1`.
- **TST:** `tst <src>`. An S2 (DS1) source means `sel` = 1, and an S1
  source (DS0 or an input) means `sel` = 0.
- **The rest:** `bra #n`, `shl|shr dst, src, #n` (dst and src U), and `halt`.

| word | text |
|---|---|
| `2f032343` | `doutd.x2.s27 ds0[0], ds0[1], ds1[0x32], ds1[0x32]` |
| `030403e5` | `doutu.cc3 ds0[2], ds0[3], ds1[1], ds1[1]` |
| `07840406` | `douta in2, ds1[0], in2, in2` |
| `cf820870` | `and ds1[0x30], in1, ds1[2]` |
| `df820070` | `op0xd.s1 ds1[0x30], in1, ds1[0]` |
| `8700c100` | `tst ds1[0x30]` |
| `90000018` | `(p0) bra #0x18` |
| `f7811470` | `shl ds1[0x30], in1, #0x14` |
| `af000000` | `halt` |

### Bits no field names

These are 0 in every program of the 25 captures we have (the `caps`, `cube`
and `ff` sets: 315 programs found through their pointers, 40 distinct code
words: capture). A decoder may treat them as reserved and
refuse words that set them, and an encoder writes 0.

| where | bits |
|---|---|
| TST | 27; 0–7; the source field `sel` doesn't pick (src2 when `sel` = 0, src1 when 1) |
| BRA | 27 |
| ALU (opcodes 4, 6, c, d) | 7–8 |
| DOUTU word 0 | 22–23 |
| DOUTU word 1 | all but 1, 2 and 22–25. Bit 1 comes only from the microkernel templates (the value 2 there) |
| DOUTI word 0 | 5–8, 17–20, 31 |
| DOUTI word 1 | 7–31 (0–6 is the half a merge copies; bit 4 is the sprite's point coordinate) |
| DOUTD word 1 (ctrl) | 26–31 |
| DOUTA word 1 (ctrl) | 0–7, and 16–31 (see DOUTA) |

**The S1/S2 half bit in 32-bit ops** (ADD, OR, AND, XOR): every captured use
and every open-stack program passes an even number, so what an odd one does is
unknown. The notation shows it as `.hi`, and nothing should emit it.

## DOUT word layouts

### DOUTU: start a USSE task (3 words)

**Word 0:**

- bits 0–3: USE code base register (`EUR_CR_USE_CODE_BASE_n`). Hardware.
- bits 4–21: code offset / 8 inside that base's 512 KB window. Hardware.
  - `off = pc - heap_base`, then `w0 = (off>>19) + base_index |
    (off*2 & mask)` (the open stack's formula).
  - Pixel programs use bases 0–9 at 0x37000000 + n·512K; vertex and state
    programs use bases 10+ at 0x3c000000 + (n−10)·512K.
  - Capture: every DOUTU in 13 captures lands on a captured USSE buffer.
- bits 22–23: 0 everywhere (capture).
- bits 24–31: **one 8-bit field, the temporary register count in fours,
  `(n+3)>>2`.** Hardware: the probe's main pixel program has 0 here, and with 4
  poked in (isa/hwt.py) a test program uses r0-r15 correctly on a CE5300 board;
  the open stack writes every program's count this way. Not shown: that 0
  breaks them. Captured values are 0 and 1.
- The microkernel's own task entries (with code base 15, the kernel code
  heap; `pds0`/`pds2`/`pds3`/`pds4`) have bit 27 (0x8000000) set,
  but `pds1`'s word does not. In this field that is the value 8, 32
  temporaries; that reading is a guess.

**Word 1:**

- bit 24 = the pixel program has plain iterations; bit 25 = it has texture
  samples. Hardware (the open stack sets them); capture: tex1
  0x02000004, vary2 0x01000004, tex2 0x03000004, and the untextured
  triangle 0x00000004.
- bit 2 is always set on pixel tasks. Guess: pixel pipe, or a sample-rate
  bit.
- Vertex and state tasks have 0. Microkernel tasks have 2 (the DOUTU swizzle
  of the entry programs sends it as word 1: hardware, srvinit).
- The pixel-event program computes word 1 at run time: an OR of the tile
  position fields, 0x00800000 and 0x00400000.

**Word 2:** 0 in every program the DDK builds (capture). The static
pixel-event program repeats its word 0 there (`doutu ds0[0], ds0[1],
ds0[0], ds0[0]`: 0x00010400, and 0x01010500 for the ds0[2] task), so the
word is probably not read. Unknown.

### DOUTD: DMA into USSE attributes (2 words, +1 with `x`)

- **Word 0:** the source address. Hardware.
- **Word 1:**
  - bits 0–7: dwords − 1, `((bytes+3)>>2)-1`: hardware (open stack); capture
    (2/3/4/1 for the vec2/vec3/vec4/float streams).
  - bits 8–21: destination attribute register. Hardware (open stack); capture:
    attr3 puts the streams at registers 0, 12, 4 and 8, the compiler's choice,
    and the shader reads them there.
  - bits 22–24: 1/2/3 on the state and uniform DMAs, 0 on vertex fetch.
    Unknown.
  - bit 25: set on state and uniform DMAs (secondary attributes?). Guess.
  - A 2-D form `(n-1) | (m-1)<<4 | reg<<8 | (n-1)<<22` (**unconfirmed**; not
    seen in captures).
- **Word 2 (with `x`):** the byte offset added to word 0 (index × stride).
  Hardware (open vertex fetch).

### DOUTI: iterator setup (2 words)

Each DOUTI has two halves. The DDK merges a texture entry into a preceding plain
entry by copying `w0 & 0x400007ff` and `w1 & 0x7f` (capture: tex2). The open
stack builds DOUTIs from the compiler's iteration list (`struct sgx_pds_douti`),
and every field below that it sets is hardware.

**Sample half** (bits 0–10, bit 30; w1 bits 0–6):

| bits | meaning | status |
|---|---|---|
| 0–4 | texture coordinate set + 1 to sample with (0 = no sample) | hardware; capture |
| 9 / 10 | 2-D (0x200) / projected (0x400) sample | hardware (the open stack sets 0x400 for a projected read and 0x200 otherwise); captures only show 2-D |
| 30 | last sample of the program | hardware; capture |
| 13, 16 (0x12000) | set on pure-sample entries (the iterate half is unused): source 0x24 | hardware, capture; meaning unknown |

**Iterate half** (bits 11–29):

| bits | meaning | status |
|---|---|---|
| 11–16 | source: texture coordinate set + 1 (TC slot from the GL varying map); 0 = the fragment's position (gl_FragCoord: `27800000` x3, `27c00000` x4: x and y at pixel centers in the target's memory order, z, then 1/w; capture `fragcoord`, `fragcoordw`, 2026-11-02); 0x20 = color (`0x1c10000`); 0x22 = the depth plane a discard's `pcoeff` reads, x3 (ISA.md "Phases, visibility feedback and discard") | capture |
| 21 | color iterator at precision < 2 | capture; meaning of the precision values guessed |
| 22–23 | components − 1 (forced 4 for color) | hardware; capture |
| 24 | plain iteration enabled | hardware; capture |
| 25–26 | a 2-bit field, seen only as 3 or 0. It is 3 on every entry the DDK's GL driver makes for a shader: plain, merged and pure-sample (`27800000`, `37400800`, `46012201`). It is 0 on the two entries built elsewhere, the clear's color `21c10000` and the background object's sample `40012001`. Both of those are drawn at w = 1, so a guess is perspective-correct interpolation | 3/0 split: capture; the open stack writes 3 (hardware); meaning guess |
| 27 / 28 | precision 2 / precision 1; mediump varyings give bit 28 | hardware: the open compiler's F16 iterations set bit 28 and read F16 values; capture |
| 29 | last plain iteration | hardware; capture |

- Iterations fill the USSE PA registers in program order. The DOUTI does not
  name a destination register (hardware: the open compiler relies on it).
- w1 bit 4 (0x10): a sprite's gl_PointCoord (capture `sprite`, 2026-11-02:
  `37400800`, iterate tc0 x2 f16, w1 0x10). Bit 5 (0x20) is not captured.
- Examples:
  - `37c00800`: tc0 ×4, f16, last.
  - `17c00800 37401000`: tc0 ×4, then tc1 ×2 (last).
  - `37c00a02 46012203`: tc0 ×4 merged with a sample of tc1, then a sample
    of tc2 (last sample).
  - `21c10000`: the clear's color ×4.
  - `40012001`: background object, sample tc0 with type bits 0.

### DOUTT: texture state (4 words)

- These go to the texture unit for the sample of the preceding DOUTI.
- Word 2 is the texture base address. Hardware (open stack); capture: 0xf0001000 and 0xf0001100
  in the texture heap; 0x3e265000, the back buffer, for the background
  object.
- Word 1: bits 0–11 = height − 1, bits 12–23 = width − 1. Hardware; capture:
  `cc2cf1df` holds 479 and 719 for the 720×480 back buffer, and `0d007007`
  holds 7 and 7 for an 8×8 texture.
- The rest are TPU control words (format, filter, stride): docs/STATE.md §5.

### DOUTA: write a primary attribute register (2 words)

- **(value, ctrl): ctrl bits 8–15 select the register.** Hardware (registers
  0-2: the entry and TA terminate programs). The width is not known: only registers 0, 1 and 2 have been seen
  (ctrl 0, 0x100, 0x200). Bits 0–7 and 16–31 are always 0, so take 8–15 (a
  primary attribute number) and treat the rest as reserved.
  - `pds3` (the microkernel's event entry) starts with
    `douta (in0, 0)`, `douta (in2, 0x100)` and `douta (in4, 0x200)`. That
    hands its inputs to the microkernel task in PA0–PA2; `pds4` does the
    same with in0 and in4.
  - The pixel-event program sends `(in2, 0)` before the EOT task.
- **Repeated DOUTAs to the same register** (the VDM type-6 program, and the
  transfer-queue programs' rows `(0, 0x100)`, `(0, 0x200)`, … three times
  each) are unexplained. Guess: they feed per-vertex slots, not one
  register.

## What the DDK generates

| program | seen in captures | open stack |
|---|---|---|
| **vertex fetch**: per stream `MUL16 ds1[0x32], in0/in1 . lo, stride`, then per element `MOVS.x DOUTD (addr, ctrl, ds1[0x32])`, then `MOVS.cc3 DOUTU`, HALT. Optional instance divisor (reciprocal MUL/SHR/ADD), power-of-two shift (SHR), 32-bit index/stride splits, and DOUTA of in0/in1 | yes: all vertex programs | builds the per-vertex form (no instancing) |
| **pixel program**: DOUTU, then per iterated input a DOUTI and, for samples, a DOUTT (merging a sample into the preceding iteration when it can) | yes | builds it |
| **secondary / state**: optional DOUTD (uniforms, state), then DOUTU | yes: 0x2e0061a0, 0x30401120 | builds it |
| VDM type 6: DOUTA (0x2000, 0), 3 × DOUTA (x, 0x100), DOUTU | yes: 0x30400000 | builds it (TA terminate) |
| the **pixel-event** program (0xc4 bytes) with its data words patched | yes: 0x2e006000 | builds its own (below) |
| **microkernel entry programs** (docs/SRVINIT.md): `pds0`/`pds2` one DOUTU; `pds1` optional DOUTD (TA3D control block) + DOUTU; `pds3` (event) and `pds4`: DOUTA of the inputs, then mask tests on in0/in1 picking one of several microkernel DOUTUs | in SrvInit traces | srvinit builds them |
| VDM state: `tst ds0[0]; bra; doutd; tst ds0[2]; bra; doutd; doutu`: two optional state DMAs and a task. 0x3040e000 holds hundreds of these 0x80-byte slots | yes | builds it |
| vertex fetch with stream bases added from DS0 0x34+s; a 2-D DOUTD form; loopback and auxiliary programs (DMA pairs, DOUTA of in0, DOUTI with `0x20011000`) | no (**unconfirmed**) | no |

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

The new captures were made with `probe-pds`, which is `tools/probe/probe.c`
with these new knobs:

- `PROBE_ATTRS`: extra float attributes a1… with n components each;
- `PROBE_INTERLEAVE`;
- `PROBE_TEX`: n 8×8 RGBA checker textures, samplers t0….

The `cap` pattern ran on a CE5300 board, 2026-10-27. All runs rc=0.

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
  What the single-bit forms `27…`/`0f…` do with word 2 (both bits together
  add it to the address).
- **Experiments on hardware:** `tools/decode/mkreplay.py --poke va:value`
  rewrites a word before every kick. Change only things a wedge can't make
  worse, and run one replay at a time, checking dmesg for `Hardware Recovery`
  between runs. A failed recovery needs a reboot.
- DOUTD ctrl bits 22–25. DOUTU w1 bit 2.
- What DOUTI bits 25–26 mean (see DOUTI), and what precision values 0/1/2
  are. Bits 5–8 and 17–20 have never been seen set.
- The width of DOUTA's register field.
- What an odd S1/S2 number does in a 32-bit ALU op.
- Why DOUTA is sometimes issued three times to the same register.
- The pixel-event inputs and event bits. The data size of a raw-pointer
  program is inferred, not read.
- The VDM info word bits 8–21 for state programs (`0e022403`, `0800e103`).
- Opcodes and sub-ops not seen: 1, 3, 5, 7, b, e; ADD/MUL with s = 1; a
  with s = 0. What d with s = 1 does.

## Gaps

- Instance index in `in1`, and the instance divisor's reciprocal sequence (ADD,
  SHR) → an instanced draw on the open stack, checked against the DDK's capture of
  the same draw.
- MUL16's split into halves for vertex counts > 0x10000 or strides > 0xffff, and
  `f7311031` (SHL 16) in it → an open-stack draw with a stride > 0xffff.
- The VDM info word's batching value (bits 22–23) → open-stack draws with each value
  0-3, watching for TA errors.
- The 2-D DOUTD form and the programs not seen in captures (stream bases from DS0,
  loopback, auxiliary) → a capture of a draw that uses them, if GL ever does.
