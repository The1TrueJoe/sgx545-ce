"""USP texture-sample blocks (block types 5, 6, 9): parse and print them.

See notes_sample.md for where every field comes from. In short:

  writer   libusc FUN_001042a0 (uspbin.c, the block walker; the sample cases
           are inlined: IR opcodes 0xa0-0xa4 -> block 5, 0xa5 -> block 6,
           0xa6 -> block 9), per-instruction flags FUN_00103fd0
  reader   libGLESv2 FUN_000511d0 (Ghidra addresses, file vaddr + 0x10000):
           block 5 at 0x51c79, block 6 at 0x51a18, block 9 at 0x513c7

All fields are packed little-endian with no padding.

  5  SAMPLE (56 bytes): one texture read. Dependent reads carry the smp
     instruction libusc encoded (a template: the driver fills in the
     destination and texture state, and repeats it per texture chunk);
     non-dependent reads carry the iterated coordinate instead.
  6  SAMPLE UNPACK (74 bytes): converts the raw texel data of the sample with
     the same id into the format and registers the shader wants.
  7  (end)
  9  TEXTURE WRITE (82 bytes): an image store (IR opcode 0xa6). Not produced
     by GLSL ES; usp.py used to size it 78 bytes, the writer and the reader
     both use 82.

    from smpblock import parse
    d = parse(5, raw_body)
"""
import struct

BODY = {5: 56, 6: 74, 9: 82}

# USP register-type codes used in the 4-valued fields (1-4)
REGTYPE4 = {1: "r", 2: "o", 3: "pa", 4: "i"}
# ... and in the 2-valued fields
REGTYPE2 = {1: "r", 2: "pa"}
# unpack destination component formats (block 6 per-channel field 3)
CFMT = {1: "f32", 2: "f16", 3: "c10", 4: "u8"}
# block 5 forced result format (driver table 0x750dc applied to n - 1)
FORCED = {1: "f32", 2: "f16", 3: "c10", 4: "u8", 6: "fmt6", 7: "fmt7", 8: None}
LOD = {0: "", 1: ".bias", 2: ".lod", 3: ".grad"}


def _u16s(raw, off, n):
    return list(struct.unpack_from("<%dH" % n, raw, off))


def _moe(raw, off):
    """format-control word, 4 MOE increment/swizzle words, 4 MOE base offsets"""
    fc = struct.unpack_from("<H", raw, off)[0]
    inc = _u16s(raw, off + 2, 4)
    base = _u16s(raw, off + 10, 4)
    return fc, inc, base


def moe_text(inc, base):
    out = []
    for w, b in zip(inc, base):
        if w & 0x8000:
            s = "+%d" % (w & 0xff)
        elif w & 0x4000:
            s = "sw" + "".join("xyzw"[(w >> (2 * k)) & 3] for k in range(4))
        else:
            s = "?%04x" % w
        out.append(s + ("@%d" % b if b else ""))
    return ",".join(out)


def iflags_text(f):
    """per-instruction flags (libusc FUN_00103fd0; driver FUN_0004f700)"""
    t = []
    if f & 1:
        t.append("iregs")       # internal registers live across it
    if f & 4:
        t.append("noskipinv")   # IR instruction had no skip-invalid flag
    if f & 0xf0:
        t.append("f%x" % (f >> 4 & 0xf))  # IR +0x88 nibble, meaning unknown
    if f & ~0xf5:
        t.append("?%x" % (f & ~0xf5))
    return "|".join(t) or "-"


def swizzle_text(s):
    """4 x 3-bit source-channel selects, result channel i <- texel channel"""
    return "".join("xyzw01??"[(s >> (3 * k)) & 7] for k in range(4))


