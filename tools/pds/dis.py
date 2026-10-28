"""Disassemble every PDS program in a pvrtrace capture (SGX545, DDK 1.7).

  dis.py CAPTURE.bin [--kick N] [--raw]
  dis.py --words 87000000 90000006 07000203 ...   (code words only, no capture)

A PDS program is a data segment (the constants it reads, 16-byte rows of
DS0 DS0 DS1 DS1) followed by code (32-bit instructions ending in HALT).
Programs are found from the things that point at them:

  - the VDM control stream of each kick (vertex-fetch programs, VDM state
    programs), whose pointer word carries the data size;
  - TA/3D state words `size_dwords << 24 | (addr - 0x2e000000) >> 4`
    (per-object pixel and secondary programs, the background object);
  - raw PDS addresses anywhere in captured memory (the pixel event program);
  - whatever is left in the PDS heaps that ends in HALT (data size inferred).

Field names and their status (confirmed / guess) are in PDS.md; bits the
decoder does not explain are printed as {?name=value}.
"""
import argparse
import os
import struct
import sys

# replace this script's directory: the name dis.py would shadow the stdlib module inspect imports
sys.path[0] = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "decode")

import cmdta  # noqa: E402
import layout  # noqa: E402
import memmap  # noqa: E402
import pvrlog  # noqa: E402

PDS_PIXEL_BASE = 0x2e000000          # base of TA-state pixel PDS pointers
USE_PIXEL_BASE = 0x37000000          # pixel shader heap: USE code bases 0..9
USE_VERTEX_BASE = 0x3c000000         # vertex shader heap: USE code bases 10..
HALT = 0xaf000000

DOUT = {2: "douti", 3: "doutd", 4: "doutt", 5: "doutu", 6: "douta"}


# ---------------------------------------------------------------- registers

def ds_name(bank, n):
    """A 32-bit data-store word. DS0 words 0x40.. are the PDS inputs."""
    if bank == 0 and n >= 0x40:
        return f"in{n - 0x40}"
    return f"ds{bank}[{n:#x}]" if n >= 10 else f"ds{bank}[{n}]"


def reg_u(f):
    """Unified 8-bit register number (dst field, shift source): DS0 0-3f, DS1 40-7f, inputs 80-."""
    if f < 0x40:
        return ds_name(0, f)
    if f < 0x80:
        return ds_name(1, f - 0x40)
    return f"in{f - 0x80}"


def reg_s1(f, halves=False):
    """src1 of a two-source op: 16-bit units on the DS0 side (inputs from word 0x40)."""
    s = ds_name(0, f >> 1)
    if halves:
        return s + (".hi" if f & 1 else ".lo")
    return s + (".hi?" if f & 1 else "")


def reg_s2(f, halves=False):
    """src2 of a two-source op: 16-bit units on the DS1 side."""
    s = ds_name(1, f >> 1)
    if halves:
        return s + (".hi" if f & 1 else ".lo")
    return s + (".hi?" if f & 1 else "")


def movs_word(sel, r1, r2):
    """MOVS swizzle select: 0/1 = src1 pair lo/hi (DS0 side), 2/3 = src2 pair lo/hi (DS1 side)."""
    if sel < 2:
        return (0, 2 * r1 + sel)
    return (1, 2 * r2 + sel - 2)


# ---------------------------------------------------------------- decoder

class Inst:
    def __init__(self, text, reads=(), dout=None, target=None, known=True):
        self.text = text
        self.reads = reads          # [(bank, word)] data-store words read
        self.dout = dout            # (dest, [(bank, word)] out0..3)
        self.target = target
        self.known = known


