"""Check hw.py against the corpora: every instruction must decode to a form,
leave no bit unexplained, and re-encode from its text to the same 64 bits.

  python3 hwcheck.py [corpus dirs...]        (default: corpus/c1 corpus/c2 corpus/c3)
"""
import collections
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw  # noqa: E402
import usp  # noqa: E402


def insts(dirs):
    for d in dirs:
        for p in sorted(glob.glob(os.path.join(d, "*.usp"))):
            s = usp.load(p)
            for lo, hi in s["pre"]:
                yield p, lo, hi
            for b in s["blocks"]:
                for lo, hi in b.get("insts", []):
                    yield p, lo, hi


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    dirs = argv or [os.path.join(here, "corpus", c) for c in ("c1", "c2", "c3")]
    n = 0
    nof = collections.Counter()
    unk = collections.Counter()
    bad = collections.Counter()
    ex = {}
    seen = set()
    for p, lo, hi in insts(dirs):
        n += 1
        if (lo, hi) in seen:
            continue
        seen.add((lo, hi))
        x = hw.decode(lo, hi)
        op = hi >> 27
        if x is None:
            nof[op] += 1
            ex.setdefault(("noform", op), (p, lo, hi))
            continue
        if x.unknown:
            unk[(x.form.name, " ".join(x.unknown))] += 1
            ex.setdefault(("unk", x.form.name, " ".join(x.unknown)), (p, lo, hi))
        t = hw.fmt(x)
        try:
            lo2, hi2 = hw.encode(hw.parse(t))
            if (lo2, hi2) != (lo, hi):
                bad[x.form.name] += 1
                ex.setdefault(("rt", x.form.name), (p, lo, hi, t, f"{hi2:08x} {lo2:08x}"))
        except Exception as e:
            bad[x.form.name] += 1
            ex.setdefault(("rt", x.form.name), (p, lo, hi, t, str(e)))
    print(f"{n} instructions, {len(seen)} distinct")
    print("no form (hw opcode: count):", dict(sorted((f"{k:02x}", v) for k, v in nof.items())))
    print("unexplained bits:", sum(unk.values()))
    for k, v in unk.most_common(40):
        print(f"  {v:4} {k[0]} {{{k[1]}}}")
    print("round-trip failures:", dict(bad))
    for k, v in ex.items():
        if k[0] == "rt":
            print("  ", k[1], v[0].split("/")[-1], f"{v[2]:08x} {v[1]:08x}", v[3], "->", v[4])
    return 1 if (nof or unk or bad) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
