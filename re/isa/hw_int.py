"""Integer, fixed-point and multi-unit forms: hardware opcodes 07 (efo),
10 (sop2), 11 (sop3, lrp1, ...), 12 (sopwm), 13 (ima8), 14 (ima16), 15 (imae),
16 (adif, adifsum, bilin, ssum16, ...), 17 (firv, firh), 18 (dot3/dot4),
19 (fpma), 1a (ima32, ...). See notes_int.md for the field tables.

Encoders (Ghidra addresses = libusc file vaddr + 0x10000):
  FUN_00116100  USEASM 0x13 efo                         hw 07
  FUN_0010bea0  USEASM 0x57 sop2 (+ coissued 0xe3)       hw 10
  FUN_001178e0  USEASM 0x59 sop3 (+ 0xde/0xdf/0xe0)      hw 11, h21 = 0
  FUN_0010db30  USEASM 0xe1 lrp1 (+ 0xde)                hw 11, h21 = 1, h10 = 0
  FUN_0010d120  USEASM 0xe2 (+ 0xe0)                     hw 11, h21 = 1, h10 = 1
  FUN_0010c9e0  USEASM 0x58 sopwm (0xfa rewritten)       hw 12
  FUN_0010f1a0  USEASM 0x62 ima8 / 0xf4 fpma             hw 13 / 19
  FUN_0010f960  USEASM 0x63 ima16 (0xe4-0xe9 rewritten)  hw 14
  FUN_00110060  USEASM 0x64 imae (0xea/0xf0 rewritten)   hw 15
  FUN_0010e460  USEASM 0x5a-0x61 dot3/dot4               hw 18
  FUN_00110800  USEASM 0x67 adif / 0x68 adifsum      hw 16 sub-op 0
  FUN_00118bf0  USEASM 0x6e                          hw 16 sub-op 1  (needs word0 & 2: not on SGX545)
  FUN_001111f0  USEASM 0x6b bilin                    hw 16 sub-op 2
  FUN_00110be0  USEASM 0x69 ssum16                   hw 16 sub-op 3
  FUN_001192a0  USEASM 0x6a ssum16, src0 = internal  hw 16 sub-op 3, h8-9 = 3 (not on SGX545)
  FUN_00111850  USEASM 0x6c firv                     hw 17, h2 = 0
  UseAssembleInstruction (FUN_00119f50) inline: USEASM 0x6d firh  hw 17, h2 = 1 (not on SGX545)
  FUN_00118650  USEASM 0x65 ima32 (and 0x167/0x168, rewritten to 0x65 as a - b)  hw 1a, h20 = 0
  FUN_00119f50 inline: USEASM 0x66                   hw 1a, h20 = 1 (not on SGX545)
Mnemonic sources: efo, sop2/sop3/sopwm, lrp1, alrp/asop, ima8/fpma/ima16/imae/ima32
and dot3/dot4 are libusc's own names (asserts/struct names in usc.c); the
others are Eurasia-family guesses (notes_int.md).
"""
from hw import BANK_DST, BANK_SRC, BANK_SRC0, PRED2, Fld, Opd, form, get, mask, put

# --- shared pieces --------------------------------------------------------


def common(rpt="h12-14"):
    """pred: FUN_00106f30(flags1, 1) << 25 (2-bit short form); skipinv: flags1
    b0; nosched: flags1 b3 << 19; end: flags1 b17 << 1; repeat count - 1."""
    f = [Fld("pred", "h25-26", "pred", PRED2),
         Fld("skipinv", "h23"),
         Fld("nosched", "h22"),
         Fld("end", "h18")]
    if rpt:
        f.append(Fld("rpt", rpt, "int", bias=1, default=1))
    return f


