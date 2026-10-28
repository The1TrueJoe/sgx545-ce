"""Texture sample (hw 1c), load (hw 1d), store (hw 1e) and the special class
(hw 1f: flow control, MOE setup, sync, emit, ...).

Encoders (Ghidra addresses = libusc + 0x10000; see ISA.md, "smp, ld/st, and the special class"):
  smp        FUN_0010a3f0  USEASM 0x6f-0x7a
  ld / st    FUN_0010b190  USEASM 0x7b-0x92
  eld        FUN_0010ace0  USEASM 0x93, 0x94
  hw 1f      inline in FUN_00119f50 (0x95-0x9a, 0xc1, 0xcd-0xd1, 0xd6, 0xd7,
             0xf1) and FUN_00119710 (0x9b), FUN_00109700 (0x9c-0xa2,
             0x101-0x104), FUN_00108120 (0xbc), FUN_001084b0 (0xbe),
             FUN_00108a50 (0xbd, 0xbf, 0xc0), FUN_0010a1e0 (0xd2, 0xd3),
             FUN_00111ef0 (0xd4), FUN_00109eb0 (0xd5), FUN_00108ff0 (0xd8,
             0xd9), FUN_00119970 (0xfd), FUN_00108d50 (0xfe-0x100)

The special class splits on h20-22 (the encoders' constant high words):
  h22 h21 h20 = 000 flow control (op in h6-8), 001 MOE control (op in
  h24-26), x10 "other" (op in h24-26; h22 is a data bit in limm and emit),
  011 visibility test (op in h24-26), 100 phase / USEASM 0xfe-0x100.
"""
from hw import BANK_DST, BANK_SRC, BANK_SRC0, PRED2, PRED3, Fld, Opd, form, get, mask, put

SP = ("h27-31", 0x1F)


# --------------------------------------------------------------------------
# operand helpers

def src0(ext="h18", kinds=None):
    """src0 slot: number l14-20, bank h2 (pa/sa), extension (o/sa) per class"""
    return Opd("src0", "l14-20", "h2", ext, BANK_SRC0, kinds=kinds)


def src1(name="src1"):
    return Opd(name, "l7-13", "l30-31", "h17", BANK_SRC)


def src2(name="src2"):
    return Opd(name, "l0-6", "l28-29", "h16", BANK_SRC)


def dst(ext="h19", name="dst"):
    return Opd(name, "l21-27", "h0-1", ext, BANK_DST)


class Imm(Opd):
    """an immediate held in plain bits: #n (signed: two's complement)"""

    def __init__(self, role, spec, signed=False):
        super().__init__(role, spec)
        self.signed = signed

    def decode(self, v, d, used):
        n = get(v, self.num)
        used |= mask(self.num)
        if self.signed and n >> (self.nbits() - 1):
            n -= 1 << self.nbits()
        return {"kind": "#", "n": n}

    def encode(self, v, o, d):
        if o["kind"] != "#" or o.get("neg") or o.get("abs") or o.get("sfx"):
            raise ValueError(f"{self.role}: needs a plain #immediate")
        n, w = o["n"], self.nbits()
        lo, hi = (-(1 << (w - 1)), (1 << (w - 1)) - 1) if self.signed else (0, (1 << w) - 1)
        if not lo <= n <= hi:
            raise ValueError(f"{self.role}: #{n} out of range {lo}..{hi}")
        return put(v, self.num, n & ((1 << w) - 1))


SWZ = "xyzw"


