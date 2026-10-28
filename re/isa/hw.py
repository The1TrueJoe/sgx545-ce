"""SGX545 USSE instruction encoding, as libusc's own assembler writes it.

Every form below is read from the decompiled USE assembler in
libusc.so.1.7.862890 (useasm.c; UseAssembleInstruction at libusc + 0x109f50,
the per-class encoders it dispatches to are named in each form's `src`), and
checked against that same encoder run in an emulator (oracle.py, hwcheck.py).
The tables drive both directions: decode() turns 64 bits into an Inst, and
encode() turns an Inst back into the same 64 bits.

Bit names: l<n> is bit n of the low word, h<n> bit n of the high word (an
instruction is stored low word first). A field spec is a comma list of bits
or ranges, least significant first: "h12-15", "h13,h18", "l25,l27".

Text form (see fmt() / parse()):

    [(p0)] mnemonic[.mod...] dst, src, ... [name name=value ...]

Operands: r<n> temporary, o<n> output, pa<n> primary attribute, sa<n>
secondary attribute, c<n> hardware constant, g<n> special register, i<n>
internal register, #<n> immediate, idx<n> index register, and indexed
operands as bank[idx<n>+off] (pa[idx1+3]). -x negates, |x| takes the
absolute value; .f16 marks a source read as F16 under format select.
"""

# --------------------------------------------------------------------------
# bit fields

def bitlist(spec):
    out = []
    for part in spec.split(","):
        part = part.strip()
        w = 0 if part[0] == "l" else 32
        if "-" in part:
            a, b = part[1:].split("-")
            out += [w + i for i in range(int(a), int(b) + 1)]
        else:
            out.append(w + int(part[1:]))
    return out


def get(v, spec):
    x = 0
    for i, b in enumerate(bitlist(spec)):
        x |= (v >> b & 1) << i
    return x


def put(v, spec, x):
    bl = bitlist(spec)
    if x >> len(bl):
        raise ValueError(f"value {x} does not fit {spec}")
    for i, b in enumerate(bl):
        v = (v & ~(1 << b)) | ((x >> i & 1) << b)
    return v


def mask(spec):
    m = 0
    for b in bitlist(spec):
        m |= 1 << b
    return m


def bname(b):
    return f"l{b}" if b < 32 else f"h{b - 32}"


# --------------------------------------------------------------------------
# operands

# Register banks, from the generic operand encoder (libusc + 0xf7830,
# FUN_00107830): which 2-bit bank value and extension bit each USEASM
# register type gets, per operand slot. 'ix' = indexed, 'cg' = constant
# (number < 64) or special register (number bit 6 set).
BANK_DST = {(0, 0): "r", (1, 0): "o", (2, 0): "pa", (3, 0): "ix",
            (0, 1): "sa", (1, 1): "g", (2, 1): "idx", (3, 1): "i"}
BANK_SRC = {(0, 0): "r", (1, 0): "o", (2, 0): "pa", (3, 0): "sa",
            (0, 1): "ix", (1, 1): "cg", (2, 1): "#", (3, 1): "i"}
BANK_SRC0 = {(0, 0): "r", (1, 0): "pa", (0, 1): "o", (1, 1): "sa"}
IXBANK = ["r", "o", "pa", "sa"]