def smp_fields(lo, hi):
    """The smp fields libusc's FUN_0010a3f0 writes (operand banks follow the
    generic slots of hw.py: src0 l14-20/h2/h18, src1 l7-13/l30-31/h17,
    src2 l0-6/l28-29/h16, dest l21-27 with h7 = pa)."""
    return {
        "op": hi >> 27,
        "pred": hi >> 24 & 7,
        "skipinv": hi >> 23 & 1,
        "nosched": hi >> 22 & 1,
        "rpt": hi >> 21 & 1,            # h12-15 is a repeat count, not a mask
        "syncstart": hi >> 20 & 1,
        "ext": hi >> 16 & 7,            # h18 src0, h17 src1, h16 src2
        "mask": hi >> 12 & 0xf,
        "dim": (hi >> 10 & 3) + 1,
        "lod": hi >> 8 & 3,
        "dpa": hi >> 7 & 1,
        "h5": hi >> 5 & 1,
        "cfmt": hi >> 3 & 3,            # h3 F16 coordinate, h4 C10 coordinate
        "s0bank": hi >> 2 & 1,
        "drc": hi & 3,
        "dst": lo >> 21 & 0x7f,
        "s0": lo >> 14 & 0x7f,
        "s1": lo >> 7 & 0x7f,
        "s2": lo & 0x7f,
        "s1bank": lo >> 30 & 3,
        "s2bank": lo >> 28 & 3,
    }


def smp_text(lo, hi):
    try:
        import hw
        x = hw.decode(lo, hi)
        if x is not None and not getattr(x, "unknown", 0):
            return hw.fmt(x)
    except Exception:
        pass
    f = smp_fields(lo, hi)
    from_tab = {(0, 0): "r", (1, 0): "o", (2, 0): "pa", (3, 0): "sa",
                (0, 1): "ix", (1, 1): "cg", (2, 1): "#", (3, 1): "i"}
    s0 = {(0, 0): "r", (1, 0): "pa", (0, 1): "o", (1, 1): "sa"}[(f["s0bank"], f["ext"] >> 2)]
    s2 = from_tab[(f["s2bank"], f["ext"] & 1)]
    s1 = from_tab[(f["s1bank"], f["ext"] >> 1 & 1)]
    cf = {0: "", 1: ".f16", 2: ".c10", 3: ".?"}[f["cfmt"]]
    return (f"smp{f['dim']}d{LOD[f['lod']]}{cf} drc{f['drc']} "
            f"{'pa' if f['dpa'] else 'r'}{f['dst']}, {s0}{f['s0']}, {s1}{f['s1']}, {s2}{f['s2']}"
            f" [mask {f['mask']:x}{' rpt' if f['rpt'] else ''}"
            f"{' skipinv' if f['skipinv'] else ''}{' nosched' if f['nosched'] else ''}"
            f"{' h5' if f['h5'] else ''}{' p%d' % f['pred'] if f['pred'] else ''}]")


def parse5(raw):
    flags, fc = struct.unpack_from("<HH", raw, 0)
    _, inc, base = _moe(raw, 2)
    smp_id = struct.unpack_from("<I", raw, 20)[0]
    f = _u16s(raw, 24, 12)
    d = {
        "type": 5,
        "nondep": flags & 1,
        "proj": flags >> 1 & 1,
        "flag2": flags >> 2 & 1,       # writer: IR sample +0x9c; driver +0x70
        "flags": flags,
        "fmtctl": fc,                  # b0 F16 format control on, b1 C10 on
        "moe_inc": inc,
        "moe_base": base,
        "id": smp_id,
        "iflags": f[0],
        "sampler": f[1],
        "swizzle": f[2],
        "dest": (REGTYPE2.get(f[3], f[3]), f[4]),     # smp destination (raw texel data)
        "usrc": (REGTYPE4.get(f[5], f[5]), f[6]),     # = the unpack's source when in place
        "scratch": (REGTYPE2.get(f[7], f[7]), f[8]),  # first temp USP may use
        "ilive": f[9],
        "ilive_c10": f[10],
        "forced_fmt": FORCED.get(f[11], f[11]),
    }
    if d["nondep"]:
        src, dim, ct, cn = _u16s(raw, 48, 4)
        d.update(coord_iter=src - 1, dim=dim, coord=(REGTYPE4.get(ct, ct), cn),
                 smp=(0x20000000, 0xe0411000))   # the reader's fixed template
    else:
        d["smp"] = struct.unpack_from("<II", raw, 48)
    return d