class MoeVal(Imm):
    """SMLSI per-operand value: an 8-bit signed increment, or (flag bit set)
    a swizzle of four 2-bit selects, printed #0.xyzw"""

    def __init__(self, role, spec, flag):
        super().__init__(role, spec, signed=True)
        self.flag = flag

    def decode(self, v, d, used):
        used |= mask(self.flag)
        if get(v, self.flag):
            used |= mask(self.num)
            n = get(v, self.num)
            return {"kind": "#", "n": 0, "sfx": ["".join(SWZ[n >> (2 * i) & 3] for i in range(4))]}
        return super().decode(v, d, used)

    def encode(self, v, o, d):
        sfx = o.get("sfx")
        if sfx:
            s = sfx[0]
            if o["kind"] != "#" or o["n"] != 0 or len(sfx) != 1 or len(s) != 4 or set(s) - set(SWZ):
                raise ValueError(f"{self.role}: swizzle is #0.xyzw")
            v = put(v, self.flag, 1)
            return put(v, self.num, sum(SWZ.index(c) << (2 * i) for i, c in enumerate(s)))
        return super().encode(put(v, self.flag, 0), o, d)


class AddrSrc2(Opd):
    """ldr/str address: the src2 slot, whose immediate form has 14 bits (low 7
    in l0-6, high 7 in l14-20)"""

    def __init__(self):
        super().__init__("addr", "l0-6", "l28-29", "h16", BANK_SRC)

    def decode(self, v, d, used):
        o = super().decode(v, d, used)
        used |= mask("l14-20")
        if o["kind"] == "#":
            o["n"] |= get(v, "l14-20") << 7
        elif get(v, "l14-20"):
            o["hi7"] = get(v, "l14-20")       # never written by the encoder
        return o

    def encode(self, v, o, d):
        if o["kind"] == "#":
            n = o["n"]
            if not 0 <= n < 1 << 14:
                raise ValueError("addr: #immediate is 14 bits")
            v = put(v, "l14-20", n >> 7)
            o = dict(o, n=n & 0x7F)
        return super().encode(v, o, d)


class PredDst(Opd):
    """atst8 predicate destination: p<n> in h7-8; h6 clear (USEASM argument
    flag 16, disable write-back) prints as p<n>.nw"""

    def __init__(self):
        super().__init__("pdst", "h7-8")

    def decode(self, v, d, used):
        used |= mask("h6-8")
        o = {"kind": "p", "n": get(v, "h7-8")}
        if not get(v, "h6"):
            o["sfx"] = ["nw"]
        return o

    def encode(self, v, o, d):
        if o["kind"] != "p" or o.get("sfx") not in (None, ["nw"]):
            raise ValueError("pdst: needs p<n> or p<n>.nw")
        v = put(v, "h6", 0 if o.get("sfx") else 1)
        return put(v, "h7-8", o["n"])


def unused_src2():
    # EncodeUnusedSource(src2): bank 2 + extension = #0 (useasm.c's own
    # assert names the helper); kept as one hidden field so a word that holds
    # anything else still shows up as a round-trip failure
    return Fld("nosrc2", "l0-6,l28-29,h16", "hidden", default=0x300)


def unused_src1():
    return Fld("nosrc1", "l7-13,l30-31,h17", "hidden", default=0x300)


# ==========================================================================
# hw 1c: smp (FUN_0010a3f0)
#
# USEASM 0x6f + 3*lod + dim: lod 0 none, 1 bias (0x72-0x74), 2 replace
# (0x75-0x77, texture2DLod), 3 gradients (0x78-0x7a); dim 0/1/2 = 1D/2D/3D
# (h10-11; cube maps sample 3D). Arguments: dest, coordinate, texture state,
# [lod: bias / lod / gradient registers], DRC, int-source-select 0x15.
# --------------------------------------------------------------------------

SMP_LOD = {0: "", 1: "bias", 2: "lod", 3: "grad"}