def decode(w):
    op, s, cc = w >> 28, (w >> 27) & 1, (w >> 24) & 7
    pred = {7: "", 0: "(p0) "}.get(cc, f"(cc{cc}?) ")
    if w == HALT:
        return Inst("halt")
    if op in (0, 2):
        dest = w & 0x1f
        sw = [(w >> 11) & 3, (w >> 9) & 3, (w >> 7) & 3, (w >> 5) & 3]
        r2, r1 = (w >> 13) & 0x1f, (w >> 18) & 0x3f
        outs = [movs_word(x, r1, r2) for x in sw]
        flags = ""
        if op == 2:
            flags += ".x2"          # top bits 0x2_ : seen on DOUTD with a 3rd (offset) word and DOUTA pairs
        if s:
            flags += ".s27"
        if cc != 7:
            flags += f".cc{cc}"     # 0x03 top byte: only on the closing DOUTU of vertex programs
        name = DOUT.get(dest, f"dout{dest}?")
        text = f"movs{flags} {name}, " + ", ".join(ds_name(*o) for o in outs)
        return Inst(text, reads=outs, dout=(dest, outs), known=dest in DOUT)
    if op in (4, 6, 0xc, 0xd):
        names = {(4, 0): "add", (4, 1): "sub?", (6, 0): "mul16", (6, 1): "mul16.s27?",
                 (0xc, 0): "or", (0xc, 1): "and", (0xd, 0): "xor", (0xd, 1): "xor.s27?"}
        halves = op == 6
        dst, s2, s1 = w & 0x7f, (w >> 9) & 0x7f, (w >> 16) & 0xff
        extra = f" {{?b7-8={(w >> 7) & 3}}}" if (w >> 7) & 3 else ""
        text = f"{pred}{names[(op, s)]} {reg_u(dst)}, {reg_s1(s1, halves)}, {reg_s2(s2, halves)}{extra}"
        reads = [(0, s1 >> 1), (1, s2 >> 1)]
        return Inst(text, reads=reads, known=s == 0 or op in (0xc, 0xd))
    if op == 8:
        s1, s2, sel = (w >> 16) & 0xff, (w >> 9) & 0x7f, (w >> 8) & 1
        src = reg_s2(s2) if sel else reg_s1(s1)
        extra = "" if (w & 0xff) == 0 and not s else f" {{?low={w & 0xff:#x} s27={s}}}"
        text = f"{pred}tst p0, {src} == 0{extra}"
        return Inst(text, reads=[(1, s2 >> 1)] if sel else [(0, s1 >> 1)])
    if op == 9:
        t = w & 0xffffff
        extra = f" {{?s27={s}}}" if s else ""
        text = {0: f"bra p0, {t}", 7: f"bra {t}"}.get(cc, f"bra cc{cc}?, {t}")
        return Inst(text + extra, target=t, known=cc in (0, 7) and not s)
    if op == 0xf:
        src, amt, dst = (w >> 16) & 0xff, (w >> 8) & 0xff, w & 0xff
        text = f"{pred}{'shr' if s else 'shl'} {reg_u(dst)}, {reg_u(src)}, {amt}"
        rd = [] if src >= 0x80 else [(src >> 6, src & 0x3f)]
        return Inst(text, reads=rd)
    return Inst(f".word {w:#010x}", known=False)


def plausible(w):
    """Could this word be code (used only to place a program whose pointer we did not find)?"""
    if w == HALT:
        return True
    op, cc = w >> 28, (w >> 24) & 7
    if op in (0, 2):
        return (w & 0x1f) in DOUT and cc in (3, 7)
    if op in (4, 6, 8, 0xc, 0xd, 0xf):
        return cc in (0, 7)
    if op == 9:
        return cc in (0, 7) and (w & 0xffffff) < 0x400
    return False


# ---------------------------------------------------------------- DOUT data

def use_addr(w0):
    idx, off = w0 & 0xf, ((w0 >> 4) & 0x3ffff) << 3
    base = USE_VERTEX_BASE + (idx - 10) * 0x80000 if idx >= 10 else USE_PIXEL_BASE + idx * 0x80000
    return base + off


