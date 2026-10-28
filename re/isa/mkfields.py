"""Build fields.json (the disassembler's tables) from an encfuzz log.

  python3 mkfields.py <encfuzz log> > fields.json

For every USEASM opcode the log has instances of (or reached by the opcode
sweep), it records:
  pattern   (mask, value) over the 64 bits: bits that never changed across the
            opcode's encodings and that no input field moves
  args      per argument: the output bits its register number moves (in
            number-bit order), its bank bits and the bank value each register
            type encodes to, and the output bits each of its flag bits sets
  flags     op-flag (f1, f2, f3, test) input bits -> output bits
Everything here is what libusc's encoder did with our inputs; nothing is
assumed about the ISA."""
import collections
import json
import sys

import fz


def outbits(lo, hi):
    return [f"l{i}" for i in range(32) if lo >> i & 1] + [f"h{i}" for i in range(32) if hi >> i & 1]


def getbit(lo, hi, name):
    return ((lo if name[0] == "l" else hi) >> int(name[1:])) & 1


def main(path):
    # keyed by (USEASM opcode, hardware opcode): one USEASM op can encode to
    # more than one hardware form (e.g. with the test flag)
    enc = collections.defaultdict(set)       # key -> {(lo, hi)} real instances
    moved = collections.defaultdict(lambda: collections.defaultdict(lambda: collections.defaultdict(set)))
    tsw = collections.defaultdict(lambda: collections.defaultdict(dict))
    bases = collections.defaultdict(list)
    sweep = collections.defaultdict(set)     # USEASM op -> hardware opcodes the sweep reached
    inst = collections.defaultdict(set)      # key -> {(input dwords 0..39, lo, hi)}
    for base, (r, lo, hi), muts in fz.parse(path):
        key = (base[0], hi >> 27)
        bases[key].append((base, lo, hi))
        for k, b, v, r2, lo2, hi2 in muts:
            if r2 <= 0:
                continue
            if b is None and k == 0:
                sweep[v].add(hi2 >> 27)
                continue
            if b is None:
                tsw[key][(k - 6) // 5].setdefault(v, set()).add((lo2, hi2, lo, hi, base[k]))
                continue
            if r2 == r:
                for o in outbits(lo ^ lo2, hi ^ hi2):
                    moved[key][k][b].add(o)
    for line in open(path):
        t = line.split()
        if t and t[0] == "I":
            eq = t.index("=")
            if int(t[eq + 1]) > 0:
                lo, hi = int(t[eq + 2], 16), int(t[eq + 3], 16)
                enc[(int(t[1], 16), hi >> 27)].add((lo, hi))
                inst[(int(t[1], 16), hi >> 27)].add((tuple(int(x, 16) for x in t[1:41]), lo, hi))
    out = {}
    for key in sorted(enc):
        op = key
        e = enc[key]
        lo0, hi0 = next(iter(e))
        const_l, const_h = 0xffffffff, 0xffffffff
        for lo, hi in e:
            const_l &= ~(lo ^ lo0)
            const_h &= ~(hi ^ hi0)
        fl = fh = 0
        for k in moved[op]:
            for b in moved[op][k]:
                for o in moved[op][k][b]:
                    if o[0] == "l":
                        fl |= 1 << int(o[1:])
                    else:
                        fh |= 1 << int(o[1:])
        for a in tsw[op]:
            for v, s in tsw[op][a].items():
                for lo2, hi2, lo, hi, bt in s:
                    fl |= lo ^ lo2
                    fh |= hi ^ hi2
        ml, mh = const_l & ~fl, const_h & ~fh
        mh |= 0xf8000000                     # the hardware opcode is always fixed
        ent = {"n_enc": len(e), "n_fuzzed": sum(1 for _ in bases[op]),
               "mask": [ml, mh], "value": [lo0 & ml, hi0 & mh], "args": {}, "flags": {}}
        for k in sorted(moved[op]):
            name = fz.inname(k)
            m = {b: sorted(v, key=lambda x: (x[0], int(x[1:]))) for b, v in moved[op][k].items()}
            if k < 5:
                ent["flags"][name] = m
                continue
            a, f = name.split(".")
            ent["args"].setdefault(a, {})[f] = m
        for a in sorted(tsw[op]):
            s = tsw[op][a]
            ml_ = mh_ = 0
            for v, lst in s.items():
                for lo2, hi2, lo, hi, bt in lst:
                    ml_ |= lo ^ lo2
                    mh_ |= hi ^ hi2
            names = outbits(ml_, mh_)
            table = {}
            for v, lst in s.items():
                for lo2, hi2, lo, hi, bt in lst:
                    table.setdefault(v, set()).add("".join(str(getbit(lo2, hi2, n)) for n in names))
                    if bt < 64:         # an unused argument slot holds stack garbage
                        table.setdefault(bt, set()).add("".join(str(getbit(lo, hi, n)) for n in names))
            ent["args"].setdefault(f"a{a}", {})["bank"] = {
                "bits": names, "types": {str(t): sorted(x) for t, x in sorted(table.items())}}
        # register types each arg had in real instances (what the compiler uses)
        for base, lo, hi in bases[op]:
            for a in range(7):
                ty = base[6 + 5 * a]
                if ty < 64:
                    ent["args"].setdefault(f"a{a}", {}).setdefault("seen_types", [])
                    if ty not in ent["args"][f"a{a}"]["seen_types"]:
                        ent["args"][f"a{a}"]["seen_types"].append(ty)
        # output bits the flips never reached: an input bit equal to it in every
        # real instance, with both values seen, is recorded as a correlation
        known = set()
        for a in ent["args"].values():
            for f in ("num", "flags", "index"):
                for outs in a.get(f, {}).values():
                    known.update(outs)
            known.update(a.get("bank", {}).get("bits", []))
        for m in ent["flags"].values():
            for outs in m.values():
                known.update(outs)
        corr = {}
        ins = list(inst[key])
        if len(ins) >= 3:
            for ob in [f"l{i}" for i in range(32)] + [f"h{i}" for i in range(32)]:
                if ob in known or (ob[0] == "l" and ml >> int(ob[1:]) & 1) or (ob[0] == "h" and mh >> int(ob[1:]) & 1):
                    continue
                ov = [getbit(lo, hi, ob) for w, lo, hi in ins]
                if len(set(ov)) < 2:
                    continue
                for k in range(1, 40):
                    if 6 <= k and (k - 5) % 5 in (0, 1):
                        continue        # numbers and types are mapped by the flips
                    for b in range(32):
                        if all((w[k] >> b & 1) == v for (w, lo, hi), v in zip(ins, ov)):
                            corr.setdefault(ob, []).append(f"{fz.inname(k)}.b{b}")
        ent["corr"] = corr
        out[f"{key[0]:#x}/{key[1]:02x}"] = ent
    out["sweep"] = {f"{v:#x}": sorted(hw) for v, hw in sorted(sweep.items())}
    json.dump(out, sys.stdout, indent=1, sort_keys=True)


if __name__ == "__main__":
    main(sys.argv[1])