def parse6(raw):
    fc, inc, base = _moe(raw, 0)
    smp_id = struct.unpack_from("<I", raw, 18)[0]
    iflags, chans, mask = _u16s(raw, 22, 3)
    regtype = _u16s(raw, 28, 4)
    regnum = _u16s(raw, 36, 4)
    cfmt = _u16s(raw, 44, 4)
    coff = _u16s(raw, 52, 4)
    st, sn, at, an, tt, tn, ilive = _u16s(raw, 60, 7)
    out = []
    for i in range(4):
        if chans >> i & 1:
            out.append((REGTYPE4.get(regtype[i], regtype[i]), regnum[i],
                        CFMT.get(cfmt[i], cfmt[i]), coff[i]))
        else:
            out.append(None)
    return {
        "type": 6,
        "fmtctl": fc,
        "moe_inc": inc,
        "moe_base": base,
        "id": smp_id,
        "iflags": iflags,
        "chans": chans,          # channels to write (subset of mask)
        "mask": mask,            # channels of the destination that are live
        "out": out,              # per channel (bank, reg, format, component)
        "src": (REGTYPE4.get(st, st), sn),       # where the raw data are expected
        "smpdest": (REGTYPE2.get(at, at), an),   # = the sample block's dest
        "scratch": (REGTYPE2.get(tt, tt), tn),
        "ilive": ilive,
    }


def parse9(raw):
    w = struct.unpack_from("<20I", raw, 0)
    return {
        "type": 9,
        "id": w[0],
        # eight (register number, operand format code) pairs: the write's
        # coordinate and data operands (IR args 0..7)
        "args": [(w[1 + 2 * i], w[2 + 2 * i]) for i in range(8)],
        "w17": w[17],            # IR params +4
        "fmt": w[18],            # writer table 0x136c58 of IR params +0x70
        "w19": w[19],            # libusc FUN_0009af00()
        "iflags": struct.unpack_from("<H", raw, 80)[0],
    }


def parse(t, raw):
    if len(raw) != BODY[t]:
        raise ValueError(f"sample block {t}: {len(raw)} bytes, want {BODY[t]}")
    return {5: parse5, 6: parse6, 9: parse9}[t](raw)


def _reg(r):
    return "%s%d" % r


def text(d):
    if d["type"] == 5:
        h = (f"sample id{d['id']} t{d['sampler']} {'nondep' if d['nondep'] else 'dep'}"
             f"{' proj' if d['proj'] else ''}{' flag2' if d['flag2'] else ''} "
             f"swz {swizzle_text(d['swizzle'])} -> {_reg(d['dest'])} usrc {_reg(d['usrc'])} "
             f"tmp {_reg(d['scratch'])} if {iflags_text(d['iflags'])}")
        if d["ilive"]:
            h += f" ilive {d['ilive']:x}/{d['ilive_c10']:x}"
        if d["forced_fmt"]:
            h += f" fmt {d['forced_fmt']}"
        if d["fmtctl"] != 2 or any(x != 0x8001 for x in d["moe_inc"]) or any(d["moe_base"]):
            h += f" fc{d['fmtctl']} moe {moe_text(d['moe_inc'], d['moe_base'])}"
        if d["nondep"]:
            h += f" | coord tc{d['coord_iter']} {d['dim']}d {_reg(d['coord'])}"
        else:
            h += " | " + smp_text(*d["smp"])
        return h
    if d["type"] == 6:
        outs = " ".join("%s%d.%s[%d]" % o if o else "-" for o in d["out"])
        h = (f"unpack id{d['id']} {outs} from {_reg(d['src'])} smpdest {_reg(d['smpdest'])} "
             f"tmp {_reg(d['scratch'])} if {iflags_text(d['iflags'])}")
        if d["mask"] != d["chans"]:
            h += f" mask {d['mask']:x}"
        if d["ilive"]:
            h += f" ilive {d['ilive']:x}"
        if d["fmtctl"] != 2 or any(x != 0x8001 for x in d["moe_inc"]) or any(d["moe_base"]):
            h += f" fc{d['fmtctl']} moe {moe_text(d['moe_inc'], d['moe_base'])}"
        return h
    return (f"texwrite id{d['id']} args {d['args']} w17 {d['w17']:#x} fmt {d['fmt']} "
            f"w19 {d['w19']:#x} if {iflags_text(d['iflags'])}")


if __name__ == "__main__":
    import os
    import sys
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    import usp
    for s in usp.corpus(sys.argv[1]):
        bl = [b for b in s["blocks"] if "sample" in b]
        if not bl:
            continue
        print(f"{s['name']} ({s['kind']}) A={s['list_a']}")
        for b in bl:
            print("   ", text(b["sample"]))