class Opd:
    """One register operand.

    num: number bits; bank/ext: bank selector bits and extension bit (None if
    the slot has none); table: (bank, ext) -> kind; neg/abs: modifier bits;
    fmt: True if the slot follows format select (h22 in the float classes:
    the number shrinks to 6 bits and bit 6 marks an F16 source)."""

    def __init__(self, role, num, bank=None, ext=None, table=None, neg=None, abs=None, fmt=False,
                 kinds=None, fixed_kind=None, text=None):
        self.role, self.num, self.bank, self.ext = role, num, bank, ext
        self.table = table or {}
        self.neg, self.abs, self.fmt = neg, abs, fmt
        self.kinds = kinds              # kinds the encoder accepts here (None = all in table)
        self.fixed_kind = fixed_kind    # slot with no bank bits (e.g. always a temporary)
        self.text = text                # custom (decode, encode) pair for odd slots

    def nbits(self):
        return len(bitlist(self.num))

    def decode(self, v, d, used):
        if self.text:
            return self.text[0](v, d, used)
        n = get(v, self.num)
        used |= mask(self.num)
        if self.fixed_kind:
            kind = self.fixed_kind
        else:
            b = get(v, self.bank) if self.bank else 0
            e = get(v, self.ext) if self.ext else 0
            used |= (mask(self.bank) if self.bank else 0) | (mask(self.ext) if self.ext else 0)
            kind = self.table.get((b, e), f"?{b}.{e}")
        o = {"kind": kind}
        width = self.nbits()
        if self.fmt and d.get("fmt"):
            o["f16"] = n >> 6 & 1
            n &= 0x3F
            width = 6
        if kind == "ix":
            ob = width - 3
            o.update(kind="ix", bank=IXBANK[n >> (ob + 1) & 3], idx=1 + (n >> ob & 1), off=n & ((1 << ob) - 1))
        elif kind == "cg":
            if width == 7 and n & 0x40:
                o.update(kind="g", n=n & 0x3F)
            else:
                o.update(kind="c", n=n)
        elif kind == "g":
            o["n"] = n & 0x3F
        else:
            o["n"] = n
        for m in ("neg", "abs"):
            spec = getattr(self, m)
            if spec:
                used |= mask(spec)
                if get(v, spec):
                    o[m] = 1
        return o

    def encode(self, v, o, d):
        if self.text:
            return self.text[1](v, o, d)
        kind = o["kind"]
        width = self.nbits()
        fmt = self.fmt and d.get("fmt")
        if fmt:
            width = 6
        if self.fixed_kind:
            if kind != self.fixed_kind:
                raise ValueError(f"{self.role}: only {self.fixed_kind} here, not {kind}")
        else:
            look = "cg" if kind in ("c", "g") and "cg" in self.table.values() else kind
            keys = [k for k, val in self.table.items() if val == look]
            if not keys or (self.kinds and kind not in self.kinds):
                raise ValueError(f"{self.role}: {kind} not encodable here")
            b, e = keys[0]
            if self.bank:
                v = put(v, self.bank, b)
            elif b:
                raise ValueError(f"{self.role}: {kind} needs bank bits this slot lacks")
            if self.ext:
                v = put(v, self.ext, e)
            elif e:
                raise ValueError(f"{self.role}: {kind} needs an extension bit this slot lacks")
        if kind == "ix":
            ob = width - 3
            if o["off"] >> ob:
                raise ValueError(f"{self.role}: index offset {o['off']} > {(1 << ob) - 1}")
            n = IXBANK.index(o["bank"]) << (ob + 1) | (o["idx"] - 1) << ob | o["off"]
        elif kind == "g":
            if width != 7:
                raise ValueError(f"{self.role}: special registers need a 7-bit number field")
            n = o["n"] | 0x40
        else:
            n = o["n"]
        if fmt:
            n |= o.get("f16", 0) << 6
            width = 7
        if n >> self.nbits():
            raise ValueError(f"{self.role}: number {n} too big for {self.num}")
        v = put(v, self.num, n)
        for m in ("neg", "abs"):
            if o.get(m):
                spec = getattr(self, m)
                if not spec:
                    raise ValueError(f"{self.role}: no {m} modifier here")
                v = put(v, spec, 1)
        return v


def opd_text(o):
    k = o["kind"]
    if k == "ix":
        t = f"{o['bank']}[idx{o['idx']}+{o['off']}]"
    elif k == "#":
        t = f"#{o['n']}"
    elif k == "lbl":
        t = f"L{o['n']}"
    else:
        t = f"{k}{o['n']}"
    if o.get("f16"):
        t += ".f16"
    for x in o.get("sfx", []):
        t += "." + x
    if o.get("abs"):
        t = f"|{t}|"
    if o.get("neg"):
        t = "-" + t
    return t