def _smp_fields():
    return [
        Fld("pred", "h24-26", "pred", PRED3),
        Fld("skipinv", "h23"),
        Fld("nosched", "h22"),
        Fld("syncs", "h20"),
        # h21 + h12-15: the repeat/mask helper. SGX545 (core flag byte9 &
        # 0x40) only accepts mask 1, no repeat: h12 is always set.
        Fld("rcnt", "h21"),
        Fld("rpt", "h12-15", "int", bias=1, default=1, when=lambda d: d.get("rcnt")),
        Fld("mask", "h12-15", "int", default=1, when=lambda d: not d.get("rcnt")),
        Fld("dim", "h10-11", "enum", {0: "1d", 1: "2d", 2: "3d", 3: "dim3"}, show="mod"),
        # USEASM flags3 bit 3 -> h14, bit 2 -> h15 (both: error); 0x8000 (=
        # libusc's "mode 3") is rejected on SGX545
        Fld("ret", "h14-15", "enum", {0: "", 1: "rdata", 2: "rinfo", 3: "ret3"}, show="mod"),
        # USEASM flags3 bit 0 -> h5 (PCF, F32 coordinates), bit 1 -> h6 (PCF,
        # F16 coordinates)
        Fld("pcf", "h5-6", "enum", {0: "", 1: "pcf", 2: "pcf16", 3: "pcf3"}, show="mod"),
        # coordinate format: argument-1 flag 22 -> h3 (F16), flag 24 -> h4 (C10)
        Fld("cfmt", "h3-4", "enum", {0: "", 1: "f16", 2: "c10", 3: "cfmt3"}, show="mod"),
        Fld("drc", "h0", "int"),
    ]


SMP_DST = Opd("dst", "l21-27", "h7", None, {(0, 0): "r", (1, 0): "pa"})
SMP_COORD = Opd("coord", "l14-20", "h2", "h18", BANK_SRC0)

for lod, name in SMP_LOD.items():
    if lod == 0:
        ops = [SMP_DST, SMP_COORD, src1("state")]
        flds = _smp_fields() + [unused_src2()]
    else:
        ops = [SMP_DST, SMP_COORD, src1("state"), src2(name)]
        flds = _smp_fields()
    form("smp", [("h27-31", 0x1C), ("h8-9", lod)], flds, ops, src="FUN_0010a3f0 smp",
         mn=(lambda d, n=name: [n] if n else []))

# ==========================================================================
# hw 1d / 1e: ld, st (FUN_0010b190), eld (FUN_0010ace0)
#
# USEASM 0x7b + i: i/4 even = load, odd = store; i/8 = address mode (h10-11:
# 0 "a", 1 "l", 2 "t"); i%4 = size (h4-5: 0x7b/0x7f.. -> 2, +1 -> 1, +2 -> 0,
# +3 rejected). Load arguments: dest, base, offset, [range], DRC; store:
# base, offset, data.
# --------------------------------------------------------------------------

LD_AM = {0: "a", 1: "l", 2: "t", 3: "am3"}
LD_SIZE = {0: "d", 1: "w", 2: "b", 3: "q"}


def _ldst_fields(store):
    f = [Fld("pred", "h24-26", "pred", PRED3),
         Fld("skipinv", "h23")]
    if store:
        f.append(Fld("nosched", "h22"))
    f += [
        # set unless USEASM flags1 bit 31; the repeat steps the address by the
        # MOE increments (same bit as the ALU classes' MOE mode)
        Fld("moe", "h21"),
        Fld("syncs", "h20"),
        Fld("f2_3", "h19"),
        Fld("rpt", "h12-15", "int", bias=1, default=1),
        Fld("am", "h10-11", "enum", LD_AM, show="mod"),
        Fld("size", "h4-5", "enum", LD_SIZE, show="mod"),
        # USEASM flags1 bit 27 -> h8, bit 28 -> h9 (not both, not with "t")
        Fld("preinc", "h8"),
        Fld("postinc", "h9"),
        # USEASM flags1 bit 29: a load then has a 4th source in src2
        Fld("range", "h6"),
        Fld("f2_9", "h3"),
        Fld("f2_2", "h1"),
    ]
    if store:
        f.append(Fld("f3_13", "h0"))
    else:
        f.append(Fld("drc", "h0", "int"))
        f.append(Fld("nosrc2", "l0-6,l28-29,h16", "hidden", default=0x300, when=lambda d: not d.get("range")))
    return f


LD_RANGE = Opd("range", "l0-6", "l28-29", "h16", BANK_SRC)
LD_RANGE.when = lambda d: d.get("range")

