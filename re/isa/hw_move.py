"""Moves (hw 05), the dot-product variants that share hw 02 with fdp, and
dual-issue (hw 1b).

Encoders: mov libusc + 0x102370 (FUN_00112370), movc + 0xfba40, USEASM 0xf3
+ 0x109b00; fdpc + 0x1059f0, fddp + 0x105d10, fddpc + 0x105540; fdual
+ 0x103620 (Ghidra = libusc + 0x10000).
"""
from hw import BANK_SRC, BANK_SRC0, PRED2, Fld, Opd, form
from hw_float import DST, SRC0, SRC1, SRC2, common, rpt_moe, FMT

# --- hw 05 mov / movc. h8-10 is the sub-op: 0 mov, 1-5 movc and the type of
# value it tests, 6-7 USEASM 0xf3. Repeat: h21 set = h12-15 is count - 1,
# clear = h12-15 is a write mask (same as the scalar class).


def rpt_mask():
    return [Fld("rcnt", "h21"),
            Fld("rpt", "h12-15", "int", bias=1, default=1, when=lambda d: d.get("rcnt")),
            Fld("mask", "h12-15", "int", default=0, when=lambda d: not d.get("rcnt"))]


# The source is the src1 slot; src2 is written as the unused-source pattern
# (bank 2 + extension = immediate #0) by EncodeUnusedSource (libusc's own
# assert names the helper: useasm.c line 0x9ff).
form("mov", [("h27-31", 5), ("h8-10", 0), ("l28-29", 2), ("h16", 1), ("l0-6", 0)],
     common() + rpt_mask(), [DST(), SRC1()], src="0x102370 mov")

# movc d, s0, s1, s2: d = test(s0) ? s1 : s2. The test is USEASM's test spec
# (dw4: bits 0-1 sign test 1 set / 2 clear, 2-3 zero test 1 zero / 2 nonzero,
# bit 4 AND): only ==0, !=0, <0 and >=0 encode, as h7 + h22. The type comes from libusc's IMOVC variants
# (asm.c): IMOVC -> 4, IMOVC_I32 -> 3, the IMOVC_U8/IMOVC_C10 pair -> 1 and
# 5, the fifth (internal op 0x87) -> 2.
MOVC_TYPE = {1: "u8", 2: "i16", 3: "i32", 4: "f32", 5: "c10"}
MOVC_COND = {0: "z", 1: "nz", 2: "ltz", 3: "gez"}
for t, name in MOVC_TYPE.items():
    form("movc", [("h27-31", 5), ("h8-10", t)],
         common() + rpt_mask() + [Fld("cond", "h7,h22", "enum", MOVC_COND, show="mod")],
         [DST(), SRC0(), SRC1(), SRC2()], src="0xfba40 movc", mn=lambda d, n=name: [n])

# USEASM 0xf3 (needs target feature 0x2000; libusc's compiler never emits
# it): writes an output register. Sub-op 7 takes a src0, sub-op 6 has none
# (its USEASM argument must be int-source-select 0x15). h7/h22 come from
# USEASM flags1 bit 1, flags1 bit 26 and flags2 bit 21 (the same flags that
# pick movc's u8 / f32 types); h5-6 = the third source's argument flags 5-6.
for sub, ops in ((6, [DST(), SRC1(), SRC2()]), (7, [DST(), SRC0(), SRC1(), SRC2()])):
    form("op0xf3", [("h27-31", 5), ("h8-10", sub)],
         common() + rpt_mask() + [Fld("f1_1", "h7"), Fld("f1_26", "h22"), Fld("a3f", "h5-6", "int")],
         ops, src="0x109b00 op0xf3", mn=lambda d, s=sub: [f"s{s}"])

# --- hw 02 dot products besides fdp (hw_float.py).
# fdpc: fdp that also writes clip-plane result cp<n> (USEASM register type
# 0xd, number 0-7 in l15-17; l14 marks it). Needs a repeat of 2+ or a
# multi-channel mask.
def _cp(spec, name):
    def dec(v, d, used):
        from hw import get, mask
        used |= mask(spec)
        return {"kind": "cp", "n": get(v, spec)}

    def enc(v, o, d):
        from hw import put
        if o["kind"] != "cp":
            raise ValueError(f"{name}: needs cp<n>")
        return put(v, spec, o["n"])
    return Opd(name, spec, text=(dec, enc))