def describe_dout(dest, v, mm, mem):
    """One line describing what a DOUT sends; None = a word computed at run time (temp/input)."""
    def h(x):
        return "<run time>" if x is None else f"{x:#010x}"
    if dest == 5:
        if v[0] is None:
            return f"USE task: w0={h(v[0])} w1={h(v[1])} w2={h(v[2])}"
        a = use_addr(v[0])
        tag = cmdta.tag(mm, mem, a) or "<not in a captured buffer>"
        return (f"USE task: code base {v[0] & 0xf}, pc {a:#x} {tag}; w0[31:22]={v[0] >> 22:#x} "
                f"w1={h(v[1])} w2={h(v[2])}")
    if dest == 3:
        a, c = v[0], v[1]
        s = f"DMA: addr {h(a)} {cmdta.tag(mm, mem, a) if a is not None else ''}"
        if c is not None:
            s += (f" ctrl {c:#010x} -> {(c & 0xff) + 1} dwords to attr {(c >> 8) & 0x3fff}"
                  f"{' [b22-24=%d]' % ((c >> 22) & 7) if (c >> 22) & 7 else ''}{' [b25]' if c >> 25 & 1 else ''}")
        return s + f" w2={h(v[2])}"
    if dest == 2:
        return f"iterate: w0={h(v[0])} w1={h(v[1])}" + ("" if None in v[:2] else "  " + douti(v[0], v[1]))
    if dest == 4:
        tags = " ".join(t for t in (cmdta.tag(mm, mem, x) for x in v if x is not None) if t)
        return "texture state: " + " ".join(h(x) for x in v) + (f"  {tags}" if tags else "")
    if dest == 6:
        reg = "" if v[1] is None else f" -> attr {(v[1] >> 8) & 0xff}"
        return f"attribute write: value {h(v[0])} ctrl {h(v[1])}{reg}"
    return ""


def douti(w0, w1):
    """DOUTI word 0, as libGLESv2 builds it from the compiler's iterated-input list (PDS.md)."""
    out = []
    if w0 >> 24 & 1:
        src = (w0 >> 11) & 0x3f
        name = {0x20: "colour", 0x22: "src22?", 0x24: "none"}.get(src, f"tc{src - 1}" if src else "src0?")
        prec = {2: " f16", 1: " lowp?"}.get(w0 >> 27 & 3, "")
        out.append(f"iterate {name} x{(w0 >> 22 & 3) + 1}{prec}{' b21' if w0 >> 21 & 1 else ''}"
                   f"{' last' if w0 >> 29 & 1 else ''}")
    if w0 & 0x1f:
        kind = {1: "2d", 2: "proj"}.get(w0 >> 9 & 3, f"type{w0 >> 9 & 3}?")
        out.append(f"sample tc{(w0 & 0x1f) - 1} {kind}{' last' if w0 >> 30 & 1 else ''}")
    unk = w0 & 0x801e01e0
    if (w0 >> 25 & 3) != 3:
        unk |= w0 & 0x06000000
    if not w0 >> 24 & 1:
        unk |= w0 & 0x3ef800 & ~0x12000
    if unk:
        out.append(f"{{?{unk:#x}}}")
    if w1:
        out.append(f"w1 {w1:#x}")
    return "; ".join(out)


# ---------------------------------------------------------------- programs

class Prog:
    def __init__(self, addr, data_size, kind, ref, size_known=True):
        self.addr, self.data_size, self.kind, self.ref = addr, data_size, kind, [ref]
        self.size_known = size_known
        self.code = addr + ((data_size + 15) & ~15)
        self.words = []

    def load(self, mem, limit=96):
        self.words = []
        for i in range(limit):
            w = mem.u32(self.code + 4 * i)
            if w is None:
                return False
            self.words.append(w)
            if w == HALT:
                # a branch may skip past this HALT to code after it
                tgts = [decode(x).target for x in self.words if decode(x).target is not None]
                if not tgts or max(tgts) < len(self.words):
                    return True
        return False

    def end(self):
        return self.code + 4 * len(self.words)


def heap_of(mm, va):
    return cmdta.heapname(mm, va)


def vdm_programs(log, mm, kicks):
    """Walk each kick's VDM control stream (start = client TA command word 0x18)."""
    out = []
    for kick in kicks:
        k = cmdta.kicks(log)[kick - 1]
        mem, ccb, off, d = cmdta.cmd_bytes(log, mm, k, kick, 0x200)
        shared = layout.layout(mm.arch)["_SGXMKIF_CMDTA_SHARED_"].size
        va = struct.unpack_from("<I", d, shared + 0x18)[0]
        for _ in range(4096):
            w = mem.u32(va)
            if w is None:
                break
            t = w >> 28
            if t == 0xc:
                break
            if t == 8:              # index list: header, index address, 2 words
                va += 16
                continue
            if t in (4, 6, 7, 0xf):
                info = mem.u32(va + 4)
                addr = (w & 0x0fffffff) << 4
                if t in (4, 6):
                    size, kind = (info >> 26) << 4, f"VDM state (type {t:x}, info {info:08x})"
                else:
                    size, kind = (info & 0x7f) << 4, f"vertex fetch (VDM type {t:x}, info {info:08x})"
                out.append((kick, Prog(addr, size, kind, f"VDM {va:#x}")))
                va += 8
                continue
            break
    return out