class SOpd(Opd):
    """an operand whose immediate is a 7-bit two's complement number when
    signed(d) (the encoders pass that as the 'signed immediate' argument of
    the generic operand encoder FUN_00107830)"""

    def __init__(self, *a, signed=None, **k):
        super().__init__(*a, **k)
        self.signed = signed

    def decode(self, v, d, used):
        o = super().decode(v, d, used)
        if o["kind"] == "#" and self.signed and self.signed(d) and o["n"] >= 64:
            o["n"] -= 128
        return o

    def encode(self, v, o, d):
        if o["kind"] == "#" and self.signed and self.signed(d) and o["n"] < 0:
            o = dict(o, n=o["n"] + 128)
        return super().encode(v, o, d)


class SameBank(Opd):
    """src0 with no bank bits of its own: it is read from src1's bank (the
    encoders require USEASM arg1 and arg2 to have the same register type,
    one of r/o/pa/sa, and no index)."""

    KINDS = ["r", "o", "pa", "sa"]

    def __init__(self):
        super().__init__("src0", "l14-20")

    def decode(self, v, d, used):
        used |= mask("l14-20")
        return {"kind": self.KINDS[get(v, "l30-31")], "n": get(v, "l14-20")}

    def encode(self, v, o, d):
        if o["kind"] not in self.KINDS:
            raise ValueError(f"src0: {o['kind']} not encodable here (r/o/pa/sa, same bank as src1)")
        if o["n"] >> 7:
            raise ValueError("src0: number too big")
        return put(v, "l14-20", o["n"])


BANK4 = {(0, 0): "r", (1, 0): "o", (2, 0): "pa", (3, 0): "sa"}


def DST():
    return Opd("dst", "l21-27", "h0-1", "h19", BANK_DST)


def SRC0(ext=None):
    # FUN_00109550: r/pa via h2 (+ o/sa when the encoder allows the ext bit);
    # an internal register becomes r124+n
    return Opd("src0", "l14-20", "h2", ext, BANK_SRC0 if ext else {(0, 0): "r", (1, 0): "pa"})


def SRC1(signed=None, table=BANK_SRC):
    return SOpd("src1", "l7-13", "l30-31", "h17" if table is BANK_SRC else None, table, signed=signed)


def SRC2(signed=None, neg=None):
    return SOpd("src2", "l0-6", "l28-29", "h16", BANK_SRC, neg=neg, signed=signed)


# INTSRCSEL source formats (USEASM 0x1e/0x1f/0x20; names guessed U8/S8/O8);
# S8 and O8 make the immediates signed
FMT8 = {0: "", 1: "s8", 2: "o8"}


def _sgn(d):
    return d.get("sgn", 0) == 1


def _sfmt(d):
    return d.get("fmt", 0) != 0


def sel(table, comp=True):
    """a select field whose spec ends with its complement bit: value = sel |
    comp << n; complemented entries print as 1-x (1-zero = one)"""
    n = max(table) + 1
    w = (n - 1).bit_length()
    out = {}
    for k in range(1 << w):
        out[k] = table.get(k, f"x{k}")
        if comp:
            c = out[k]
            out[k | 1 << w] = "one" if c == "zero" else ("zero" if c == "one" else "1-" + c)
    return out


OP2 = {0: "add", 1: "sub"}
OP4 = {0: "add", 1: "sub", 2: "min", 3: "max"}


def NSRC0(neg=None):
    """FUN_00109550 with the extension bit disallowed: r/pa only (an internal
    register becomes r124+n)"""
    return Opd("src0", "l14-20", "h2", None, {(0, 0): "r", (1, 0): "pa"}, neg=neg)


# --- hw 07: efo (USEASM 0x13, FUN_00116100) ----------------------------------
# The encoder ORs USEASM argument 4 (an immediate built by usc2/asm.c from
# the psEfo routing) straight into the high word; the field meanings below
# are asm.c's (usc.c lines ~1926-2400, SGX545 = the bNewEfoFeature branch).
# Units: M0 M1 multipliers, A0 A1 adders, I0 I1 internal registers.
EFO_M = {0: "m0=s0*s2,m1=s1*s2", 1: "m0=s2*s1,m1=s2*s2", 2: "m0=s1*s0,m1=s2*s2",
         3: "m0=s1*i0,m1=s2*i1"}