form("ld", [("h27-31", 0x1D), ("h22", 0)], _ldst_fields(False),
     [Opd("dst", "l21-27", "h7", None, {(0, 0): "r", (1, 0): "pa"}), src0(), src1("off"), LD_RANGE],
     src="FUN_0010b190 ld")

form("st", [("h27-31", 0x1E), ("h7", 0), ("l21-27", 0)], _ldst_fields(True),
     [src0(), src1("off"), src2("data")], src="FUN_0010b190 st")

# eld: USEASM 0x93 (h3 = 0) / 0x94 (h3 = 1). Arguments: dest, src1, src0,
# an 11-bit immediate (h5-11 low 7 bits, h16-19 high 4: so no source has an
# extension bit), src2, DRC. Repeat needs USEASM flags1 bit 31.
form("eld", [("h27-31", 0x1D), ("h22", 1)],
     [Fld("pred", "h24-26", "pred", PRED3), Fld("skipinv", "h23"), Fld("syncs", "h20"),
      Fld("f2_3", "h21"), Fld("rpt", "h12-15", "int", bias=1, default=1),
      Fld("size", "h3", "enum", {0: "d", 1: "q"}, show="mod"),
      Fld("f2_2", "h1"), Fld("drc", "h0", "int")],
     [Opd("dst", "l21-27", "h4", None, {(0, 0): "r", (1, 0): "pa"}),
      Opd("src1", "l7-13", "l30-31", None, BANK_SRC),
      Opd("src0", "l14-20", "h2", None, BANK_SRC0),
      Imm("imm", "h5-11,h16-19"),
      Opd("src2", "l0-6", "l28-29", None, BANK_SRC)],
     src="FUN_0010ace0 eld")

# ==========================================================================
# hw 1f, h22-20 = 000: flow control. op h6-8: 0 ba, 1 br, 2 lapc, 5 nop.
# --------------------------------------------------------------------------

FC = [SP, ("h20-22", 0)]


def _fc_sync():
    return [Fld("pred", "h24-26", "pred", PRED3),
            # USEASM flags1 bit 18 (libusc asm.c: the branch's sync-end)
            Fld("syncend", "h23"),
            # USEASM flags3 bit 4 (asm.c: SGX_FEATURE_FLAGS_USE_BRANCH_EXTSYNCEND);
            # the encoder sets h23 with it
            Fld("xsyncend", "h12"),
            Fld("nosched", "h11")]


# ba: absolute target = label + the program's load offset (encode()'s
# `offset`); br: target - this instruction's index, signed. 18 bits (core
# feature word +0x20 = 18). h9 = USEASM flags1 bit 19 (asm.c ICALL: save the
# link address, i.e. a call); h10 = flags1 bit 22.
form("ba", FC + [("h6-8", 0), ("h19", 0)], _fc_sync() + [Fld("link", "h9"), Fld("f1_22", "h10")],
     [Imm("target", "l0-17")], src="FUN_00119f50 0x95")
form("br", FC + [("h6-8", 1), ("h19", 0)], _fc_sync() + [Fld("link", "h9"), Fld("f1_22", "h10")],
     [Imm("target", "l0-17", signed=True)], src="FUN_00119f50 0x96")
# lapc: jump to the link address (asm.c: ILAPC = return)
form("lapc", FC + [("h6-8", 2)], _fc_sync(), [], src="FUN_00119f50 0x97")
# USEASM 0x98: needs core feature word0 & 0x200000; no arguments
form("op0x98", FC + [("h6-8", 0), ("h9", 1), ("h19", 1)], [Fld("nosched", "h11")], [],
     src="FUN_00119f50 0x98")
# nop (USEASM 0xf1; libusc's INOP and IDRVPADDING). l0 = USEASM flags2 bit
# 26, l1 = flags3 bit 5 (core feature word0 & 0x100000)
form("nop", FC + [("h6-8", 5)],
     [Fld("syncs", "h23"), Fld("end", "h18"), Fld("nosched", "h11"),
      Fld("f2_26", "l0"), Fld("f3_5", "l1")], [], src="FUN_00119f50 0xf1")