def state_programs(mm, mem, kick):
    """TA/3D state words: size_dwords << 24 | (addr - 0x2e000000) >> 4."""
    out = []
    for va, d, b in mem.regions:
        hn = heap_of(mm, va)
        # client state blocks (DMA sources of VDM state programs) and the 3D command;
        # the parameter buffer's copies are TA output and its free list matches by chance
        if hn not in ("pdsvertex", "kdata"):
            continue
        n = len(d) // 4
        for i, w in enumerate(struct.unpack_from(f"<{n}I", d)):
            dw = w >> 24
            if not 1 <= dw <= 0x40:
                continue
            a = PDS_PIXEL_BASE + ((w & 0xffffff) << 4)
            if heap_of(mm, a) != "pdspixel" or not mm.find(a):
                continue
            p = Prog(a, dw * 4, "pixel (TA state)", f"{hn} {va + 4 * i:#x}")
            if p.load(mem) and all(decode(x).known for x in p.words):
                out.append((kick, p))
    return out


def raw_programs(mm, mem, kick, have):
    """Raw PDS addresses in kernel data (the pixel event program); data size inferred."""
    out = []
    for va, d, b in mem.regions:
        if heap_of(mm, va) != "kdata":
            continue
        n = len(d) // 4
        for i, w in enumerate(struct.unpack_from(f"<{n}I", d)):
            if heap_of(mm, w) not in ("pdspixel", "pdsvertex") or w & 15 or not mm.find(w):
                continue
            if any(p.addr == w for _, p in have):
                continue
            p = infer(mem, w, f"kdata {va + 4 * i:#x}")
            if p:
                p.kind = "raw kdata pointer (pixel event)" if heap_of(mm, w) == "pdspixel" else "raw kdata pointer"
                out.append((kick, p))
    return out