def opd_parse(t):
    import re
    o = {}
    t = t.strip()
    if t.startswith("-"):
        o["neg"] = 1
        t = t[1:]
    if t.startswith("|") and t.endswith("|"):
        o["abs"] = 1
        t = t[1:-1]
    parts = t.split(".")
    t, sfx = parts[0], parts[1:]
    if "f16" in sfx:
        o["f16"] = 1
        sfx.remove("f16")
    if sfx:
        o["sfx"] = sfx
    m = re.fullmatch(r"(r|o|pa|sa)\[idx(\d)\+(\d+)\]", t)
    if m:
        o.update(kind="ix", bank=m.group(1), idx=int(m.group(2)), off=int(m.group(3)))
        return o
    m = re.fullmatch(r"(r|o|pa|sa|c|g|i|idx|#|L|p)(-?\d+|0x[0-9a-fA-F]+)", t)
    if not m:
        raise ValueError(f"bad operand {t!r}")
    k = {"L": "lbl"}.get(m.group(1), m.group(1))
    o.update(kind=k, n=int(m.group(2), 0))
    return o


# --------------------------------------------------------------------------
# instruction fields and forms

class Fld:
    """A non-operand field. kind: 'bool' (printed as its name when set),
    'int' (name=value when not default), 'enum' (table value; '' prints
    nothing). when: callable(decoded so far) -> bool, for fields whose bits
    mean something else in other modes. show: 'mod' puts it on the mnemonic
    (fmad.sat), 'flag' in the trailing brackets (default)."""

    def __init__(self, name, spec, kind="bool", table=None, when=None, show="flag", default=0, bias=0):
        self.name, self.spec, self.kind, self.table = name, spec, kind, table
        self.when, self.show, self.default, self.bias = when, show, default, bias

    def active(self, d):
        return self.when is None or self.when(d)


class Form:
    def __init__(self, name, fixed, fields, ops, src="", mn=None, check=None):
        self.name = name                # base mnemonic
        self.fixed = fixed              # [(spec, value)] that identify the form
        self.fields = fields            # [Fld]
        self.ops = ops                  # [Opd] in text order
        self.src = src                  # where in libusc the encoder lives
        self.mn = mn                    # optional callable(d) -> mnemonic suffix list
        self.check = check
        self.fmask = 0
        self.fval = 0
        for spec, val in fixed:
            self.fmask |= mask(spec)
            self.fval = put(self.fval, spec, val)

    def matches(self, v):
        return (v & self.fmask) == self.fval and (self.check is None or self.check(v))


FORMS = []


def form(*a, **k):
    f = Form(*a, **k)
    FORMS.append(f)
    return f


class Inst:
    def __init__(self, form, d, ops, unknown):
        self.form, self.d, self.ops, self.unknown = form, d, ops, unknown


def find_form(v):
    best = None
    for f in FORMS:
        if f.matches(v):
            if best is None or bin(f.fmask).count("1") > bin(best.fmask).count("1"):
                best = f
    return best


def decode(lo, hi):
    v = hi << 32 | lo
    f = find_form(v)
    if f is None:
        return None
    used = f.fmask
    d = {}
    for fl in f.fields:
        if fl.active(d):
            d[fl.name] = get(v, fl.spec) + fl.bias
            used |= mask(fl.spec)
    ops = []
    u = [used]
    for op in f.ops:
        if callable(getattr(op, "when", None)) and not op.when(d):
            continue
        holder = {"u": 0}
        o = op.decode(v, d, _Used(holder))
        u[0] |= holder["u"]
        ops.append(o)
    used = u[0]
    unknown = [bname(b) for b in range(64) if v >> b & 1 and not used >> b & 1]
    return Inst(f, d, ops, unknown)


class _Used:
    """lets Opd.decode do `used |= mask` on a shared holder"""

    def __init__(self, h):
        self.h = h

    def __ior__(self, m):
        self.h["u"] |= m
        return self


def encode(inst):
    f = inst.form
    v = f.fval
    d = dict(inst.d)
    for fl in f.fields:
        if fl.active(d):
            x = d.get(fl.name, fl.default) - fl.bias
            v = put(v, fl.spec, x)
    ops = [op for op in f.ops if not (callable(getattr(op, "when", None)) and not op.when(d))]
    if len(ops) != len(inst.ops):
        raise ValueError(f"{f.name}: {len(ops)} operands, got {len(inst.ops)}")
    for op, o in zip(ops, inst.ops):
        v = op.encode(v, o, d)
    return v & 0xFFFFFFFF, v >> 32


# --------------------------------------------------------------------------
# text

PRED3 = {0: "", 1: "p0", 2: "p1", 3: "p2", 4: "p3", 5: "!p0", 6: "!p1", 7: "pn"}
PRED2 = {0: "", 1: "p0", 2: "p1", 3: "!p0"}