# ==========================================================================
# hw 1f, h22-20 = 001: MOE control. op h24-26. h18 = nosched. h19 set: the
# values come from two registers (src1, src2) instead of immediates (core
# feature word0 & 4).
# --------------------------------------------------------------------------

MC = [SP, ("h20-22", 1)]
MC_F = [Fld("nosched", "h18")]

# smoa (op 0, USEASM 0xbc): four signed 10-bit values (dest, src0, src1,
# src2 order) and four 2-bit modes (USEASM register type 0xe arguments)
form("smoa", MC + [("h24-26", 0), ("h19", 0)],
     MC_F + [Fld("am0", "h14-15", "int"), Fld("am1", "h12-13", "int"),
             Fld("am2", "h10-11", "int"), Fld("am3", "h8-9", "int")],
     [Imm("v0", "l30-31,h0-7", signed=True), Imm("v1", "l20-29", signed=True),
      Imm("v2", "l10-19", signed=True), Imm("v3", "l0-9", signed=True)], src="FUN_00108120 0xbc")

# smlsi (op 2, USEASM 0xbe; libusc ISMLSI): per operand an 8-bit increment
# or, with its flag (h3 dest, h2 src0, h1 src1, h0 src2), a swizzle; then
# three register limits in units of 4 (h12-15, h8-11, h4-7)
form("smlsi", MC + [("h24-26", 2), ("h19", 0)],
     MC_F + [Fld("lim0", "h12-15", "int"), Fld("lim1", "h8-11", "int"), Fld("lim2", "h4-7", "int")],
     [MoeVal("v0", "l24-31", "h3"), MoeVal("v1", "l16-23", "h2"),
      MoeVal("v2", "l8-15", "h1"), MoeVal("v3", "l0-7", "h0")], src="FUN_001084b0 0xbe")

# smr (op 1, USEASM 0xbd), smbo (op 3, 0xbf: MOE base offsets), imo (op 4,
# 0xc0: signed, the encoder takes -32..31): four 12-bit values
for op, name, signed, uop in ((1, "smr", False, 0xbd), (3, "smbo", False, 0xbf), (4, "imo", True, 0xc0)):
    form(name, MC + [("h24-26", op), ("h19", 0)], list(MC_F),
         [Imm("v0", "h4-15", signed), Imm("v1", "l24-31,h0-3", signed),
          Imm("v2", "l12-23", signed), Imm("v3", "l0-11", signed)], src=f"FUN_00108a50 {uop:#x}")

for op, name, uop in ((0, "smoa", 0xbc), (1, "smr", 0xbd), (2, "smlsi", 0xbe), (3, "smbo", 0xbf)):
    form(name, MC + [("h24-26", op), ("h19", 1)], list(MC_F), [src1(), src2()],
         src=f"{uop:#x} register form", mn=lambda d: ["r"])

# setfc (op 5, USEASM 0xc1; libusc ISETFC): two one-bit arguments
form("setfc", MC + [("h24-26", 5), ("h19", 0)], list(MC_F),
     [Imm("a0", "l0"), Imm("a1", "l8")], src="FUN_00119f50 0xc1")

# op 6, USEASM 0xfd (core feature word0 & 0x800000): dest + immediate 0..8
form("op0xfd", MC + [("h24-26", 6)], MC_F + [Fld("skipinv", "h23")],
     [dst(), Imm("n", "h11-14")], src="FUN_00119970 0xfd")

# ==========================================================================
# hw 1f, h21-20 = 10: "other". op h24-26.
# --------------------------------------------------------------------------

OT = [SP, ("h20-21", 2)]

# idf (op 0, USEASM 0x99): DRC h0, second argument #0/#1 -> h14
form("idf", OT + [("h22", 0), ("h24-26", 0)],
     [Fld("drc", "h0", "int"), Fld("path", "h14", "int"), Fld("nosched", "h11")], [],
     src="FUN_00119f50 0x99")
