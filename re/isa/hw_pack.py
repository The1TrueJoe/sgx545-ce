"""Pack/unpack, test, testmask and bitwise forms: hardware opcodes 08 (pck),
09 (test), 0a (and/or), 0b (xor), 0c (shl/rol), 0d (shr/asr), 0e (rlp),
0f (testmask).

Encoders (Ghidra addresses, libusc file vaddr + 0x10000):
  FUN_001165c0  pack/unpack (USEASM 0x14-0x4e, 0x16f, 0x170)
  FUN_00112770  test/testmask (any ALU op with flags1 bit 30, TEST)
  FUN_00117270  and/or/xor/shl/rol/shr/asr/rlp (USEASM 0x4f-0x56)
with the generic operand encoder FUN_00107830 (src1 slot FUN_001080b0, src2
slot FUN_00108040, dest FUN_0010b9d0) and the predicate field FUN_00106f30.
See ISA.md, "Pack/unpack, test, testmask, bitwise" for the evidence behind every field.
"""
from hw import BANK_DST, BANK_SRC, PRED3, Fld, Opd, form, get, put, mask

# --------------------------------------------------------------------------
# shared pieces


def _common(end=True):
    # pred h24-26, skipinv h23 (f1 b0), nosched h22 (f1 b3), syncs h20 (f1 b2),
    # end h18 (f1 b17). nosched sits at h22 in all three encoders here.
    f = [Fld("pred", "h24-26", "pred", PRED3),
         Fld("skipinv", "h23"),
         Fld("nosched", "h22"),
         Fld("syncs", "h20")]
    if end:
        f.append(Fld("end", "h18"))
    return f


def _rpt():
    # f1 repeat count n>0: h21 set, h12-15 = n-1; else h21 clear and h12-15 =
    # f1 write mask (b9-12) verbatim (pack and bitwise encoders, same code).
    return [Fld("rcnt", "h21"),
            Fld("rpt", "h12-15", "int", bias=1, default=1, when=lambda d: d.get("rcnt")),
            Fld("mask", "h12-15", "int", default=1, when=lambda d: not d.get("rcnt"))]


class COpd(Opd):
    """Opd with an optional 'when' (operand present only in some modes) and a
    per-slot extra bit group printed as an operand suffix (sfx)."""

    def __init__(self, *a, when=None, lane=None, fmtname=None, **k):
        super().__init__(*a, **k)
        self.when = when
        self.lane = lane            # spec of a 2-bit lane select printed as .l<n>
        self.fmtname = fmtname      # callable(d) -> name printed instead of .f16

    def halved(self, o, d):
        # under test's format select an F16 operand's number counts halves like the float classes'
        # (hardware, hwt_p10); a C10 operand (group-2 ops) is a whole register
        if self.fmtname and self.fmtname(d) != "f16":
            return False
        return super().halved(o, d)

    def decode(self, v, d, used):
        o = super().decode(v, d, used)
        if self.lane:
            used |= mask(self.lane)
            n = get(v, self.lane)
            if n:
                o.setdefault("sfx", []).append(f"l{n}")
        if self.fmtname and (o.get("f16") or "half" in o):
            nm = self.fmtname(d)
            if nm != "f16":
                # a C10 operand: hw.Opd read the number as F16 halves; undo that
                if "half" in o:
                    o["n"] = o["n"] << 1 | o.pop("half")
                o.pop("f16", None)
                o.setdefault("sfx", []).append(nm)
        return o

    def encode(self, v, o, d):
        o = dict(o)
        sfx = list(o.get("sfx", []))
        if self.lane:
            ln = [x for x in sfx if len(x) == 2 and x[0] == "l" and x[1].isdigit()]
            if ln:
                sfx.remove(ln[0])
                v = put(v, self.lane, int(ln[0][1]))
        if self.fmtname:
            nm = self.fmtname(d)
            if nm in sfx:
                sfx.remove(nm)
                o["f16"] = 1
        if sfx:
            raise ValueError(f"{self.role}: unknown operand suffix {sfx}")
        o.pop("sfx", None)
        return super().encode(v, o, d)


def _dst(**k):
    return COpd("dst", "l21-27", "h0-1", "h19", BANK_DST, **k)


def _src1(**k):
    return COpd("src1", "l7-13", "l30-31", "h17", BANK_SRC, **k)