EFO_A = {0: "a0=m0+m1,a1=i1+i0", 1: "a0=m0+s0,a1=i1+i0", 2: "a0=m0+i0,a1=i1+m1",
         3: "a0=s0+s2,a1=s1+s2"}
EFO_I = {0: "i0=a0,i1=a1", 1: "i0=a1,i1=a0", 2: "i0=m0,i1=m1", 3: "i0=a0,i1=m1"}
EFO_D = {0: "i0", 1: "i1", 2: "a0", 3: "a1"}
form("efo", [("h27-31", 0x07)],
     [Fld("pred", "h24-25", "pred", PRED2), Fld("skipinv", "h23"), Fld("nosched", "h11"),
      Fld("rpt", "h12-13", "int", bias=1, default=1),
      Fld("d", "h20-21", "enum", EFO_D),
      Fld("wi0", "h10"), Fld("wi1", "h9"),
      Fld("i", "h18-19", "enum", EFO_I), Fld("a", "h16-17", "enum", EFO_A),
      Fld("m", "h14-15", "enum", EFO_M),
      Fld("a0rneg", "h26"), Fld("a0lneg", "h22"), Fld("f3_10", "h8")],
     [Opd("dst", "l21-27", "h0-1", None, {(0, 0): "r", (1, 0): "o", (2, 0): "pa", (3, 0): "ix"}),
      Opd("src0", "l14-20", "h2", "h6", BANK_SRC0, neg="h5"),
      Opd("src1", "l7-13", "l30-31", None, BANK4, neg="h4"),
      Opd("src2", "l0-6", "l28-29", "h7", BANK_SRC, neg="h3")],
     src="FUN_00116100 hw07")

# --- hw 10: sop2 (USEASM 0x57 + coissued 0xe3, FUN_0010bea0) -----------------
# dst.rgb = (src1 * c1) cop (src2 * c2); dst.a = (src1.a * a1) aop (src2.a * a2)
# (Eurasia SOP2). INTSRCSEL: 0 zero, 1 one, 2 asat, 5 src1, 6 src2, 8 src1a,
# 9 src2a, 0x18/0x19 unknown (k18/k19); 0x1a = complement (cs1c/as1c),
# 0x14 = negate (aneg), 0x12-0x17 = add sub - - min max.
CS1 = {0: "zero", 1: "s1", 2: "s2", 3: "s1a", 4: "s2a", 5: "asat", 6: "k18"}
CS2 = {0: "zero", 1: "s1", 2: "s2", 3: "s1a", 4: "s2a", 5: "asat", 6: "k19"}
form("sop2", [("h27-31", 0x10)],
     common() + [
         Fld("c1", "h6-8,h24", "enum", sel(CS1)), Fld("c2", "h3-5,h15", "enum", sel(CS2)),
         Fld("cop", "l18-19", "enum", OP4), Fld("cs1c", "l20"),
         Fld("a1", "h20-21,h11", "enum", sel({0: "zero", 1: "s1a", 2: "s2a", 3: "k18"})),
         Fld("a2", "h9-10,h2", "enum", sel({0: "zero", 1: "s1a", 2: "s2a", 3: "k19"})),
         Fld("aop", "l16-17", "enum", OP4), Fld("as1c", "l15"), Fld("aneg", "l14")],
     [DST(), SRC1(), SRC2()], src="FUN_0010bea0 hw10")

# --- hw 12: sopwm (USEASM 0x58; 0xfa = 0x58 with s1a, 1-s1a, add, add;
# FUN_0010c9e0). Write mask from USEASM dest flags b7-10 (flag b20 = has mask),
# stored rotated: x h12, y h13, z h14, w h11.
WMSEL = {0: "zero", 1: "asat", 2: "s1", 3: "s1a", 6: "s2", 7: "s2a"}
form("sopwm", [("h27-31", 0x12)],
     common(rpt=None) + [
         Fld("mask", "h12,h13,h14,h11", "int", default=15),
         Fld("c1", "h6-8,h24", "enum", sel(WMSEL)), Fld("c2", "h3-5,h15", "enum", sel(WMSEL)),
         Fld("cop", "h20-21", "enum", OP4), Fld("aop", "h9-10", "enum", OP4)],
     [DST(), SRC1(), SRC2()], src="FUN_0010c9e0 hw12")

