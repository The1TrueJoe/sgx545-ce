"""Parse a USP binary (the DDK compiler's pre-patch shader container, DDK 1.7).

Layout read from libusc's writer (tools/intern/usc2/uspbin.c, at 0xf28a0 in
libusc.so.1.7.862890; the block walker callbacks are 0xf42a0, 0xf26e0, 0xf2380,
0xf23d0). Every field is written packed, with no padding, through a stream of
u32 / u16 / raw-bytes callbacks, so u16 fields leave later u32s unaligned.

  header (0x84 bytes)
    +0x00 u32 magic 0x55535020 ("USP ")
    +0x04 u32 version 0x23
    +0x08 u32 size of the whole binary minus 12
    +0x0c u32 1 (block type: PC shader header)
    +0x10 u32 program type: 0 fragment, 1 vertex, 2 (geometry?)
    +0x14 .. 0x83  flags, register counts and list counts (see HDR below)
  list A   n_a entries x 12 bytes (6 u16), one per iterated input:
           (kind, sampler, iterator, components, precision, registers);
           kind 4 a varying, 0 a non-dependent texture read, 1 a projected
           one; precision 1 highp, 2 mediump, 3 lowp
  list B   n_b records: the shader's constant/SA loads (u16 kind first)
  list C   n_c records, same record format as list B
           kind 1, 2, 4: 4 u16 (8 bytes); kind 3, 5: 10 bytes
  pre      n_pre instructions, 8 bytes each: the secondary update program
           (uniform-only math the compiler hoists out of the main program;
           every fragment shader's starts with the same 4-instruction prologue)
  [vertex] ceil(n/32) u32 bitmask words (count not in the header, found by fit)
  4 u32
  blocks   u32 type, then a type-specific body, until type 7
    2  code:   u16 count, u16 flags, 4 u16 MOE inc, 4 u16 MOE base,
               count u16 per-instruction flags, count x 8-byte instructions
    3  branch: 8-byte encoded instruction, u16 label
    4  label:  u16 label id
    5  sample (60 bytes of body), 6 (74 bytes), 9 (78 bytes): texture sample
       descriptions the patcher turns into code; their instructions are
       generated at finalize time, not stored here
    7  end

An instruction is 64 bits, stored as (low word, high word).
"""
import glob
import os
import struct
import sys

MAGIC = 0x55535020

# header field offsets that matter for parsing (others are kept raw)
H_NPRE = 0x7C   # u16: instructions in the pre-program
H_NA = 0x4E     # u16: list A entries
H_NC = 0x50     # u16: list C records
H_NB = 0x52     # u32: list B records

REC_SIZE = {1: 8, 2: 8, 3: 10, 4: 8, 5: 10}
BLOCK_BODY = {5: 56, 6: 74, 9: 78}


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
            out.append({"type": "sample%d" % t, "raw": r.raw(BLOCK_BODY[t])})
        else:
            raise ValueError(f"block type {t:#x} at {r.o - 4:#x}")


def load(path):
    d = open(path, "rb").read()
    magic, ver, size = struct.unpack_from("<III", d, 0)
    if magic != MAGIC:
        raise ValueError(f"{path}: not a USP binary")
    end = size + 12
    d = d[:end]
    kind = struct.unpack_from("<I", d, 0x10)[0]
    n_a = struct.unpack_from("<H", d, H_NA)[0]
    n_c = struct.unpack_from("<H", d, H_NC)[0]
    n_b = struct.unpack_from("<I", d, H_NB)[0]
    n_pre = struct.unpack_from("<H", d, H_NPRE)[0]
    r = Reader(d, 0x84)
    list_a = [struct.unpack_from("<6H", d, r.o + 12 * i) for i in range(n_a)]
    r.o += 12 * n_a
    list_b = records(r, n_b)
    list_c = records(r, n_c)
    pre = [r.inst() for _ in range(n_pre)]
    after_pre = r.o
    # vertex programs carry a bitmask whose length isn't in the header: take
    # the first word count that makes the block stream end exactly on type 7
    err = None
    for nmask in ([0] if kind == 0 else range(0, 9)):
        r.o = after_pre
        mask = [r.u32() for _ in range(nmask)]
        tail = [r.u32() for _ in range(4)]
        try:
            blks = blocks(r, end)
        except (ValueError, struct.error) as e:
            err = e
            continue
        if r.o == end:
            break
        err = ValueError(f"block stream ended at {r.o:#x}, size {end:#x}")
    else:
        raise ValueError(f"{path}: {err}")
    return {
        "name": os.path.basename(path).split("-", 1)[-1].rsplit(".", 1)[0],
        "ver": ver,
        "size": end,
        "kind": {0: "frag", 1: "vert"}.get(kind, str(kind)),
        "list_a": list_a,
        "list_b": list_b,
        "list_c": list_c,
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