def _src2(**k):
    return COpd("src2", "l0-6", "l28-29", "h16", BANK_SRC, **k)


class POpd(Opd):
    """a predicate register number in a 2-bit field: p0-p3"""

    def __init__(self, role, spec, when=None):
        super().__init__(role, spec)
        self.when = when

    def decode(self, v, d, used):
        used |= mask(self.num)
        return {"kind": "p", "n": get(v, self.num)}

    def encode(self, v, o, d):
        if o["kind"] != "p" or o["n"] > 3:
            raise ValueError(f"{self.role}: needs p0-p3")
        return put(v, self.num, o["n"])


# --------------------------------------------------------------------------
# hw 08: pack/unpack. FUN_001165c0. Formats: h9-11 source, h6-8 destination
# (USEASM op -> local_34 table; the order is that of the USEASM op enum
# PCKF16F32 = 0x14, PCKF16F16, PCKF16U16, PCKF16S16, PCKU16F32, ...).

PCKFMT = ["u8", "s8", "o8", "u16", "s16", "f16", "f32", "c10"]
FLOATS = (5, 6)
RND = {0: "", 1: "down", 2: "up", 3: "zero"}

for _sf in range(8):
    for _df in range(8):
        _ff = _sf in FLOATS and _df in FLOATS
        _flds = _common() + _rpt() + [
            # destination byte write mask, USEASM arg0 flags b7-10
            Fld("dmask", "h2-5", "int", default=0xF),
            # l18: f2 b8 on float<->int pairs (normalise), f1 b24 on
            # float->float pairs (the compiler's round/trunc ops)
            Fld("rint" if _ff else "scale", "l18"),
            # rounding mode, arg3 (type 0x10): USEASM 0x3a->0, 0x39->1, 0x3b->2, 0->3
            Fld("rnd", "l19-20", "enum", RND)]
        form("pck", [("h27-31", 8), ("h9-11", _sf), ("h6-8", _df)], _flds,
             # source lane selects (arg flags b5-6): src1 l16-17, src2 l14-15
             [_dst(), _src1(lane="l16-17"), _src2(lane="l14-15")],
             src="FUN_001165c0 hw08", mn=lambda d, a=PCKFMT[_df], b=PCKFMT[_sf]: [a, b])


# --------------------------------------------------------------------------
# hw 09 test, hw 0f testmask. FUN_00112770. The ALU op (l14-19) is the
# operation the TEST-flagged USEASM op names; the condition (h7-11) is the
# test spec (USEASM dw4) bits 0-4 in the same order.

_F = {0: "fadd", 3: "fsubflr", 4: "frcp", 5: "frsq", 6: "flog2", 7: "fexp2", 8: "fdp", 9: "fmin",
      10: "fmax", 11: "fdsx", 12: "fdsy", 13: "fmul", 14: "fsub"}
_I16 = {6: "iadd16", 7: "isub16", 8: "imul16", 9: "iaddu16", 10: "isubu16", 11: "imulu16",
        12: "iadd32", 13: "iaddu32"}
_I8 = {0: "i8op0", 1: "i8op1", 2: "i8op2", 3: "i8op3", 4: "i8op4", 5: "i8op5", 6: "i8op6",
       7: "fpadd8", 8: "fpsub8"}
_BIT = {0: "and", 1: "or", 2: "xor", 3: "shl", 4: "shr", 5: "rol", 7: "asr"}
ALU = {}
for _g, _t in enumerate((_F, _I16, _I8, _BIT)):
    for _s in range(16):
        ALU[_g << 4 | _s] = _t.get(_s, f"alu{_g << 4 | _s:02x}")

# condition: value = sign test (h10-11) | zero test (h8-9) << 2 | and (h7) << 4
COND = {}
for _v in range(32):
    COND[_v] = f"s{_v & 3}z{_v >> 2 & 3}{'a' if _v >> 4 else 'o'}"
COND.update({0x14: "eq", 0x18: "ne", 0x19: "lt", 0x1A: "gt", 0x05: "le", 0x06: "ge",
             0x11: "neg", 0x12: "pos"})  # neg/pos: sign bit set/clear

TMTYPE = {0: "byte", 1: "word", 2: "dword", 3: "tm3"}


