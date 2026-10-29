"""Parse a USP binary (the DDK compiler's pre-patch shader container, DDK 1.7).

Layout read from libusc's writer (tools/intern/usc2/uspbin.c, at 0xf28a0 in
libusc.so.1.7.862890; the block walker callbacks are 0xf42a0, 0xf26e0, 0xf2380,
0xf23d0). Every field is written packed, with no padding, through a stream of
u32 / u16 / raw-bytes callbacks, so u16 fields leave later u32s unaligned.

  header (0x84 bytes): see HDR below. The writer fills it from libusc's
  compile state and the driver's psSAOffsets parameters; the driver's
  loader (libGLESv2 FUN_000511d0, block type 1) copies it into a 0xbc-byte
  record that the finalizer and FUN_00054da0 (the finalised program) use.
  Then, in this order (writer and reader agree):
  brn      hdr+0x28 u32: hardware-workaround ids (0x54c1, 0x54d1, 0x5518,
           written when libusc's state +0xbc bits 0/1/2 are set; BRN numbers,
           guess); none in the corpora
  list A   hdr+0x4e entries x 12 bytes (6 u16), fragment only, one per
           iterated input in PA order: (flags, sampler, iterator,
           components, format, registers); flags 4 or 0xc = a plain
           iteration, else a sample; iterator = texture coordinate set + 1,
           0xb-0xf other inputs; format 1 f32, 2 f16, 3 c10; registers =
           PA registers taken
  list B   hdr+0x52 records: constants the driver keeps in memory (u16 kind
           first); never seen
  list C   hdr+0x50 records: constants the driver loads into SA registers,
           register field relative to hdr+0x3c; kind 1, 2, 4: 4 u16 (8 bytes);
           kind 3, 5: 10 bytes. Same record format as list B
  list D   hdr+0x56 entries x 6 bytes (3 u16; reader checks the first <= 16,
           the second <= 4); never written by libusc 1.7
  pre      hdr+0x7c instructions, 8 bytes each: the secondary update program
  [vertex] ceil(hdr+0x80 / 32) u32: the live input (pa) register mask
  outmask  ceil(hdr+0x82 / 32) u32 (hdr+0x82 is always 128): libusc state
           +0x7fc, always ffffffff ffffffff 0 0 in the corpora; libusc
           renumbers vertex outputs by its set bits (guess: the valid output
           registers)
  blocks   u32 type, then a type-specific body, until type 7
    2  code:   u16 count, u16 flags, 4 u16 MOE inc, 4 u16 MOE base,
               count u16 per-instruction flags, count x 8-byte instructions
    3  branch: 8-byte encoded instruction, u16 label
    4  label:  u16 label id (hdr+0x74..0x7a name the labels the driver
               patches at: program start, feedback point, phase-1 start)
    5  sample (56 bytes), 6 sample unpack (74 bytes), 9 texture write
       (82 bytes): parsed by smpblock.py (see ISA.md, "USP texture-sample blocks"). The driver's
       USP finalizer turns them into code; a dependent sample's block holds
       libusc's smp as a template (block["sample"]["smp"]), which is not
       part of "insts"
    7  end

An instruction is 64 bits, stored as (low word, high word).
"""
import glob
import os
import struct
import sys

try:
    import smpblock
except ImportError:  # append, never prepend: this directory's dis.py shadows the stdlib's
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    import smpblock

MAGIC = 0x55535020