# --- hw 11: sop3 family (FUN_001178e0 0x59, FUN_0010db30 0xe1, FUN_0010d120
# 0xe2). h21 picks the colour op, h10 the coissued alpha op. Selects: 0 zero,
# 1 asat, 2 s1, 3 s1a, 4 s0, 5 s0a, 6 s2, 7 s2a; h24/h15 complement c1/c2.
SEL3 = {0: "zero", 1: "asat", 2: "s1", 3: "s1a", 4: "s0", 5: "s0a", 6: "s2", 7: "s2a"}
OPS3 = [DST(), NSRC0(), SRC1(), SRC2()]
C3 = [Fld("c1", "h6-8,h24", "enum", sel(SEL3)), Fld("c2", "h3-5,h15", "enum", sel(SEL3))]
ALRP = [Fld("a1", "h13,h9", "enum", sel({0: "zero", 1: "s1a"})),
        Fld("a2", "h12,h14", "enum", sel({0: "zero", 1: "s2a"}))]
# 0x59 + 0xde (asop) / 0xdf (arsop: h11, alpha sources swapped)
form("sop3", [("h27-31", 0x11), ("h21", 0), ("h10", 0)],
     common(rpt=None) + [Fld("cop", "h20", "enum", OP2)] + C3 + [
         Fld("a", "h12-13,h14", "enum", sel({0: "zero", 1: "s0a", 2: "s1a", 3: "s2a"})),
         Fld("aop", "h9", "enum", OP2), Fld("arev", "h11")],
     OPS3, src="FUN_001178e0 hw11")
# 0x59 + 0xe0 (alrp); h11 = colour result negate (USEASM arg7 0x14)
form("sop3l", [("h27-31", 0x11), ("h21", 0), ("h10", 1)],
     common(rpt=None) + [Fld("cop", "h20", "enum", OP2)] + C3 + ALRP + [Fld("cneg", "h11")],
     OPS3, src="FUN_001178e0 hw11")
# 0xe1 lrp1 + 0xde: colour = lerp of l1/l2 by cs (src0 or src0 alpha); the
# coissued asop's two selects use the c1/c2 fields
form("lrp1", [("h27-31", 0x11), ("h21", 1), ("h10", 0)],
     common(rpt=None) + [
         Fld("l1", "h13,h20", "enum", sel({0: "zero", 1: "s1"})),
         Fld("l2", "h12,h14", "enum", sel({0: "zero", 1: "s2"})),
         Fld("cs", "h11", "enum", {0: "s0", 1: "s0a"})] + C3 + [Fld("aop", "h9", "enum", OP2)],
     OPS3, src="FUN_0010db30 hw11")
# 0xe2 + 0xe0 (alrp): arg6/arg7 pair in h20/h11
form("sop3x", [("h27-31", 0x11), ("h21", 1), ("h10", 1)],
     common(rpt=None) + [
         Fld("p", "h11,h20", "enum", {0: "s1.s2", 1: "s1.1-s2", 2: "one.s2", 3: "1-s1.s2"})] + C3 + ALRP,
     OPS3, src="FUN_0010d120 hw11")