# wdf (op 1, USEASM 0x9a): wait for DRC h0
form("wdf", OT + [("h22", 0), ("h24-26", 1)], [Fld("drc", "h0", "int"), Fld("nosched", "h11")], [],
     src="FUN_00119f50 0x9a")
# op 2, USEASM 0xd7: immediate below core feature +0x10 (16)
form("op0xd7", OT + [("h22", 0), ("h24-26", 2)], [Fld("nosched", "h11")], [Imm("n", "l0-3")],
     src="FUN_00119f50 0xd7")

# emit (op 3, FUN_00109700). h14-15 target: USEASM 0x9c/0x9d -> 0,
# 0x9f-0xa1/0x101/0x102 -> 1, 0xa2 -> 2, 0x103/0x104 -> 3; h12 set by
# 0xa0/0x101/0x103, h13 by 0xa1 (old cores) or 0x101 + flags3 bit 8; h10 =
# flags3 bit 6 (0x101/0x103) or bit 7 (0x102/0x104); l21 = flags3 bit 20.
# n = first argument (h0-1, h9: 0..5 on SGX545). The sources not used by an
# opcode hold the unused-source pattern (#0). 0x9d/0x9e/0xa1/0xa2 add a 14-bit
# immediate (bits 0-5 l22-27, 6-11 h3-8, 12-13 h22-23); 0x9c has none but
# sets h8, i.e. immediate 0x800.
form("emit", OT + [("h24-26", 3)],
     [Fld("end", "h18"), Fld("nosched", "h11"),
      Fld("tgt", "h14-15", "enum", {0: "t0", 1: "t1", 2: "t2", 3: "t3"}, show="mod"),
      Fld("f12", "h12"), Fld("f13", "h13"), Fld("f10", "h10"), Fld("f3_20", "l21"),
      Fld("n", "h0-1,h9", "int"), Fld("imm", "l22-27,h3-8,h22-23", "int")],
     [src0("h19"), src1(), src2()], src="FUN_00109700 emit")

# limm (op 4, USEASM 0x9b): 32-bit immediate (l0-20, h4-8, h12-17; a label
# gives its absolute address; argument flag 11 inverts it), predicate in
# h9-11
form("limm", OT + [("h24-26", 4)],
     [Fld("pred", "h9-11", "pred", PRED3), Fld("skipinv", "h23"), Fld("nosched", "h22"),
      Fld("end", "h18")],
     [dst(), Imm("imm", "l0-20,h4-8,h12-17")], src="FUN_00119710 0x9b")

# op 5: USEASM 0xcd (l0 = 0) / 0xce (l0 = 1), immediate below core feature
# +0xc (8) in l4-6
for rel, name, uop in ((0, "lock", 0xcd), (1, "release", 0xce)):
    form(name, OT + [("h22", 0), ("h24-26", 5), ("l0", rel)], [Fld("nosched", "h11")],
         [Imm("n", "l4-6")], src=f"FUN_00119f50 {uop:#x}")

# op 6: USEASM 0xd0 (h19 = 0: dest, address, DRC) / 0xd1 (h19 = 1:
# address, data)
form("ldr", OT + [("h22", 0), ("h24-26", 6), ("h19", 0)],
     [Fld("skipinv", "h23"), Fld("nosched", "h11"), Fld("drc", "h0", "int")],
     [Opd("dst", "l21-27", "h7", None, {(0, 0): "r", (1, 0): "pa"}), AddrSrc2()], src="FUN_00119f50 0xd0")
form("str", OT + [("h22", 0), ("h24-26", 6), ("h19", 1), ("l21-27", 0), ("h7", 0)],
     [Fld("skipinv", "h23"), Fld("nosched", "h11")],
     [AddrSrc2(), src1("data")], src="FUN_00119f50 0xd1")

# op 7, USEASM 0xd6: immediate 0..5 in l0-2, l3 = flags3 bit 6
form("op0xd6", OT + [("h22", 0), ("h24-26", 7)], [Fld("nosched", "h11"), Fld("f3_6", "l3")],
     [Imm("n", "l0-2")], src="FUN_00119f50 0xd6")

