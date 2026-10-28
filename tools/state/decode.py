"""Annotate every TA/3D state word of a pvrtrace capture (see STATE.md).

    python3 tools/state/decode.py CAPTURE.bin [--kick N] [--brief]

For each SGX_DOKICK it follows, from the TA command in the CCB:
  the client header and tail of SGXMKIF_CMDTA (the kernel's CMDTA_SHARED in between is skipped),
  the render details (3D register values), the PB descriptor, the render-target data,
  the VDM control stream: state programs -> their DMA'd TA state blocks (ISP/TSP/viewport/...),
  index lists and vertex programs, and the pixel PDS programs the state blocks point at
  (DOUTU, DOUTI, DOUTT texture words), and the end-of-tile USSE program's PBE words
  (render target address, stride, size, format).
Fields are tagged [D] confirmed by a replay flip on hardware, [D] confirmed by a capture
diff (or read straight from the DDK code that writes it), [G] guess.

--brief prints one line per word, which is what `diff` between two captures wants.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "decode"))
import importlib.util  # noqa: E402
import cmdta  # noqa: E402
import memmap  # noqa: E402
import pvrlog  # noqa: E402

# tools/pds/dis.py, loaded by path: its name would shadow the standard library's dis
_spec = importlib.util.spec_from_file_location("pdsdis", os.path.join(HERE, "..", "pds", "dis.py"))
pdsdis = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pdsdis)

PDS_PIXEL = 0x2e000000
PIX_CODE = 0x37000000
VTX_CODE = 0x3c000000


def f32(v):
    return struct.unpack("<f", struct.pack("<I", v))[0]


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


CMP = ["never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"]
PASS = {0: "opaque", 1: "translucent", 2: "translucent+discard", 3: "viewport/scissor generate",
        4: "punch-through", 5: "depth-only"}
SOP = ["keep", "zero", "replace", "incr", "decr", "invert", "incr_wrap", "decr_wrap"]


class Out:
    def __init__(self, brief):
        self.brief = brief
        self.lines = []

    def h(self, s):
        self.lines.append(s if not self.brief else "# " + s)

    def w(self, where, v, text):
        self.lines.append(f"  {where:<24} {v:08x}  {text}")


# --------------------------------------------------------------- field tables

def ispa(v):
    f = [f"dcmp={CMP[bits(v, 22, 3)]} [C]", "dwrite=off [D]" if v & 0x100000 else "dwrite=on [D]",
         f"passtype={bits(v, 25, 3)} ({PASS.get(bits(v, 25, 3), '?')}) [D]"]
    if v & 0x200000:
        f.append("b21 no-colour-write (mask 0 or a blend that keeps dst) [D]")
    if v & 0x800:
        f.append("b11 two-sided stencil [D]")
    if v & 0x200:
        f.append("b9 ISPB follows [D]")
    if v & 0x100:
        f.append("b8 ISPC (stencil) follows [D]")
    if bits(v, 15, 4):
        f.append(f"b15-18 objtype={bits(v, 15, 4)} [D]")
    if v & 0xff:
        f.append(f"sref={v & 0xff} [D]")
    rest = v & ~(0x1fc00000 | 0x100000 | 0x200000 | 0x800 | 0x300 | 0x78000 | 0xff | 0xe000000)
    if rest:
        f.append(f"?{rest:#x}")
    return "ISPA: " + ", ".join(f)


def ispb(v):
    f = [f"colour mask (R8 G4 B2 A1)={bits(v, 4, 4):x} [D]", f"line/point width*16={bits(v, 8, 8)} [D]"]
    if v & 0x800000:
        f.append("b23 depth bias (polygon offset) [D]")
    rest = v & ~(0xf0 | 0xff00 | 0x800000)
    if rest:
        f.append(f"?{rest:#x}")
    return "ISPB: " + ", ".join(f)


def ispc(v):
    return (f"ISPC stencil: scmp={CMP[bits(v, 25, 3)]} sfail={SOP[bits(v, 22, 3)]} zfail={SOP[bits(v, 19, 3)]} "
            f"zpass={SOP[bits(v, 16, 3)]} comparemask={bits(v, 8, 8):#x} writemask={bits(v, 0, 8):#x} [D]")


def pdsword(v, what):
    a = PDS_PIXEL + ((v & 0x3ffffff) << 4)
    return f"{what}: PDS pixel program {a:#x}, data {bits(v, 26, 6) * 16:#x} bytes [D]"


def mtectl(v):
    cull = {0: "none", 1: "1 (window, GL front=CCW: cull back)", 2: "2 (window, GL front=CCW: cull front)",
            3: "?"}[v & 3]
    rest = v & ~3
    return f"MTE ctrl: cull={cull} [C]; rest {rest:#x} (GL's constant 0x1555620; 0x40 on the clear) [G]"


def outsel(v):
    return (f"output select: {bits(v, 16, 8)} dwords per vertex [D], "
            f"{'point size, ' if v & 0x100 else ''}flags {v & 0xffff:#x} [G]")


HDR = [  # (header bit, nwords, name, decoder)
    (0x1, 1, "ISPA", ispa), (0x2, 1, "ISPB", ispb), (0x4, 1, "ISPC", ispc),
    (0x8, 1, "back ISPA", ispa), (0x10, 1, "back ISPB", ispb), (0x20, 1, "back ISPC", ispc),
    (0x40, 3, "PDS", None), (0x80, 2, "region clip", None), (0x100, 6, "viewport", None),
    (0x200, 1, "w0", None), (0x400, 1, "output select", outsel), (0x800, 1, "wclamp", None),
    (0x1000, 1, "MTE ctrl", mtectl), (0x4000, 2, "TSP coord sizes", None),
]


def ta_block(o, mem, va, n):
    """A TA state block: header word, then the groups its bits name, in header-bit order."""
    ws = mem.words(va, n)
    hdr = ws[0]
    o.w(f"{va:#x}+0x0", hdr, "TA state header: " + " ".join(nm for b, _, nm, _ in HDR if hdr & b) + " [D]")
    i = 1
    pds = []
    for b, cnt, nm, dec in HDR:
        if not hdr & b:
            continue
        if b == 0x4000 and not hdr & 0x8000:
            cnt = 1
        g = ws[i:i + cnt]
        for j, v in enumerate(g):
            where = f"{va:#x}+{4 * (i + j):#x}"
            if dec:
                o.w(where, v, dec(v))
            elif nm == "PDS":
                t = ["secondary", "USSE attributes/temps", "primary"][j]
                if j == 1:
                    o.w(where, v, f"PDS {t}: {v:#x} (from the fragment program object +0x64) [G]")
                else:
                    o.w(where, v, pdsword(v, f"PDS {t}"))
                    pds.append(v)
            elif nm == "region clip":
                if j == 0:
                    o.w(where, v, f"region clip: enable={v >> 31} xmin={bits(v, 16, 10)} xmax={bits(v, 0, 10)} "
                                  f"(32-px tiles) [D]")
                else:
                    o.w(where, v, f"region clip: ymin={bits(v, 16, 10)} ymax={bits(v, 0, 10)} (16-px tiles) [D]")
            elif nm == "viewport":
                k = ["x offset", "x scale", "y offset", "y scale", "z offset", "z scale"][j]
                o.w(where, v, f"viewport {k} = {f32(v):g} [{'C' if j == 0 else 'D'}]")
            elif nm == "wclamp":
                o.w(where, v, f"MTE W clamp = {f32(v):g} [D]")
            elif nm == "TSP coord sizes":
                if j == 0:
                    o.w(where, v, "texcoord sizes, 3 bits per set (1,2,3 comps; 5 = 4) [D]")
                else:
                    o.w(where, v, "texcoord sets with precision value <= 2 (bitmask) [D]")
            else:
                o.w(where, v, f"{nm} [G]")
        i += cnt
    for j in range(i, n):
        if ws[j]:
            o.w(f"{va:#x}+{4 * j:#x}", ws[j], "past the groups the header names [G]")
    return pds


# --------------------------------------------------------------- textures

def doutt(o, where, ws):
    w0, w1, w2, w3 = ws
    o.w(where + " T0", w0, f"tex word 0: {tex0(w0)}")
    fmt = bits(w1, 24, 5)
    o.w(where + " T1", w1, f"tex word 1: width={bits(w1, 12, 12) + 1} height={bits(w1, 0, 12) + 1} [D], "
                           f"format {fmt:#x} {TEXFMT.get(fmt, '?')} [D], layout {bits(w1, 29, 3)} "
                           f"({TEXLAYOUT.get(bits(w1, 29, 3), '?')}) [D]")
    o.w(where + " T2", w2, f"tex word 2: base address {w2:#x} [D]")
    o.w(where + " T3", w3, f"tex word 3: {tex3(w3)}")


TEXFMT = {0x02: "RGBA4444", 0x04: "RGBA5551", 0x05: "RGB565", 0x0c: "BGRA8888 (render target)",
          0x0d: "RGBA8888 (also RGB888 uploads)", 0x19: "A8", 0x1a: "L8", 0x1b: "LA88"}
TEXLAYOUT = {0: "twiddled, power of two", 3: "linear, NPOT", 6: "strided (stride in T0)"}
WRAP = ["repeat", "mirror", "clamp", "?3", "?4", "?5", "?6", "?7"]


def tex0(v):
    if v & 0x07e00000 != 0x07e00000:          # strided layout: T0 holds the stride instead
        return (f"strided: stride/16-1 = {bits(v, 18, 6)} [G], wrap S={WRAP[bits(v, 6, 3)]} "
                f"T={WRAP[bits(v, 3, 3)]} [D], rest {v & 0x3fe00:#x} [G]")
    mip = not v & 0x40000
    return (f"wrap S={WRAP[bits(v, 6, 3)]} T={WRAP[bits(v, 3, 3)]} [C], min={'linear' if v & 0x400 else 'nearest'} "
            f"[D], mag={'linear' if v & 0x1000 else 'nearest'} [C], "
            + (f"mipmapped, mip filter={'linear' if v & 0x200 else 'nearest'} [D]" if mip else "no mipmaps [D]")
            + (", b30 (L/A formats) [D]" if v & 0x40000000 else "")
            + (f", ?{v & ~0x47fe17f8:#x}" if v & ~0x47fe17f8 else ""))


def tex3(v):
    return f"{v:#x}: the shader's per-sample override half of (state & mask) | override [D]"


# --------------------------------------------------------------- PDS programs

def prog_words(mem, addr, dsize):
    code = addr + ((dsize + 15) & ~15)
    out = []
    for i in range(64):
        w = mem.u32(code + 4 * i)
        if w is None:
            break
        out.append(w)
        if w == pdsdis.HALT:
            break
    data = mem.words(addr, (dsize + 3) // 4) or []
    return data, out


def ds(data, bank, n):
    i = (n >> 1) * 4 + (n & 1) + 2 * bank
    return data[i] if i < len(data) else None


def douts(mem, addr, dsize):
    """[(dest, [words])] of every MOVS with all-constant sources."""
    data, code = prog_words(mem, addr, dsize)
    res = []
    for w in code:
        i = pdsdis.decode(w)
        if i.dout:
            dest, srcs = i.dout
            vals = [ds(data, b, n) if n < 0x30 else None for b, n in srcs]
            res.append((dest, vals))
    return res


def use_task(w0):
    base = w0 & 0xf
    off = bits(w0, 4, 18) * 8
    return (VTX_CODE + (base - 10) * 0x80000 if base >= 10 else PIX_CODE + base * 0x80000) + off


def pixel_program(o, mem, word):
    addr = PDS_PIXEL + ((word & 0x3ffffff) << 4)
    dsize = bits(word, 26, 6) * 16
    o.h(f"pixel PDS program {addr:#x} (data {dsize:#x})")
    for dest, v in douts(mem, addr, dsize):
        where = f"{addr:#x} dout{dest}"
        if dest == 5 and v[0] is not None:
            o.w(where, v[0], f"DOUTU: USSE task at {use_task(v[0]):#x}, temps/4={v[0] >> 24} [D]")
            if v[1] is not None:
                o.w(where, v[1], f"DOUTU w1: iterations={v[1] >> 24 & 1} samples={v[1] >> 25 & 1} [D]")
        elif dest == 2 and v[0] is not None:
            o.w(where, v[0], f"DOUTI: {pdsdis.douti(v[0], v[1] or 0)}")
        elif dest == 4 and None not in v:
            doutt(o, where, v)
        elif dest == 3 and v[0] is not None:
            o.w(where, v[0], f"DOUTD: DMA {bits(v[1], 0, 8) + 1} dwords from {v[0]:#x} to attr "
                             f"{bits(v[1], 8, 14)} (uniforms) [D]")


# --------------------------------------------------------------- end of tile (PBE)

def limm(lo, hi):
    return (lo & 0x1fffff) | bits(hi, 4, 5) << 21 | bits(hi, 12, 6) << 26, bits(lo, 21, 3)


def eot(o, mem, va):
    o.h(f"end-of-tile USSE program {va:#x}: LIMM r0-r3 = PBE words, then two EMITPIX")
    regs = {}
    side = None
    for i in range(1, 16):
        lo, hi = mem.u32(va + 8 * i), mem.u32(va + 8 * i + 4)
        if hi is None:
            break
        if hi & 0xfff00000 == 0xfca00000:
            v, r = limm(lo, hi)
            regs[r] = v
        elif hi & 0xffff0000 == 0xfb240000 or hi & 0xffff0000 == 0xfb200000 and lo & 0xffff == 0x8180:
            side = bits(lo, 22, 6) | bits(hi, 3, 6) << 6 | bits(hi, 22, 2) << 12
            break
    names = ["PBE 0: clip min y<<16|x [D]", "PBE 1: clip max y<<16|x", "PBE 2", "PBE 3: address [D]"]
    for r in range(4):
        v = regs.get(r)
        if v is None:
            continue
        t = names[r]
        if r == 1:
            t += f" = ({bits(v, 0, 16)}, {bits(v, 16, 16)}) -> {bits(v, 0, 16) + 1}x{bits(v, 16, 16) + 1} [C]"
        if r == 2:
            t += (f": stride=({bits(v, 16, 12)}+1)*2={(bits(v, 16, 12) + 1) * 2} px [C], "
                  f"b15={v >> 15 & 1} [D], rot/aa {bits(v, 28, 4):#x} [D], rest {v & 0x7fff:#x} [G]")
        o.w(f"{va:#x} r{r}", v, t)
    if side is not None:
        o.w(f"{va:#x} sideband", side, f"PBE sideband: format code={side & 0x3f} [D], b9 always, "
                                       f"dither={side >> 10 & 1} [D], b12={side >> 12 & 1} [G]")


# --------------------------------------------------------------- CCB command

HEAD = {0x00: "command size, rounded to 64 [D]", 0x04: "TA flags (from kick flags) [D]",
        0x08: "background object PDS secondary [D]", 0x0c: "background object PDS middle word [D]",
        0x10: "background object PDS primary (samples the RT: load old contents) [D]",
        0x14: "0x40000000 + 0x10000000 unless kick flag 0x2000000; else n<<27 [D]",
        0x18: "constant 0x50000000 [D]", 0x1c: "3D flags (from kick flags; 0x400 = render) [D]",
        0x20: "kick +0x00 [G]", 0x24: "PB descriptor (kdata) [D]",
        0x38: "render details (kdata) [D]", 0x3c: "RT dependency block (sync addresses) [G]",
        0x40: "RT data (kdata) [D]", 0x44: "RT data entry of this render [D]",
        0x48: "kick +0x08 [G]", 0x4c: "multi-RT value [G]", 0x50: "multi-RT value [G]",
        0x54: "deferred render-details updates: count [D]"}
TAIL = {0x238: "TE AA: 0x20000000 | b31 x2 AA in X | b30 in Y [D]",
        0x23c: "TE MTILE1: b31 4x4 mode, macrotile X boundaries b22/b12/b0 (32-px tiles) [D]",
        0x240: "TE MTILE2: macrotile Y boundaries b22/b12/b0 (16-px tiles) [D]",
        0x244: "TE screen: last tile X (b0-11), last tile Y (b12-) [D]",
        0x248: "tiles per macrotile [D]", 0x24c: "TA flags 2 [D]",
        0x250: "VDM control stream base [C]", 0x254: "kick +0x104 [G]",
        0x258: "region header array of this render [D]",
        0x25c: "tail pointer array [D]", 0x260: "1e-6 constant [D]", 0x264: "1e-6 constant [D]",
        0x268: "screen size (h-1)<<16 | (w-1) [D]", 0x26c: "kick +0xec [G]",
        0x270: "kick +0xe8 [G]", 0x274: "3D params heap base (PB base & 0xfff00000) [D]",
        0x278: "kick +0x20 << 24 [G]", 0x27c: "kick +0x1c [G]", 0x29c: "RT data entries - 1 [D]",
        0x2a0: "RT +0x68 << 12 [D]", 0x2a4: "RT +0x14 (0x88 = RT flag b0, no AA) [D]"}

RD = {0x00: "3D request base (PB base & 0xfff00000) [D]", 0x04: "ZLS request base [D]",
      0x08: "kick +0x90 | 0x80 [G]", 0x0c: "kick +0x70 | +0x18 [G]", 0x10: "RT +0x10 | b31 [D]", 0x14: "depth bias units (glPolygonOffset, rounded) [D]",
      0x18: "depth bias factor (float) [D]",
      0x20: "region headers, this render [D]", 0x24: "kick +0x4c [G]", 0x28: "region headers, next render [D]",
      0x2c: "ZLS control: 0x111000 | (xtiles-1)<<3 | drawable bits [D]",
      0x30: "depth store address - ZLS base [D]", 0x34: "stencil store address - ZLS base [D]",
      0x38: "b31 | depth load address - ZLS base [D]", 0x3c: "b31 | stencil load address - ZLS base [D]",
      0x40: "ISP background depth (1.0; not glClearDepth) [C]", 0x44: "kick +0x80 | +0x14 [G]",
      0x48: "background object: 0x2000000 | (block - PB base)>>4 [D]", 0x4c: "RT +0x1c [D]",
      0x50: "RT +0x18 [D]", 0x54: "2 = TA dependency [G]",
      0x58: "constant 0x30 [D]", 0x5c: "pixel event PDS program (end of tile) [D]",
      0x60: "pixel event data rows [D]", 0x64: "kick +0x8c [G]", 0x68: "RT flag b0 [D]",
      0xcc: "b8 = kick flag 0x8000000 [D]"}

PBD = {0x00: "flags (b1-4 = app-hint thresholds) [D]", 0x04: "PB pages [D]",
       0x08: "free list head: last<<16 | first [D]", 0x0c: "(last-1)<<16 [D]", 0x10: "free list address [D]",
       0x14: "pages in use? (0 before the first TA) [G]", 0x18: "copy of +0x08 [D]", 0x1c: "copy of +0x0c [D]",
       0x28: "TA threshold = pages + 0x10 [D]", 0x2c: "ZLS threshold = pages - 0x22 [D]",
       0x30: "global threshold = pages - 0x22 [D]", 0x34: "PDS threshold = min - 0x100 [D]",
       0x38: "PB block list {pages, first|last<<16, address, 0} [D]"}


def named(o, mem, base, n, table, label, skip=()):
    o.h(f"{label} @ {base:#x}")
    ws = mem.words(base, n) or []
    for i, v in enumerate(ws):
        off = 4 * i
        if off in skip:
            continue
        t = table.get(off)
        if t or v:
            o.w(f"{label}+{off:#x}", v, (t or "[G]") + tag(mem, v))


def tag(mem, v):
    t = cmdta.tag(mem.mm, mem, v)
    return f"  {t}" if t else ""


# --------------------------------------------------------------- VDM stream

def vdm(o, mem, va):
    o.h(f"VDM control stream @ {va:#x}")
    blocks = []
    for _ in range(256):
        w = mem.u32(va)
        if w is None:
            break
        t = w >> 28
        if t == 0xc:
            o.w(f"{va:#x}", w, "VDM terminate [D]")
            break
        if t in (4, 6):
            info = mem.u32(va + 4)
            addr = (w & 0xfffffff) << 4
            o.w(f"{va:#x}", w, f"VDM state program (type {t}) at {addr:#x} [D]")
            o.w(f"{va + 4:#x}", info, f"  info: data {bits(info, 26, 6) * 16:#x} bytes [D], "
                                      f"low {info & 0x3ffffff:#x} (attr dwords/4 | buffer index; 0xe100 / "
                                      f"0x2022400 forms) [G]")
            blocks.append((t, addr, bits(info, 26, 6) * 16))
            va += 8
            continue
        if t == 8:
            a, b, c, d, e = (mem.u32(va + 4 * k) for k in range(1, 6))
            o.w(f"{va:#x}", w, f"VDM index list: count={w & 0xffff} prim={bits(w, 26, 2)} "
                               f"(0 tri, 1 line, 2 point, 3 strip; fan = b28) flags {w & 0x00c00000:#x} [D]")
            o.w(f"{va + 4:#x}", a, f"  index buffer {a & ~3:#x}{' 32-bit' if c & 0x1000000 else ' 16-bit'} [D]"
                                   + tag(mem, a & ~3))
            o.w(f"{va + 8:#x}", b, f"  (batch-1)<<28 = {b >> 28} | index base {b & 0xffffff:#x} [D]")
            o.w(f"{va + 12:#x}", c, "  max index [G]")
            vaddr = (d & 0xfffffff) << 4
            o.w(f"{va + 16:#x}", d, f"  vertex PDS program {vaddr:#x}, (vertices per USSE task-1) = {d >> 28} [D]")
            o.w(f"{va + 20:#x}", e, f"  info: data {bits(e, 0, 7) * 16:#x} bytes, in {bits(e, 7, 15) * 4} dw, "
                                    f"out {bits(e, 24, 8) * 4} dw, b22-23={bits(e, 22, 2)} [D]")
            blocks.append((8, vaddr, bits(e, 0, 7) * 16))
            va += 24
            continue
        o.w(f"{va:#x}", w, "?? unknown VDM word")
        break
    return blocks


def state_program(o, mem, addr, dsize):
    o.h(f"VDM state program {addr:#x}")
    pds = []
    for dest, v in douts(mem, addr, dsize):
        if dest == 3 and v[0]:
            n = bits(v[1], 0, 8) + 1
            sec = bits(v[1], 25, 1)
            o.w(f"{addr:#x} doutd", v[0], f"DMA {n} dwords from {v[0]:#x} into "
                                          f"{'secondary' if sec else 'primary'} attributes, kind "
                                          f"{bits(v[1], 22, 3)} [D]")
            first = mem.u32(v[0])
            if first is not None and first & 0x3f and not first >> 16:
                pds += ta_block(o, mem, v[0], n)
            else:
                o.h(f"vertex secondary attributes (uniforms, driver constants) @ {v[0]:#x}")
                for j, x in enumerate(mem.words(v[0], n) or []):
                    if x:
                        o.w(f"{v[0]:#x}+{4 * j:#x}", x, f"= {f32(x):g}" + tag(mem, x))
        elif dest == 5 and v[0] is not None:
            o.w(f"{addr:#x} doutu", v[0], f"USSE state/vertex task {use_task(v[0]):#x} [D]")
        elif dest == 6 and v[0] is not None:
            o.w(f"{addr:#x} douta", v[0], f"DOUTA value, ctrl {v[1]:#x} [G]")
    return pds


# --------------------------------------------------------------- main

def kick(o, log, mm, k, n):
    snaps = [s for s in log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == n]
    mem = memmap.Memory(mm, snaps)
    _, ccb, off, _ = cmdta.cmd_bytes(log, mm, k, n, 4)
    base = ccb.devva + off - 0x58
    o.h(f"===== kick {n}: TA command at CCB {base:#x} (CMDTA_SHARED at +0x58..+0x238 skipped)")
    named(o, mem, base, 0x58 // 4, HEAD, "cmd")
    named(o, mem, base + 0x238, (0x2a8 - 0x238) // 4,
          {k - 0x238: v for k, v in TAIL.items()}, "cmd+0x238")
    hdr = mem.words(base, 0x58 // 4)
    tail = mem.words(base + 0x238, 0x1e)
    ndef = hdr[0x54 // 4]
    if ndef:
        o.h(f"deferred render-details updates @ {base + 0x2a8:#x}")
        rd = hdr[0x38 // 4]
        for i, (va, v) in enumerate(zip(*[iter(mem.words(base + 0x2a8, 2 * ndef))] * 2)):
            o.w(f"cmd+{0x2a8 + 8 * i:#x}", v, f"render details+{va - rd:#x} := this [D]")
    if hdr[0x38 // 4]:
        rd = hdr[0x38 // 4]
        named(o, mem, rd, 0xd0 // 4, RD, "render details")
        o.w("render details+0x13c", mem.u32(rd + 0x13c) or 0, "x<<22 (0x7c00000 default) [G]")
        ev = mem.u32(rd + 0x5c)
        if ev:
            for dest, v in douts(mem, ev, 0x50):
                if dest == 5 and v[0] is not None and v[0] & 0xf == 0 and use_task(v[0]) != use_task(0):
                    pass
            # the EOT USSE program's DOUTU word is ds1[1] of the pixel event program
            data = mem.words(ev, 0x50 // 4)
            w = ds(data, 1, 1)
            if w:
                eot(o, mem, use_task(w))
    if hdr[0x24 // 4]:
        named(o, mem, hdr[0x24 // 4], 0x3c // 4, PBD, "PB descriptor")
    vbase = tail[(0x250 - 0x238) // 4]
    for t, addr, dsize in vdm(o, mem, vbase):
        if t in (4, 6):
            for w in state_program(o, mem, addr, dsize):
                pixel_program(o, mem, w)
        else:
            o.h(f"vertex PDS program {addr:#x}")
            for dest, v in douts(mem, addr, dsize):
                if dest == 3 and v[0]:
                    o.w(f"{addr:#x} doutd", v[0], f"vertex stream {v[0]:#x}: {bits(v[1], 0, 8) + 1} dwords "
                                                  f"to attr {bits(v[1], 8, 14)} [D]")
                elif dest == 5 and v[0] is not None:
                    o.w(f"{addr:#x} doutu", v[0], f"vertex shader {use_task(v[0]):#x} [D]")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--kick", type=int)
    ap.add_argument("--brief", action="store_true", help="one line per word, for diffing two captures")
    a = ap.parse_args()
    log = pvrlog.load(a.capture)
    mm = memmap.build(log)
    o = Out(a.brief)
    ks = cmdta.kicks(log)
    for n, k in enumerate(ks, 1):
        if a.kick and n != a.kick:
            continue
        kick(o, log, mm, k, n)
    print("\n".join(o.lines))


if __name__ == "__main__":
    main()