# The header from 0x10, field by field: (offset, struct format, name, meaning).
# Sources: libusc's writer (0xf28a0, locals named after where they come from),
# the driver's loader (FUN_000511d0 header case) and its users; see ISA.md,
# "The USP container", for the evidence per field.
HDR = [
    (0x10, "I", "type", "program type: 0 fragment, 1 vertex, 2 geometry"),
    (0x14, "I", "flags_a", "2/4: state +0x9c 0x100000/0x200000 (4: colour may stay in pa0); "
                           "8: feedback split (pre/post-feedback phases, discard); 0x10: state +0xa0 b1"),
    (0x18, "I", "cflags", "libusc compile flags (state +0xac); 0x20 = the driver inserts "
                          "feedback (discard), 0x800 also set for discard, 8 dynamic uniform indexing"),
    (0x1c, "I", "flags_b", "1: state +0x9c 0x4000 (discard shaders); 2: last instruction cannot "
                           "take end; 4: phas sel43; 8, 0x10: state +0x9c 0x800/0x1000; "
                           "0x20: +0x9c 0x10; 0x40: no secondary update program"),
    (0x20, "I", "sa_1a", "psSAOffsets[0x1a], 1..10 (always 9); unknown"),
    (0x24, "I", "sa_1b", "psSAOffsets[0x1b] (always 0x2793); unknown"),
    (0x28, "I", "n_brn", "u32 workaround ids after the header"),
    (0x2c, "I", "flags_c", "1: list B used; 4: memory area at 0x34/0x60; 8: SA constants "
                           "(0x3c, 0x7e); 0x10: indexable temporaries in memory (0x40, 0x4a)"),
    (0x30, "H", "sa_cbase", "psSAOffsets[1] (0)"),
    (0x32, "H", "z32", "0"),
    (0x34, "H", "sa_mem", "psSAOffsets[2] when flags_c 4, else ffff: SA register of that area's address"),
    (0x36, "H", "z36", "ffff"),
    (0x38, "I", "n_mconst", "libusc state +0x110: in-memory constant count (0)"),
    (0x3c, "H", "sa_const", "first SA register of the constants (driver: 9 fragment, 10 vertex)"),
    (0x3e, "H", "sa_count", "SA registers available from sa_const (driver: 0x77, 0x1f6)"),
    (0x40, "H", "sa_scratch", "SA register holding the indexable-temporary memory address, or ffff"),
    (0x42, "H", "sa_0f", "psSAOffsets[0xf] (0); unknown"),
    (0x44, "H", "n_pa", "primary attribute registers the main program uses"),
    (0x46, "H", "n_temps", "temporaries (r) the main program uses"),
    (0x48, "H", "n_temps_pre", "temporaries the secondary update program uses"),
    (0x4a, "I", "scratch_size", "bytes of indexable-temporary memory (flags_c 0x10)"),
    (0x4e, "H", "n_a", "list A entries (iterated inputs, fragment only)"),
    (0x50, "H", "n_c", "list C records (constants in SA registers)"),
    (0x52, "I", "n_b", "list B records (constants in memory)"),
    (0x56, "H", "n_d", "list D entries (0)"),
    (0x58, "I", "smp_mask0", "per-sampler bitmask, libusc state +0x744 (set for vertex texture reads)"),
    (0x5c, "I", "smp_mask1", "second word of it"),
    (0x60, "I", "mem_size", "bytes of the flags_c 4 memory area"),
    (0x64, "H", "neg_off", "-(state +0x7bc * 4); 0"),
    (0x66, "H", "col_type", "fragment: colour result register type (1 r, 2 o, 3 pa)"),
    (0x68, "H", "col_reg", "fragment: colour result register number"),
    (0x6a, "H", "col_fmt", "fragment: colour result format (1 f32, 2 f16, 3 c10, 4 u8)"),
    (0x6c, "H", "col_count", "fragment: colour result registers"),
    (0x6e, "H", "col_alt_r", "fragment: temporary the driver may move the colour to"),
    (0x70, "H", "col_alt_pa", "fragment: pa register the driver may move the colour to"),
    (0x72, "H", "col_alt_o", "fragment: o register the driver may move the colour to"),
    (0x74, "H", "lbl_start", "label at the main program start (placeholders: phas, pcoeff)"),
    (0x76, "H", "lbl_feedback", "label of the feedback point (the discard nop), or ffff"),
    (0x78, "H", "lbl_phase1", "label where the post-feedback phase starts, or ffff"),
    (0x7a, "H", "lbl_split", "fourth label (ffff in the corpora; guess: sample-rate split)"),
    (0x7c, "H", "n_pre", "instructions in the secondary update program"),
    (0x7e, "H", "n_sa_const", "SA registers holding constants (flags_c 8)"),
    (0x80, "H", "n_inmask", "vertex: bits in the input-register mask"),
    (0x82, "H", "n_outmask", "bits in the output mask (always 128)"),
]

REC_SIZE = {1: 8, 2: 8, 3: 10, 4: 8, 5: 10, 0x10: 8, 0x11: 10}  # 0x10/0x11: sgxc -D (our driver only)
BLOCK_BODY = smpblock.BODY