def fmt(inst):
    f, d = inst.form, inst.d
    mn = [f.name]
    flags = []
    pre = ""
    for fl in f.fields:
        if fl.name not in d:
            continue
        x = d[fl.name]
        if fl.kind == "pred":
            if fl.table[x]:
                pre = f"({fl.table[x]}) "
            continue
        if fl.kind == "hidden":
            continue
        if fl.kind == "bool":
            if x:
                (mn if fl.show == "mod" else flags).append(fl.name)
        elif fl.kind == "enum":
            t = fl.table.get(x, f"{fl.name}?{x}")
            if t:
                (mn if fl.show == "mod" else flags).append(t if fl.show == "mod" else f"{fl.name}={t}")
        else:
            if x != fl.default:
                flags.append(f"{fl.name}={x}")
    if f.mn:
        mn += f.mn(d)
    text = pre + ".".join(mn)
    if inst.ops:
        text += " " + ", ".join(opd_text(o) for o in inst.ops)
    if flags:
        text += "  [" + " ".join(flags) + "]"
    return text


def forms_named(name):
    return [f for f in FORMS if f.name == name]


def parse(text):
    """inverse of fmt(): -> Inst (the first form of that name that encodes it)"""
    import re
    t = text.strip()
    pred = None
    m = re.match(r"\((!?p[0-3n])\)\s*", t)
    if m:
        pred = m.group(1)
        t = t[m.end():]
    flags = []
    if "  [" in t:
        t, fl = t.split("  [", 1)
        flags = fl.rstrip("] ").split()
    t = t.strip()
    mnem, _, rest = t.partition(" ")
    parts = mnem.split(".")
    ops = [opd_parse(x) for x in _split_ops(rest)] if rest.strip() else []
    errs = []
    for f in FORMS:
        if f.name != parts[0]:
            continue
        try:
            d = {}
            mods = parts[1:]
            for fl in f.fields:
                if fl.kind == "pred":
                    inv = {v: k for k, v in fl.table.items()}
                    d[fl.name] = inv[pred or ""]
                elif fl.show == "mod":
                    if fl.kind == "bool":
                        d[fl.name] = int(fl.name in mods)
                    elif fl.kind == "enum":
                        hit = [k for k, v in fl.table.items() if v and v in mods]
                        d[fl.name] = hit[0] if hit else [k for k, v in fl.table.items() if v == ""][0]
            used_mods = set()
            for fl in f.fields:
                if fl.show == "mod":
                    if fl.kind == "bool" and d.get(fl.name):
                        used_mods.add(fl.name)
                    elif fl.kind == "enum" and fl.table.get(d.get(fl.name)):
                        used_mods.add(fl.table[d[fl.name]])
            extra = [m for m in mods if m not in used_mods]
            if extra != (f.mn(d) if f.mn else []):
                raise ValueError(f"modifiers {extra} not in this form")
            for x in flags:
                k, _, val = x.partition("=")
                fl = [y for y in f.fields if y.name == k]
                if not fl:
                    raise ValueError(f"no field {k}")
                fl = fl[0]
                if fl.kind == "bool":
                    d[k] = 1
                elif fl.kind == "enum":
                    inv = {v: kk for kk, v in fl.table.items()}
                    d[k] = inv[val] if val in inv else int(val, 0)
                else:
                    d[k] = int(val, 0)
            inst = Inst(f, d, ops, [])
            lo, hi = encode(inst)
            back = decode(lo, hi)
            if back is None or back.form is not f:
                raise ValueError("decodes as another form")
            return inst
        except (ValueError, KeyError, IndexError) as e:
            errs.append(f"{f.src or f.name}: {e}")
    raise ValueError(f"cannot assemble {text!r}: " + "; ".join(errs) if errs else f"unknown mnemonic {parts[0]}")


def _split_ops(s):
    out, cur, depth = [], "", 0
    for ch in s:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


# --------------------------------------------------------------------------
# the forms live in hw_<class>.py, one module per group of hardware opcodes

import glob as _glob  # noqa: E402
import importlib as _il  # noqa: E402
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402

_sys.modules.setdefault("hw", _sys.modules[__name__])
for _p in sorted(_glob.glob(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "hw_*.py"))):
    _il.import_module(_os.path.basename(_p)[:-3])