# --- hw 18: dot3/dot4 (USEASM 0x5a-0x61, FUN_0010e460) ------------------------
# h24 = vector length 4 (0x5b 0x5d 0x5f 0x61); h6 set by 0x5e-0x61; 0x5c 0x5d
# 0x60 0x61 put 3 in h20-21. Without a coissue h9-10 = 3. Coissue 0xdc/0xdd
# (h5 = 0, h4 = 0xdd) or 0xda/0xdb (h5 = 1, h4 = 0xdb; src0 at l14-20).
for vl in (3, 4):
    for alt in (0, 1):
        fl = common() + [
            Fld("f6", "h6"),
            Fld("cs", "h7-8", "enum", {0: "", 1: "cx2", 2: "cx4", 3: "cx8"}),
            Fld("s1c", "h11"), Fld("s2c", "h15"), Fld("f4", "h4")]
        if alt:
            fl += [Fld("a1", "h20-21", "enum", {0: "zero", 1: "s1a", 2: "s2a", 3: "x3"}),
                   Fld("a2", "h9-10,h3", "enum", sel({0: "zero", 1: "s1a", 2: "s2a", 3: "x3"}))]
            ops = [DST(), NSRC0(), SRC1(), SRC2()]
        else:
            fl += [Fld("as", "l16-17", "enum", {0: "", 1: "ax2", 2: "ax4", 3: "ax8"}),
                   Fld("a1", "h20-21,h2", "enum", sel({0: "zero", 1: "s1a", 2: "s2a", 3: "x3"})),
                   Fld("a2", "h9-10,h3", "enum", sel({0: "zero", 1: "s1a", 2: "s2a", 3: "x3"})),
                   Fld("s1ac", "l15")]
            ops = [DST(), SRC1(), SRC2()]
        form(f"dot{vl}" + ("s" if alt else ""), [("h27-31", 0x18), ("h24", vl - 3), ("h5", alt)], fl, ops,
             src="FUN_0010e460 hw18")

# --- hw 13 ima8 (USEASM 0x62, and 0xeb/0xec/0xf5/0xf6 rewritten into it) and
# hw 19 fpma (0xf4); FUN_0010f1a0. dst = c0 * c1 + c2 per channel, alpha
# a0 * a1 + a2 (s1a, s2a fixed, complements only). h11 = USEASM flags2 b10.
IMA8 = [Fld("sat", "h11"),
        Fld("c0", "h20-21,h10", "enum", sel({0: "s0", 1: "s1", 2: "s0a", 3: "s1a"})),
        Fld("c1", "h5,h24", "enum", sel({0: "s1", 1: "s1a"})),
        Fld("c2", "h4,h15", "enum", sel({0: "s2", 1: "s2a"})),
        Fld("a0", "h9,h6", "enum", sel({0: "s0a", 1: "s1a"})),
        Fld("a1c", "h7"), Fld("a2c", "h8")]
for op, name in ((0x13, "ima8"), (0x19, "fpma")):
    form(name, [("h27-31", op)], common() + IMA8, [DST(), NSRC0(neg="h3"), SRC1(), SRC2()],
         src=f"FUN_0010f1a0 hw{op:02x}")

# --- hw 14: ima16 (USEASM 0x63; 0xe4/0xe5/0xe7/0xe8 add/sub and 0xe6/0xe9 mul
# rewritten into it), FUN_0010f960. dst = src0 * src1 + src2, 16-bit.
F16 = {0: "", 1: "u8", 2: "s8", 3: "o8"}
form("ima16", [("h27-31", 0x14)],
     common() + [Fld("sgn", "h11", show="mod"), Fld("sat", "h10"),
                 Fld("f2_15", "h3-4", "int"), Fld("dflag1", "h24"),
                 Fld("f1", "h6-7", "enum", F16), Fld("f2", "h8-9", "enum", F16),
                 Fld("s1hi", "h20"), Fld("s2hi", "h5")],
     [DST(), NSRC0(), SRC1(lambda d: d.get("f1") != 1), SRC2(lambda d: d.get("f2") != 1, neg="h21")],
     src="FUN_0010f960 hw14")

