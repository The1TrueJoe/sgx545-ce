"""Float ALU forms: hardware opcodes 0 (fmad and friends), 1 (scalar
functions), 2 (fdp), 3 (fmin/fmax), 4 (fdsx/fdsy), 6 (fmad16).

Encoder: libusc + 0x1047f0 (FUN_001147f0 in a Ghidra load at 0x10000), with
the repeat/mask helper + 0xf7080 and the operand encoder + 0xf7830.
"""
from hw import BANK_DST, BANK_SRC, BANK_SRC0, PRED3, Fld, Opd, form

# --- fields every ALU class shares (encoder: pred via + 0xf6f30, the rest
# OR'd in directly). Names: libusc's asm.c sets these USEASM flags from its
# own instruction flags INST_SKIPINV (h23), INST_SYNCSTART (h20) and
# INST_NOSCHED (h11), which its asserts name; h18 ends the program (seen on
# hardware: the last instruction of every finalised program has it).


def common(end="h18", sync="h20"):
    f = [Fld("pred", "h24-26", "pred", PRED3),
         Fld("skipinv", "h23"),
         Fld("nosched", "h11")]
    if sync:
        f.append(Fld("syncs", sync))
    if end:
        f.append(Fld("end", end))
    return f


def DST(fmt=False):
    return Opd("dst", "l21-27", "h0-1", "h19", BANK_DST, fmt=fmt)


def SRC0(neg=None, abs=None, fmt=False):
    return Opd("src0", "l14-20", "h2", None, BANK_SRC0, neg=neg, abs=abs, fmt=fmt)


def SRC1(neg=None, abs=None, fmt=False):
    return Opd("src1", "l7-13", "l30-31", "h17", BANK_SRC, neg=neg, abs=abs, fmt=fmt)


def SRC2(neg=None, abs=None, fmt=False):
    return Opd("src2", "l0-6", "l28-29", "h16", BANK_SRC, neg=neg, abs=abs, fmt=fmt)


# Repeat and increments on SGX545 (target feature 0x8 set): h12-13 = repeat
# count - 1 (1..4; a write mask is turned into a count). h21 set: operands
# step by the MOE increments (libusc INST_MOE). h21 clear: each operand
# steps by one only if its bit is set: h15 (USEASM flags2 bit 29), h14 (bit
# 30), h8 (bit 31); asm.c fills them from the instruction's per-source
# increment mask, and the corpus has bit 30 with a register second source,
# bit 31 with a register third source.
def rpt_moe():
    return [Fld("rpt", "h12-13", "int", bias=1, default=1),
            Fld("moe", "h21"),
            Fld("inc0", "h15", when=lambda d: not d.get("moe")),
            Fld("inc1", "h14", when=lambda d: not d.get("moe")),
            Fld("inc2", "h8", when=lambda d: not d.get("moe"))]


# h22: format select. Set when any source has USEASM argument flag 22; every
# source number then has 6 bits and bit 6 says that source is F16.
FMT = Fld("fmt", "h22")

# --- hw 0: three-source float. Sub-op h9-10.
for sub, name in ((0, "fmad"), (1, "fadm"), (2, "fmsa"), (3, "fsubflr")):
    if sub < 3:
        ops = [DST(), SRC0(abs="h7", fmt=True), SRC1(neg="h5", abs="h6", fmt=True),
               SRC2(neg="h3", abs="h4", fmt=True)]
    else:
        ops = [DST(), SRC1(neg="h5", abs="h6", fmt=True), SRC2(neg="h3", abs="h4", fmt=True)]
    form(name, [("h27-31", 0), ("h9-10", sub)], common() + rpt_moe() + [FMT], ops, src="0x1047f0 hw0")

# --- hw 1: one-source functions. Sub-op h9-10 + h16.
SCALAR = ["frcp", "frsq", "flog2", "fexp2", "fsqrt", "fsin", "fcos"]


def _scalar_src():
    return Opd("src", "l7-13", "l30-31", "h17", BANK_SRC, neg="h5", abs="h6")


for sub, name in enumerate(SCALAR):
    form(name, [("h27-31", 1), ("h9-10", sub & 3), ("h16", sub >> 2)],
         common() + [
             # h21 = repeat count select: set, h12-15 is the count - 1; clear, a write mask
             Fld("rcnt", "h21"),
             Fld("rpt", "h12-15", "int", bias=1, default=1, when=lambda d: d.get("rcnt")),
             Fld("mask", "h12-15", "int", default=0, when=lambda d: not d.get("rcnt")),
             # source format: h7 F16 (h3 = high half), h8 a packed format with
             # h3-4 picking the channel (USEASM argument flags 22 and 24)
             Fld("f16", "h7"), Fld("c10", "h8"),
             Fld("chan", "h3-4", "int"),
             # USEASM flags2 bit 22: F16 destination (hardware, hwt_p3: with an
             # F16 source the result goes, as F16, into the half h3 selects and
             # the other half is zeroed; no effect with an f32 source)
             Fld("f16d", "h2")],
         [DST(), _scalar_src()], src="0x1047f0 hw1")

# --- hw 3: three-source min/max. libusc writes fmin(a, b) as sub-op 1 on
# (a, a, b) and fmax(a, b) as sub-op 0 on (a, b, a) (encoder, USEASM 0xc/0xd
# rewritten to 0x11/0x12).
for sub, name in ((0, "fmaxmin"), (1, "fminmax")):
    form(name, [("h27-31", 3), ("h9-10", sub)], common() + rpt_moe() + [FMT],
         [DST(), SRC0(abs="h7", fmt=True), SRC1(neg="h5", abs="h6", fmt=True),
          SRC2(neg="h3", abs="h4", fmt=True)], src="0x1047f0 hw3")

# --- hw 2, 4: two-source. src1 = USEASM argument 1, src2 = argument 2.
for op, sub, name in ((2, 0, "fdp"), (4, 0, "fdsx"), (4, 1, "fdsy")):
    form(name, [("h27-31", op), ("h9-10", sub)], common() + rpt_moe() + [FMT],
         [DST(), SRC1(neg="h5", abs="h6", fmt=True), SRC2(neg="h3", abs="h4", fmt=True)], src=f"0x1047f0 hw{op}")

# --- hw 6: fmad16. Per-source F16 swizzle (USEASM argument flags 25-26):
# src0 h9-10, src1 h13 + h18, src2 h21-22. Repeat 1-2 in h12 (needs the
# explicit increments h15/h14/h8).
# Values are libusc's FMAD16_SWIZZLE enum (f16opt.c asserts compare it with
# LOWLOW = 1, HIGHHIGH = 2, CVTFROMF32 = 3; asm.c passes it straight to the
# USEASM argument flags): 0 each half its own, 1 both halves read the low
# half, 2 both read the high half, 3 the source is F32, converted.
SWZ16 = {0: "", 1: "ll", 2: "hh", 3: "f32"}
form("fmad16", [("h27-31", 6)],
     common(end=None) + [
         Fld("rpt", "h12", "int", bias=1, default=1),
         Fld("inc0", "h15"), Fld("inc1", "h14"), Fld("inc2", "h8"),
         Fld("swz0", "h9-10", "enum", SWZ16), Fld("swz1", "h13,h18", "enum", SWZ16),
         Fld("swz2", "h21-22", "enum", SWZ16)],
     [DST(), SRC0(abs="h7"), SRC1(neg="h5", abs="h6"), SRC2(neg="h3", abs="h4")], src="0x1047f0 hw6")
