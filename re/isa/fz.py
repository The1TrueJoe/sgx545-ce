"""Read an encfuzz log: base instructions and the mutations that changed them.

  python3 fz.py <log> [opcode...]   per USEASM opcode, which input bits move which output bits

Input dwords: 0 opcode, 1-3 flags, 4 test, then args of 5 dwords each from
dword 5: (number, type, flags, index, ?). Output bits are named l0..l31 (low
word) and h0..h31 (high word)."""
import collections
import sys

ARGF = ["num", "type", "flags", "index", "x"]


def inname(k):
    if k < 5:
        return ["op", "f1", "f2", "f3", "test"][k]
    a, f = divmod(k - 5, 5)
    return f"a{a}.{ARGF[f]}"


def parse(path):
    """yield (base words, (ret, lo, hi), [(k, bit|None, value|None, ret, lo, hi)])"""
    base = out = None
    muts = None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == "I":
            eq = t.index("=")
            base = [int(x, 16) for x in t[1:eq]]
            out = (int(t[eq + 1]), int(t[eq + 2], 16), int(t[eq + 3], 16))
        elif t[0] == "F":
            muts = []
        elif t[0] == "M" and muts is not None:
            k = int(t[1])
            if t[2].startswith("v="):
                b, v = None, int(t[2][2:])
            else:
                b, v = int(t[2]), None
            muts.append((k, b, v, int(t[4]), int(t[5], 16), int(t[6], 16)))
        elif t[0] == "E" and muts is not None:
            yield base, out, muts
            muts = None


def bits(x, pre):
    return [f"{pre}{i}" for i in range(32) if x >> i & 1]


def ranges(m):
    """{input bit: frozenset(output bits)} -> compact text"""
    out, run = [], None
    for b in sorted(m):
        o = m[b]
        one = next(iter(o)) if len(o) == 1 else None
        if one and run and b == run[1] + 1 and one[0] == run[3][0] and int(one[1:]) == int(run[3][1:]) + 1:
            run = (run[0], b, run[2], one)
            continue
        if run:
            out.append(run)
        run = (b, b, one, one) if one else None
        if not one:
            out.append((b, b, None, " ".join(sorted(o, key=lambda x: (x[0], int(x[1:]))))))
    if run:
        out.append(run)
    txt = []
    for b0, b1, o0, o1 in out:
        if o0 is None:
            txt.append(f"b{b0}:{o1}")
        elif b0 == b1:
            txt.append(f"b{b0}:{o0}")
        else:
            txt.append(f"b{b0}-{b1}:{o0}-{o1}")
    return "; ".join(txt)


def summary(path):
    """per USEASM opcode: union over fuzzed instances of input bit -> output bits"""
    agg = collections.defaultdict(lambda: collections.defaultdict(lambda: collections.defaultdict(set)))
    sweep = collections.defaultdict(dict)
    tsweep = collections.defaultdict(lambda: collections.defaultdict(dict))
    n = collections.Counter()
    for base, (r, lo, hi), muts in parse(path):
        op = base[0]
        n[op] += 1
        for k, b, v, r2, lo2, hi2 in muts:
            if b is None:
                if r2 > 0 and k == 0:
                    sweep[op].setdefault(v, (hi2, lo2, hi, lo))
                elif r2 > 0:
                    # arg type v: keep the whole output, with the base, per arg
                    tsweep[op][(k - 6) // 5].setdefault(v, []).append((hi2, lo2, hi, lo, base[k]))
                continue
            if r2 != r:
                continue
            ch = frozenset(bits(lo ^ lo2, "l") + bits(hi ^ hi2, "h"))
            if ch:
                agg[op][k][b] |= ch
    return n, agg, sweep, tsweep


def main_summary(path):
    n, agg, sweep, tsweep = summary(path)
    for op in sorted(agg):
        print(f"== useasm op {op:#x} ({n[op]} instances)")
        for k in sorted(agg[op]):
            m = {b: frozenset(v) for b, v in agg[op][k].items()}
            # an index field: every bit maps to the same thing
            if len(m) > 8 and len({v for v in m.values()}) <= 2:
                m = {0: frozenset().union(*m.values())}
                print(f"   {inname(k):10} any bit: {ranges(m).split(':', 1)[1]}")
                continue
            print(f"   {inname(k):10} {ranges(m)}")
        for a in sorted(tsweep[op]):
            t = tsweep[op][a]
            # the bits any type change moves are this arg's bank (and maybe number) bits
            moved_h = moved_l = 0
            for v, lst in t.items():
                for hi2, lo2, hi, lo, bt in lst:
                    moved_h |= hi ^ hi2
                    moved_l |= lo ^ lo2
            names = bits(moved_l, "l") + bits(moved_h, "h")

            def val(hi2, lo2):
                return "".join(str((lo2 if n[0] == "l" else hi2) >> int(n[1:]) & 1) for n in names)
            # the base instances' own types too
            seen = {}
            for v, lst in t.items():
                for hi2, lo2, hi, lo, bt in lst:
                    seen.setdefault(v, set()).add(val(hi2, lo2))
                    seen.setdefault(bt, set()).add(val(hi, lo))
            print(f"   a{a}.type [{' '.join(names)}] " + " ".join(f"{v}:{'/'.join(sorted(x))}" for v, x in sorted(seen.items())))


def main():
    if sys.argv[1] == "-s":
        return main_summary(sys.argv[2])
    want = {int(x, 0) for x in sys.argv[2:]}
    for base, (r, lo, hi), muts in parse(sys.argv[1]):
        if want and base[0] not in want:
            continue
        print(f"== op {base[0]:#x} f1 {base[1]:x} f2 {base[2]:x} f3 {base[3]:x} -> {hi:08x} {lo:08x}")
        args = [base[5 + 5 * i:9 + 5 * i] for i in range(7)]
        print("   args", " ".join(f"({a[0]:x},{a[1]},{a[2]:x},{a[3]:x})" for a in args))
        per = collections.defaultdict(list)
        for k, b, v, r2, lo2, hi2 in muts:
            if b is None:
                continue
            ch = bits(lo ^ lo2, "l") + bits(hi ^ hi2, "h")
            per[k].append((b, " ".join(ch) if r2 == r else f"ret{r2} " + " ".join(ch)))
        for k in sorted(per):
            print(f"   {inname(k):10}", "; ".join(f"b{b}:{c}" for b, c in per[k]))
        ops = [(v, hi2, lo2) for k, b, v, r2, lo2, hi2 in muts if b is None]
        print("   opsweep", " ".join(f"{v:#x}={h >> 27:02x}" for v, h, l in ops))


if __name__ == "__main__":
    main()