# --- hw 15: imae (USEASM 0x64; 0xea/0xf0 rewritten into it), FUN_00110060.
# dst = src0 * src1 + src2 (16x16 + 32), carry via an internal register.
form("imae", [("h27-31", 0x15)],
     common() + [Fld("sgn", "h11", show="mod"), Fld("sat", "h10"),
                 Fld("f2_15", "h3-5", "int"),
                 Fld("t2", "h6-7", "enum", {0: "", 1: "s16", 2: "u32", 3: "x3"}),
                 Fld("s0hi", "h24"), Fld("s1hi", "h21"), Fld("s2hi", "h20"),
                 Fld("cout", "h8"), Fld("cin", "h9"), Fld("ci", "h15", "int")],
     [DST(), NSRC0(), SRC1(_sgn), SRC2(lambda d: d.get("t2", 0) != 0)],
     src="FUN_00110060 hw15")


# --- hw 16 sub-op 0: adif / adifsum (FUN_00110800) ---------------------------
# 0x67: dst = src1, src2 (two sources, h6 = 0); 0x68: three sources, h6 = 1,
# src0 r/pa only; arg1 (src0) USEASM flag bit 13 -> h15.
form("adif", [("h27-31", 0x16), ("h20-21", 0), ("h6", 0)], common(),
     [DST(), SRC1(), SRC2()], src="FUN_00110800 hw16.0")
form("adifsum", [("h27-31", 0x16), ("h20-21", 0), ("h6", 1)], common() + [Fld("s0f13", "h15")],
     [DST(), SRC0(), SRC1(), SRC2()], src="FUN_00110800 hw16.0")

# --- hw 16 sub-op 1 (USEASM 0x6e, FUN_00118bf0; only on cores with word0 & 2)
# h3-6 = USEASM flags2 bits 15-18; h7 arg8 0x1f (else 0x1e); h8 arg4 0x38
# (else NONE); h9 arg5 0x1c (else NONE); h10-11 arg7 0x39/0x3a/0x3b -> 0/1/2;
# h15 arg6 = internal register i0/i1.
form("op16s1", [("h27-31", 0x16), ("h20-21", 1)],
     common(rpt="h12-13") + [
         Fld("f2_15", "h3-6", "int"),
         Fld("s8", "h7"), Fld("x38", "h8"), Fld("x1c", "h9"),
         Fld("x39", "h10-11", "int"),
         Fld("i", "h15", "int")],
     [DST(), SRC0(), SRC1(), SRC2()], src="FUN_00118bf0 hw16.1")

# --- hw 16 sub-op 2: bilin (USEASM 0x6b, FUN_001111f0) ------------------------
# arg4 fmt h8-9; arg5 0x2d -> h24 (else 0x2e); arg6 0x30 -> h11 (else 0x2f);
# arg7 0x32 -> h10 (else 0x31); arg8 0x33 -> h15 (else NONE). Names guessed
# from the INTSRCSEL order INTERLEAVED/PLANAR, SRC01/SRC23, DST01/DST23, RND.
form("bilin", [("h27-31", 0x16), ("h20-21", 2)],
     common() + [Fld("fmt", "h8-9", "enum", FMT8, show="mod"),
                 Fld("ileave", "h24"), Fld("src23", "h11"), Fld("dst23", "h10"), Fld("rnd", "h15")],
     [DST(), SRC0(), SRC1(_sfmt), SRC2(_sfmt)], src="FUN_001111f0 hw16.2")

# --- hw 16 sub-op 3: ssum16 (USEASM 0x69, FUN_00110be0) ----------------------
# src0 number raw at l14-20 in src1's bank; src1 r/o/pa/sa only. arg4 fmt
# h8-9 (0-2); arg5 0x1b -> h24; arg6 0x1c -> h7; arg7 0x1d -> h10 (else NONE);
# h15 set unless the dest has USEASM argument flag bit 16.
SS16 = [Fld("fmt", "h8-9", "enum", FMT8, show="mod"),
        Fld("x1b", "h24"), Fld("x1c", "h7"), Fld("x1d", "h10"), Fld("nf16", "h15")]