def _fmtname(d):
    # per-source format bit: F16 for the float group, C10 for group 2 (USEASM
    # argument flag b24 instead of b22)
    return "c10" if d.get("alu", 0) >> 4 == 2 else "f16"


def _test_fields(tm, wdst):
    f = [Fld("pred", "h24-26", "pred", PRED3),
         Fld("skipinv", "h23"),
         Fld("nosched", "h22"),
         Fld("syncs", "h20"),
         Fld("f2_0", "h21"),                        # USEASM flags2 bit 0
         Fld("fmt", "h18"),                         # sources (and dest) have a format bit
         Fld("alu", "l14-19", "enum", ALU, show="mod"),
         Fld("cond", "h10-11,h8-9,h7", "enum", COND, show="mod")]
    if tm:
        f += [Fld("type", "h4-5", "enum", TMTYPE, show="mod")]
    else:
        f += [Fld("chan", "h4-6", "int")]
    f += [Fld("mask", "h12-15", "int", default=1)]
    if not wdst:
        # no destination (USEASM arg0 flag b16): l20 clear, dest field = h0 + h19
        f += [Fld("nodst", "l21-27,h0-1,h19", "int", default=0x280)]
    return f


for _tm, _op in ((0, 9), (1, 0xF)):
    for _w in (0, 1):
        _ops = []
        if not _tm:
            _ops.append(POpd("pdst", "h2-3"))
        if _w:
            _ops.append(_dst(fmt=True, fmtname=_fmtname))
        _ops += [_src1(fmt=True, fmtname=_fmtname), _src2(fmt=True, fmtname=_fmtname)]
        form("testmask" if _tm else "test", [("h27-31", _op), ("l20", _w)], _test_fields(_tm, _w), _ops,
             src=f"FUN_00112770 hw{_op:02x}")


# --------------------------------------------------------------------------
# hw 0a-0e: bitwise. FUN_00117270. src2 may be a 16-bit immediate
# (l0-6, l14-20, h4-5) rotated left by h6-10; h11 inverts src2.


def _imm_dec(v, d, used):
    b = get(v, "l28-29")
    e = get(v, "h16")
    used |= mask("l0-6,l28-29,h16")
    if (b, e) == (2, 1):
        used |= mask("l14-20,h4-5")
        return {"kind": "#", "n": get(v, "l0-6") | get(v, "l14-20") << 7 | get(v, "h4-5") << 14}
    return _BSRC2.decode(v, d, used)


def _imm_enc(v, o, d):
    if o["kind"] == "#":
        n = o["n"]
        if n >> 16:
            raise ValueError("src2: immediate > 16 bits (use rot / inv)")
        v = put(v, "l28-29", 2)
        v = put(v, "h16", 1)
        v = put(v, "l0-6", n & 0x7F)
        v = put(v, "l14-20", n >> 7 & 0x7F)
        return put(v, "h4-5", n >> 14)
    return _BSRC2.encode(v, o, d)


_BSRC2 = Opd("src2", "l0-6", "l28-29", "h16", BANK_SRC)


def _bit_fields():
    return _common() + _rpt() + [
        Fld("inv", "h11", show="mod"),          # src2 inverted (arg2 flag 0x800)
        Fld("partial", "h2"),                   # USEASM flags1 bit 20
        Fld("rot", "h6-10", "int")]             # immediate rotate-left count


for _op, _sub, _name in ((0xA, 0, "and"), (0xA, 1, "or"), (0xB, 0, "xor"), (0xC, 0, "shl"),
                         (0xC, 1, "rol"), (0xD, 0, "shr"), (0xD, 1, "asr")):
    form(_name, [("h27-31", _op), ("h3", _sub)], _bit_fields(),
         [_dst(), _src1(), Opd("src2", "l0-6", text=(_imm_dec, _imm_enc))], src=f"FUN_00117270 hw{_op:02x}")

# hw 0e rlp: src2 slot unused (encoded as #0: l28-29 = 2, h16), the predicate
# source number in l14-15.
form("rlp", [("h27-31", 0xE)],
     _common() + _rpt() + [Fld("partial", "h2"), Fld("unused2", "l0-6,l28-29,h16", "int", default=0x300)],
     [_dst(), _src1(), POpd("psrc", "l14-15")], src="FUN_00117270 hw0e")