def infer(mem, start, ref):
    """Program starting at `start` with unknown data size: code begins at the first
    16-byte row from which every word decodes up to a HALT that ends the program."""
    for code in range(start, start + 0x400, 16):
        p = Prog(start, code - start, "inferred", ref, size_known=False)
        if not p.load(mem, 64):
            continue
        rows = (code - start) // 16
        if all(plausible(x) and decode(x).known for x in p.words):
            # every data word the code reads (temps and inputs aside) is inside the data segment
            if all(r[1] >= 0x30 or r[1] // 2 < rows for x in p.words for r in decode(x).reads):
                return p
    return None


def leftovers(mm, mem, kick, have):
    """HALT-terminated code in the PDS heaps that no pointer led to."""
    out = []
    covered = []
    for _, p in have:
        covered.append((p.addr, p.end()))
    for va, d, b in mem.regions:
        if heap_of(mm, va) not in ("pdspixel", "pdsvertex"):
            continue
        n = len(d) // 4
        ws = struct.unpack_from(f"<{n}I", d)
        prev_end = va
        spans = sorted(c for c in covered if va <= c[0] < va + len(d))
        for i, w in enumerate(ws):
            a = va + 4 * i
            if any(s <= a < e for s, e in spans):
                prev_end = max(prev_end, next(e for s, e in spans if s <= a < e))
                continue
            if w != HALT:
                continue
            j = i
            while j > 0 and plausible(ws[j - 1]) and ws[j - 1] != HALT and va + 4 * (j - 1) >= prev_end:
                j -= 1
            code = va + ((4 * j + 15) & ~15)
            if code > a:
                code -= 16
            # data: the rows the code reads, but not into the previous program
            rows = 0
            for x in ws[(code - va) // 4:i + 1]:
                for bank, n in decode(x).reads:
                    if n < 0x30:
                        rows = max(rows, n // 2 + 1)
            start = max((prev_end + 15) & ~15, code - 16 * rows)
            p = Prog(start, code - start, "unreferenced", "none (scan)", size_known=False)
            if p.load(mem, 64):
                out.append((kick, p))
                covered.append((p.addr, p.end()))
                spans = sorted(c for c in covered if va <= c[0] < va + len(d))
            prev_end = a + 4
    return out


# ---------------------------------------------------------------- output

def show(kick, p, mm, mem, raw):
    used = {}
    for w in p.words:
        ins = decode(w)
        for bank, n in ins.reads:
            used.setdefault((bank, n), []).append(ins)
    print(f"== {p.kind} @ {p.addr:#x}: data {p.data_size:#x} bytes{'' if p.size_known else ' (inferred)'}, "
          f"code @ {p.code:#x} ({len(p.words)} inst); kick {kick}; from {', '.join(p.ref)}")
    rows = (p.code - p.addr) // 16
    for r in range(rows):
        ws = mem.words(p.addr + 16 * r, 4) or [0] * 4
        if not any(ws) and not raw:
            continue
        cells = []
        for c, x in enumerate(ws):
            bank, n = (0, 2 * r + c) if c < 2 else (1, 2 * r + c - 2)
            cells.append(f"{ds_name(bank, n)}={x:08x}")
        print(f"   {p.addr + 16 * r:#010x}: " + "  ".join(cells))
    for i, w in enumerate(p.words):
        ins = decode(w)
        line = f"   {i:3d} {p.code + 4 * i:#010x}: {w:08x}  {ins.text}"
        if ins.dout:
            dest, outs = ins.dout
            vals = []
            for bank, n in outs:
                if n >= 0x30 or 16 * (n // 2) >= p.code - p.addr:
                    vals.append(None)
                else:
                    vals.append(mem.u32(p.addr + 16 * (n // 2) + 4 * (n % 2) + (8 if bank else 0)))
            line += "\n" + " " * 20 + "; " + describe_dout(dest, vals, mm, mem)
        print(line)
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", nargs="?")
    ap.add_argument("--words", nargs="+", help="disassemble these hex code words")
    ap.add_argument("--kick", type=int, help="only this kick (default: every kick)")
    ap.add_argument("--raw", action="store_true", help="print all-zero data rows too")
    a = ap.parse_args()
    if a.words:
        for i, x in enumerate(a.words):
            w = int(x, 16)
            print(f"{i:3d}: {w:08x}  {decode(w).text}")
        return
    log = pvrlog.load(a.capture)
    mm = memmap.build(log)
    nk = len(cmdta.kicks(log))
    kicks = [a.kick] if a.kick else list(range(1, nk + 1))
    seen = {}
    for kick in kicks:
        snaps = [s for s in log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == kick]
        mem = memmap.Memory(mm, snaps)
        found = []
        for k, p in vdm_programs(log, mm, [kick]):
            if p.load(mem):
                found.append((k, p))
        found += state_programs(mm, mem, kick)
        found += raw_programs(mm, mem, kick, found)
        found += leftovers(mm, mem, kick, found)
        for k, p in sorted(found, key=lambda t: t[1].addr):
            key = (p.addr, p.code, tuple(p.words), bytes(mem.read(p.addr, p.code - p.addr) or b""))
            if key in seen:
                if p.ref[0] not in seen[key].ref and p.ref[0] != "none (scan)":
                    seen[key].ref += p.ref
                continue
            seen[key] = p
            p.kick, p.mem = k, mem
    groups = {}
    for p in sorted(seen.values(), key=lambda p: (p.addr, p.kick)):
        if p.kind == "unreferenced":
            groups.setdefault((tuple(p.words), p.code - p.addr), []).append(p)
        else:
            show(p.kick, p, mm, p.mem, a.raw)
    for ps in groups.values():
        show(ps[0].kick, ps[0], mm, ps[0].mem, a.raw)
        if len(ps) > 1:
            print(f"   ... and {len(ps) - 1} more with this code and data size at "
                  + " ".join(f"{q.addr:#x}" for q in ps[1:16]) + (" ..." if len(ps) > 16 else ""))
            print()


if __name__ == "__main__":
    main()