form("ssum16", [("h27-31", 0x16), ("h20-21", 3)], common() + SS16,
     [DST(), SameBank(), SRC1(_sfmt, BANK4), SRC2(_sfmt)], src="FUN_00110be0 hw16.3",
     check=lambda v: get(v, "h8-9") != 3)
# USEASM 0x6a (FUN_001192a0, word0 & 2 cores): h8-9 = 3, src0 = internal
# register number at l14-20, no src1.
form("ssum16i", [("h27-31", 0x16), ("h20-21", 3), ("h8-9", 3)],
     common() + [Fld("x1b", "h24"), Fld("x1c", "h7"), Fld("x1d", "h10"), Fld("nf16", "h15")],
     [DST(), Opd("src0", "l14-20", fixed_kind="i"), SRC2()], src="FUN_001192a0 hw16.3")

# --- hw 17: firv (USEASM 0x6c, FUN_00111850) / firh (0x6d inline) ------------
# src0 shares src1's bank. arg4 fmt h8-9; arg5 (immediate 0-2) h6-7; arg6
# (register type 0x11, 0-7) h3-5; arg7 (immediate -16..15, 5-bit two's
# complement) bits 0-1, 2-3, 4; arg8 (immediate 0-3) h20-21.
FIR = [Fld("fmt", "h8-9", "enum", FMT8, show="mod"),
       Fld("a5", "h6-7", "int"), Fld("coef", "h3-5", "int"), Fld("a8", "h20-21", "int")]
form("firv", [("h27-31", 0x17), ("h2", 0)],
     common(rpt="h12-13") + FIR + [Fld("shift", "h10-11,h14-15,h24", "int")],
     [DST(), SameBank(), SRC1(_sfmt, BANK4), SRC2(_sfmt)], src="FUN_00111850 hw17")
# 0x6d (word0 & 2 cores): dest = internal register (number raw at l21-27),
# h0-1 hold shift bits 2-3, repeat up to 6 in h12-14, h19 = flags2 bit 27.
form("firh", [("h27-31", 0x17), ("h2", 1)],
     common() + FIR + [Fld("shift", "h10-11,h0-1,h24", "int"), Fld("f2_27", "h19")],
     [Opd("dst", "l21-27", fixed_kind="i"), SameBank(), SRC1(_sfmt, BANK4), SRC2(_sfmt)],
     src="FUN_00119f50 0x6d hw17")

# --- hw 1a h20 = 0: ima32 (USEASM 0x65, FUN_00118650) -------------------------
# dst = src0 * src1 + src2 (32-bit). h9 signed (USEASM flags2 0x1000, else
# 0x800); h4-6 high word written to internal register (h6 set, h4-5 = i<n>);
# h10-11 carry-in from i0/i1/i2 (arg5 INTSRCSEL 0x2a + arg6 internal); h8 =
# src0 negate XOR src1 negate; h7 = src2 negate. src0 o/sa via ext h15.
HI = {0: "", 4: "i0", 5: "i1", 6: "i2"}
CIN = {0: "", 1: "i0", 2: "i1", 3: "i2"}


form("ima32", [("h27-31", 0x1a), ("h20", 0)],
     common() + [Fld("sgn", "h9", show="mod"), Fld("hi", "h4-6", "enum", HI),
                 Fld("cin", "h10-11", "enum", CIN), Fld("negp", "h8")],
     [DST(), SRC0(ext="h15"), SRC1(_sgn), SRC2(_sgn, neg="h7")], src="FUN_00118650 hw1a")

# --- hw 1a h20 = 1: USEASM 0x66 inline (cores without target flag bit 16) ----
# dest r/pa only (pa: h7), number raw at l21-27; arg3 register type 10 (DRC?)
# number 0-1 in h0.
form("op1adrc", [("h27-31", 0x1a), ("h20", 1)],
     common() + [Fld("drc", "h0", "int")],
     [Opd("dst", "l21-27", "h7", None, {(0, 0): "r", (1, 0): "pa"}), SRC1(), SRC2()],
     src="FUN_00119f50 0x66 hw1a")