class Reader:
    def __init__(self, d, o=0):
        self.d, self.o = d, o

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.o)[0]
        self.o += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.o)[0]
        self.o += 4
        return v

    def raw(self, n):
        v = self.d[self.o:self.o + n]
        self.o += n
        return v

    def inst(self):
        lo, hi = struct.unpack_from("<II", self.d, self.o)
        self.o += 8
        return lo, hi


def records(r, n):
    out = []
    for _ in range(n):
        kind = struct.unpack_from("<H", r.d, r.o)[0]
        size = REC_SIZE.get(kind)
        if size is None:
            raise ValueError(f"record kind {kind} at {r.o:#x}")
        out.append(struct.unpack_from("<%dH" % (size // 2), r.d, r.o))
        r.o += size
    return out


def blocks(r, end):
    out = []
    while True:
        if r.o + 4 > end:
            raise ValueError(f"ran past the end at {r.o:#x}")
        t = r.u32()
        if t == 7:
            return out
        if t == 2:
            n, flags = r.u16(), r.u16()
            moe_inc = [r.u16() for _ in range(4)]
            moe_base = [r.u16() for _ in range(4)]
            iflags = [r.u16() for _ in range(n)]
            insts = [r.inst() for _ in range(n)]
            out.append({"type": "code", "flags": flags, "moe_inc": moe_inc,
                        "moe_base": moe_base, "iflags": iflags, "insts": insts})
        elif t == 3:
            inst = r.inst()
            out.append({"type": "branch", "insts": [inst], "label": r.u16()})
        elif t == 4:
            out.append({"type": "label", "label": r.u16()})
        elif t in BLOCK_BODY:
            raw = r.raw(BLOCK_BODY[t])
            out.append({"type": "sample%d" % t, "raw": raw, "sample": smpblock.parse(t, raw)})
        else:
            raise ValueError(f"block type {t:#x} at {r.o - 4:#x}")


def header(d):
    return {name: struct.unpack_from("<" + fmt, d, off)[0] for off, fmt, name, _ in HDR}


def load(path):
    d = open(path, "rb").read()
    magic, ver, size = struct.unpack_from("<III", d, 0)
    if magic != MAGIC:
        raise ValueError(f"{path}: not a USP binary")
    end = size + 12
    d = d[:end]
    h = header(d)
    r = Reader(d, 0x84)
    brn = [r.u32() for _ in range(h["n_brn"])]
    list_a = [struct.unpack_from("<6H", d, r.o + 12 * i) for i in range(h["n_a"])]
    r.o += 12 * h["n_a"]
    list_b = records(r, h["n_b"])
    list_c = records(r, h["n_c"])
    list_d = [struct.unpack_from("<3H", d, r.o + 6 * i) for i in range(h["n_d"])]
    r.o += 6 * h["n_d"]
    pre = [r.inst() for _ in range(h["n_pre"])]
    mask = [r.u32() for _ in range((h["n_inmask"] + 31) // 32)] if h["type"] == 1 else []
    tail = [r.u32() for _ in range((h["n_outmask"] + 31) // 32)]
    blks = blocks(r, end)
    if r.o != end:
        raise ValueError(f"{path}: block stream ended at {r.o:#x}, size {end:#x}")
    return {
        "name": os.path.basename(path).split("-", 1)[-1].rsplit(".", 1)[0],
        "ver": ver,
        "size": end,
        "kind": {0: "frag", 1: "vert"}.get(h["type"], str(h["type"])),
        "hdr": h,
        "brn": brn,
        "list_a": list_a,
        "list_b": list_b,
        "list_c": list_c,
        "list_d": list_d,
        "pre": pre,
        "mask": mask,
        "tail": tail,
        "blocks": blks,
        # every instruction: the secondary program, then the main program's blocks
        "insts": pre + [i for b in blks for i in b.get("insts", [])],
        "raw": d,
    }


def corpus(dirname):
    return [load(p) for p in sorted(glob.glob(os.path.join(dirname, "*.usp")))]


if __name__ == "__main__":
    bad = 0
    for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.usp"))):
        try:
            s = load(p)
        except ValueError as e:
            print("FAIL", e)
            bad += 1
            continue
        kinds = " ".join(b["type"] + (f"[{len(b['insts'])}]" if "insts" in b else "") for b in s["blocks"])
        print(f"{s['name']:14} {s['kind']} a{len(s['list_a'])} b{len(s['list_b'])} c{len(s['list_c'])} "
              f"pre{len(s['pre'])} mask{len(s['mask'])} | {kinds}")
    print(f"{bad} failed")
