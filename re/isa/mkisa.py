"""Print the generated part of ISA.md: one section per instruction form found
in fields.json, with the corpus shaders that use it.

  python3 mkisa.py fields.json names.json <encfuzz log>"""
import collections
import json
import sys

from isa import TYPE


def rng(outs_by_bit):
    """{'0': ['l21'], '1': ['l22'], ...} -> 'l21..l27' style text"""
    items = sorted(((int(b), o) for b, o in outs_by_bit.items()), key=lambda x: x[0])
    parts, run = [], None
    for b, o in items:
        one = o[0] if len(o) == 1 else None
        if one and run and b == run[1] + 1 and one[0] == run[3][0] and int(one[1:]) == int(run[3][1:]) + 1:
            run = (run[0], b, run[2], one)
            continue
        if run:
            parts.append(run)
            run = None
        if one:
            run = (b, b, one, one)
        else:
            parts.append((b, b, None, "+".join(o)))
    if run:
        parts.append(run)
    txt = []
    for b0, b1, o0, o1 in parts:
        if o0 is None:
            txt.append(f"b{b0}→{o1}")
        elif b0 == b1:
            txt.append(f"b{b0}→{o0}")
        else:
            txt.append(f"b{b0}-{b1}→{o0}..{o1}")
    return ", ".join(txt)


def uses(path):
    """(op, hw) -> shader names whose code has it"""
    u = collections.defaultdict(list)
    shader = None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == "S":
            shader = t[1]
        elif t[0] == "I":
            eq = t.index("=")
            key = f"{int(t[1], 16):#x}/{int(t[eq + 3], 16) >> 27:02x}"
            if shader not in u[key]:
                u[key].append(shader)
    return u


def main(fields, names, log):
    T = json.load(open(fields))
    N = json.load(open(names))
    U = uses(log)
    keys = sorted((k for k in T if k != "sweep"), key=lambda k: (int(k.split("/")[1], 16), int(k.split("/")[0], 16)))
    print("| hw opcode | forms in the corpus (useasm op: mnemonic) | other useasm ops the sweep encodes here |")
    print("|---|---|---|")
    byhw = collections.defaultdict(list)
    for k in keys:
        byhw[k.split("/")[1]].append(k.split("/")[0])
    sw = collections.defaultdict(list)
    for v, hws in T["sweep"].items():
        for h in hws:
            sw[f"{h:02x}"].append(v)
    for hw in sorted(set(byhw) | set(sw), key=lambda x: int(x, 16)):
        forms = []
        for op in byhw.get(hw, []):
            nm, how = N.get(f"{op}/{hw}") or N.get(op) or [f"op{op}", "guess"]
            forms.append(f"{op}: `{nm}`" + ("" if how == "corpus" else " (guess)"))
        others = sorted((v for v in sw.get(hw, []) if v not in byhw.get(hw, [])), key=lambda x: int(x, 16))
        print(f"| {hw} | {', '.join(forms) or '-'} | {' '.join(others) or '-'} |")
    print()
    for k in keys:
        e = T[k]
        op, hw = k.split("/")
        name, how = N.get(k) or N.get(op) or [f"op{op}", "guess"]
        ml, mh = e["mask"]
        vl, vh = e["value"]
        print(f"### hw {hw} / useasm {op}: `{name}` ({how})\n")
        sh = U.get(k, [])
        print(f"- in {len(sh)} corpus shaders: {', '.join(sh[:12])}{' …' if len(sh) > 12 else ''}")
        print(f"- {e['n_enc']} distinct encodings, {e['n_fuzzed']} fuzzed")
        print(f"- fixed bits: hi `{vh:08x}` under mask `{mh:08x}`, lo `{vl:08x}` under mask `{ml:08x}`")
        if e["n_fuzzed"] == 0:
            print("- no fuzzed instance: operand fields not mapped for this form\n")
            continue
        for f, m in sorted(e["flags"].items()):
            print(f"- {f}: {rng(m)}")
        for a in sorted(e["args"], key=lambda x: int(x[1:])):
            d = e["args"][a]
            bits = []
            if "num" in d:
                bits.append("number " + rng(d["num"]))
            if "bank" in d and d["bank"]["bits"]:
                tb = ", ".join(f"{TYPE.get(int(t), t)}={'/'.join(v)}" for t, v in d["bank"]["types"].items())
                bits.append(f"bank [{' '.join(d['bank']['bits'])}]: {tb}")
            if "flags" in d:
                bits.append("flags " + rng(d["flags"]))
            if "index" in d:
                allb = sorted({o for v in d["index"].values() for o in v}, key=lambda x: (x[0], int(x[1:])))
                bits.append("index (any bit) → " + "+".join(allb))
            if "seen_types" in d:
                bits.append("compiler used " + ", ".join(str(TYPE.get(t, t)) for t in sorted(d["seen_types"])))
            if bits:
                print(f"- {a}: " + "; ".join(bits))
        print()


if __name__ == "__main__":
    main(*sys.argv[1:4])