# ==========================================================================
# hw 1f, h21-20 = 11: visibility test (h22 is data in atst8). op h24-26. Sources: src0 (extension
# h19), src1, src2. The int-source-select arguments: 0x34 -> h5; 0x36 ->
# h3, 0x37 -> h4 (0x15 / 0x35 = none).
# --------------------------------------------------------------------------

VT = [SP, ("h20-21", 3)]

# op 0: USEASM 0xd2 (three sources) / 0xd3 (h15, none)
form("pcoeff", VT + [("h22", 0), ("h24-26", 0), ("h15", 0)], [Fld("end", "h18"), Fld("nosched", "h11")],
     [src0("h19"), src1(), src2()], src="FUN_0010a1e0 0xd2")
form("ptoff", VT + [("h22", 0), ("h24-26", 0), ("h15", 1)], [Fld("end", "h18"), Fld("nosched", "h11")], [],
     src="FUN_0010a1e0 0xd3")
# op 1, USEASM 0xd4: dest (no extension: r/o/pa), predicate dest, 3 sources
form("atst8", VT + [("h24-26", 1)],
     [Fld("syncs", "h23"), Fld("f2_25", "h22"), Fld("end", "h18"), Fld("nosched", "h11"),
      Fld("sel34", "h5"), Fld("sel36", "h3"), Fld("sel37", "h4")],
     [Opd("dst", "l21-27", "h0-1", None, {(0, 0): "r", (1, 0): "o", (2, 0): "pa", (3, 0): "ix"}),
      PredDst(), src0("h19"), src1(), src2()], src="FUN_00111ef0 0xd4")
# op 3, USEASM 0xd5: 2-bit predicate in h9-10
form("depthf", VT + [("h24-26", 3), ("h22", 0)],
     [Fld("pred", "h9-10", "pred", PRED2), Fld("syncs", "h23"), Fld("end", "h18"),
      Fld("nosched", "h11"), Fld("sel34", "h5"), Fld("sel37", "h4")],
     [src0("h19"), src1(), src2()], src="FUN_00109eb0 0xd5")

# ==========================================================================
# hw 1f, h22-20 = 100 (core feature word0 & 0x1000 / & 0x1000000).
# --------------------------------------------------------------------------

P4 = [SP, ("h20-22", 4), ("h23", 0)]

# phas (op 2): USEASM 0xd8 (h18 = 0: two register sources) / 0xd9 (h18 = 1:
# 18-bit absolute address l0-17, temporaries / 4 in h0-7, int-source-select
# arguments 0x3c/0x3e/0x3d -> h11-12 = 0/1/2, 0x3f/0x40/0x41 -> h8-10 =
# 1/2/7 (none: 0), 0x42/0x43 -> h13). h19 = end.
form("phas", P4 + [("h24-26", 2), ("h18", 0)], [Fld("end", "h19"), Fld("nosched", "h11")],
     [src1(), src2()], src="FUN_00108ff0 0xd8")
form("phas", P4 + [("h24-26", 2), ("h18", 1)],
     [Fld("end", "h19"), Fld("rate", "h11-12", "int"), Fld("wait", "h8-10", "int"),
      Fld("sel43", "h13")],
     [Imm("pc", "l0-17"), Imm("temps4", "h0-7")], src="FUN_00108ff0 0xd9", mn=lambda d: ["i"])

# op 0, USEASM 0xff/0xfe/0x100 = h12-13 1/2/3: src1, src2, immediate 0..3 in
# h8-9; h15 = flags3 bit 11, h14 = flags3 bit 12
for sub, uop in ((1, 0xff), (2, 0xfe), (3, 0x100)):
    form(f"op{uop:#x}", P4 + [("h24-26", 0), ("h12-13", sub)],
         [Fld("nosched", "h11"), Fld("f3_11", "h15"), Fld("f3_12", "h14")],
         [src1(), src2(), Imm("n", "h8-9")], src=f"FUN_00108d50 {uop:#x}")