form("fdpc", [("h27-31", 2), ("h9-10", 0), ("l14", 1)], common() + rpt_moe() + [FMT],
     [DST(), _cp("l15-17", "cp"), SRC1(neg="h5", abs="h6", fmt=True), SRC2(neg="h3", abs="h4", fmt=True)],
     src="0x1059f0 fdpc")

# fddp / fddpc: two dot products at once; the second result goes to internal
# register i0 or i1 (h19), so the destination has no extension bit.
DST_NOEXT = Opd("dst", "l21-27", "h0-1", None, {(0, 0): "r", (1, 0): "o", (2, 0): "pa", (3, 0): "ix"})


def _ireg(spec, name):
    def dec(v, d, used):
        from hw import get, mask
        used |= mask(spec)
        return {"kind": "i", "n": get(v, spec)}

    def enc(v, o, d):
        from hw import put
        if o["kind"] != "i":
            raise ValueError(f"{name}: needs i0/i1")
        return put(v, spec, o["n"])
    return Opd(name, spec, text=(dec, enc))


form("fddp", [("h27-31", 2), ("h9-10", 1)], common() + rpt_moe() + [FMT],
     [DST_NOEXT, _ireg("h19", "idst"), SRC0(abs="h7", fmt=True), SRC1(neg="h5", abs="h6", fmt=True),
      SRC2(neg="h3", abs="h4", fmt=True)], src="0x105d10 fddp")
form("fddpc", [("h27-31", 2), ("h9-10", 2)], common() + rpt_moe() + [FMT],
     [DST_NOEXT, _ireg("h19", "idst"), _cp("h3-5", "cp0"), _cp("h6-8", "cp1"),
      SRC0(fmt=True), SRC1(fmt=True), SRC2(fmt=True)], src="0x105540 fddpc")

# --- hw 1b fdual: a primary float op and a secondary op in one instruction
# (USEASM flags1 bit 25 pairs an instruction with the next; target feature
# 0x100). The primary op uses the normal dst/src0/src1/src2 slots; the
# secondary op computes into internal registers.
#   primary op (h18 + h19 + h26):
PRI = {0: "fmad", 1: "mov", 2: "fadm", 3: "fmsa", 4: "fddp", 5: "fminmax", 6: "op65", 7: "opa7"}
#   secondary op (h6-9); fadm, fsubflr and ffrc all encode as 3
SEC = {0: "fmad", 1: "fmad16", 2: "fmsa", 3: "fadm", 4: "fmaxmin", 5: "frcp", 6: "frsq", 7: "fexp2",
       8: "flog2", 9: "fsin", 10: "fcos", 11: "fadd", 12: "fsub", 13: "fmul", 14: "op65", 15: "mov"}
#   secondary destination and first source (h5 + h13): i1 / i2 / i0 (the
#   first source is then the same internal register), or 3: destination i0,
#   first source = the src0 slot
SD = {0: "i0", 1: "i1", 2: "i2", 3: "i0s0"}
#   secondary second source (h14-15): i0 / i1 / i2 / the primary's src1
SB = {0: "i0", 1: "i1", 2: "i2", 3: "src1"}
#   secondary third source (h10 + h21): i0 / i1 / i2 / the src2 slot
SC = {0: "i0", 1: "i1", 2: "i2", 3: "src2"}

form("fdual", [("h27-31", 0x1b)],
     [Fld("pred", "h24-25", "pred", PRED2), Fld("skipinv", "h23"), Fld("nosched", "h11"), Fld("syncs", "h20"),
      Fld("pri", "h18-19,h26", "enum", PRI, show="mod"),
      Fld("sec", "h6-9", "enum", SEC, show="mod"),
      Fld("rpt", "h12", "int", bias=1, default=1),
      FMT,
      Fld("sd", "h5,h13", "enum", SD), Fld("sb", "h14-15", "enum", SB), Fld("sc", "h10,h21", "enum", SC),
      # negate on the source in the src1 / src2 slot (argument flag 0)
      Fld("neg1", "h4"), Fld("neg2", "h3")],
     [DST_NOEXT, SRC0(fmt=True), SRC1(fmt=True), SRC2(fmt=True)], src="0x103620 fdual")
