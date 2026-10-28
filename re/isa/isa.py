"""SGX545 USSE decoding from fields.json (made by mkfields.py from encfuzz logs).

An instruction is matched to the USEASM opcode whose fixed-bit pattern fits
with the most fixed bits, then each argument is read from the bits libusc's
encoder was seen to drive for it. Bits that no table explains are returned as
unknown, so the disassembler can print them."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = json.load(open(os.path.join(HERE, "fields.json")))

# USEASM register type -> prefix. 0-3 and 7 are backed by what the corpus does
# with them (temps, varyings in pa, uniforms/secondary results in sa, the
# internal registers aliased to r124-127); 5, 6, 8 by where the encoder puts
# them (6 is what the compiler uses for "special constant zero"); 8 is what
# unused sources are set to, value 0. See ISA.md.
TYPE = {0: "r", 1: "o", 2: "pa", 3: "sa", 4: "x", 5: "g", 6: "c", 7: "i", 8: "#",
        9: "p", 12: "drc", 13: "lbl", 15: "t15:", 16: "t16:", 18: "t18:", 21: "t21:"}

# USEASM opcode -> (mnemonic, how sure). "corpus": a one-op shader compiles to
# it; "sweep": libusc encodes it into the same hardware opcode and sub-op as a
# corpus-backed neighbour, name from the IMG enum order (see ISA.md); "guess".
NAMES = json.load(open(os.path.join(HERE, "names.json")))
ARGFLAG = {"0": "neg", "1": "abs"}     # corpus: -a*b, abs(a)*b


def ops_by_specificity():
    lst = []
    for key, e in TABLE.items():
        if key == "sweep":
            continue
        ml, mh = e["mask"]
        op = int(key.split("/")[0], 16)
        lst.append((-(bin(ml).count("1") + bin(mh).count("1")), -e["n_enc"], op, e))
    lst.sort(key=lambda x: x[:3])
    return lst


ORDER = ops_by_specificity()


def hw_flag_bits():
    """hardware opcode -> output bits some op-flag input drives in any form of it"""
    m = {}
    for key, e in TABLE.items():
        if key == "sweep":
            continue
        hw = int(key.split("/")[1], 16)
        for f, bm in e["flags"].items():
            for outs in bm.values():
                m.setdefault(hw, set()).update(outs)
    return m


HWFLAGS = hw_flag_bits()


def bitval(lo, hi, name):
    return ((lo if name[0] == "l" else hi) >> int(name[1:])) & 1


def match(lo, hi):
    for _, _, op, e in ORDER:
        ml, mh = e["mask"]
        vl, vh = e["value"]
        if (lo & ml) == vl and (hi & mh) == vh and (ml or mh):
            return op, e
    return None, None


def arg_text(lo, hi, a, used):
    """read one argument; returns text or None when it has no decodable bits"""
    num = 0
    have = False
    for b, outs in sorted(a.get("num", {}).items(), key=lambda x: int(x[0])):
        if len(outs) == 1:
            have = True
            num |= bitval(lo, hi, outs[0]) << int(b)
            used.add(outs[0])
    prefix = ""
    bank = a.get("bank")
    if bank and bank["bits"]:
        s = "".join(str(bitval(lo, hi, n)) for n in bank["bits"])
        types = [int(t) for t, vals in bank["types"].items() if s in vals]
        if not types:
            # the tables were recorded at other register numbers: compare
            # only the bits that aren't number bits
            numbits = {o[0] for o in a.get("num", {}).values() if len(o) == 1}
            keep = [i for i, n in enumerate(bank["bits"]) if n not in numbits]
            p = "".join(s[i] for i in keep)
            types = sorted({int(t) for t, vals in bank["types"].items()
                            if any("".join(v[i] for i in keep) == p for v in vals) and int(t) != 7})
        if types:
            used.update(bank["bits"])
            t = min(types)
            prefix = TYPE.get(t, f"t{t}:")
            if t == 7:                  # internal registers: the low bits pick i0-i3
                num &= 3
            if t == 5 and num >= 64:
                num -= 64
        else:
            prefix = "?bank" + s + ":"
    elif have:
        prefix = "n"
    if not have and not prefix:
        return None
    mods = []
    for b, outs in sorted(a.get("flags", {}).items(), key=lambda x: int(x[0])):
        if all(bitval(lo, hi, o) for o in outs) and not all(o in used for o in outs):
            mods.append(ARGFLAG.get(b, f"f{b}"))
            used.update(outs)
    text = f"{prefix}{num}" if prefix != "#" else f"#{num}"
    for m in mods:
        text = {"neg": f"-{text}", "abs": f"|{text}|"}.get(m, f"{text}.{m}")
    return text


def decode(hi, lo):
    """-> (text, unknown bit names)"""
    op, e = match(lo, hi)
    if e is None:
        u = [f"l{i}" for i in range(32) if lo >> i & 1] + [f"h{i}" for i in range(32) if hi >> i & 1]
        return f"hw{hi >> 27:02x}?", u
    used = set()
    ml, mh = e["mask"]
    used.update(f"l{i}" for i in range(32) if ml >> i & 1)
    used.update(f"h{i}" for i in range(32) if mh >> i & 1)
    key = f"{op:#x}/{hi >> 27:02x}"
    name, how = NAMES.get(key) or NAMES.get(f"{op:#x}") or [f"op{op:#x}", "guess"]
    mn = name + ("" if how == "corpus" else "?")
    args = []
    for an in sorted(e["args"], key=lambda x: int(x[1:])):
        t = arg_text(lo, hi, e["args"][an], used)
        if t is not None:
            args.append(t)
    flags = []
    f1 = e["flags"].get("f1", {})
    # predicate: USEASM flags1 bits 13-15 land in h24-h26 (encoder)
    if any(o in ("h24", "h25", "h26") for b in ("13", "14", "15") for o in f1.get(b, [])):
        p = hi >> 24 & 7
        used.update(("h24", "h25", "h26"))
        if p:
            flags.append(f"p={p}")
    # repeat count / channel mask: flags1 bits 4-12 drive h12-h15 (and h21) (encoder)
    rm = sorted({o for b in map(str, range(4, 13)) for o in f1.get(b, []) if o in ("h12", "h13", "h14", "h15", "h21")},
                key=lambda x: int(x[1:]))
    if rm:
        if any(bitval(lo, hi, o) for o in rm):
            flags.append("rm[" + ",".join(rm) + "]=" + "".join(str(bitval(lo, hi, o)) for o in reversed(rm)))
        used.update(rm)
    for f, m in sorted(e["flags"].items()):
        for b, outs in sorted(m.items(), key=lambda x: int(x[0])):
            if len(outs) == 1 and bitval(lo, hi, outs[0]) and outs[0] not in used:
                flags.append(f"{f}.{b}")
                used.add(outs[0])
    # multi-bit op-flag fields (e.g. the predicate): print their raw value
    for f, m in sorted(e["flags"].items()):
        multi = sorted({o for outs in m.values() if len(outs) > 1 for o in outs} - used,
                       key=lambda x: (x[0], int(x[1:])))
        if multi and any(bitval(lo, hi, o) for o in multi):
            flags.append(f"{f}[{','.join(multi)}]=" + "".join(str(bitval(lo, hi, o)) for o in multi))
            used.update(multi)
    for ob, srcs in sorted(e.get("corr", {}).items()):
        if bitval(lo, hi, ob) and ob not in used:
            flags.append(f"{srcs[0]}~{ob}")
            used.add(ob)
    # bits a flag word drives in another form of this hardware opcode
    ctl = [b for b in sorted(HWFLAGS.get(hi >> 27, ()), key=lambda x: (x[0], int(x[1:])))
           if bitval(lo, hi, b) and b not in used]
    if ctl:
        flags.append("ctl[" + ",".join(ctl) + "]")
        used.update(ctl)
    unknown = [f"l{i}" for i in range(32) if lo >> i & 1 and f"l{i}" not in used]
    unknown += [f"h{i}" for i in range(32) if hi >> i & 1 and f"h{i}" not in used]
    text = f"{mn:10} " + ", ".join(args) + (f"  [{' '.join(flags)}]" if flags else "")
    return text, unknown
