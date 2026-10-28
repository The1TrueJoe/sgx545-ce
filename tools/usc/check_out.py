"""Check every instruction sgxc emitted (the USP code blocks, branch blocks
and smp templates of OUTDIR/*.usp) three ways:

1. hw.decode() finds a form and no bit it can't explain;
2. its text (dis.py's syntax) assembles back to the same 64 bits;
3. libusc's own encoder (oracle.py, run in unicorn) gives the same 64 bits
   for the USEASM instruction those fields describe (branches carry a
   label the finalizer fills in, so they're checked with the raw target).

    LIBUSC=<libusc.so.1.7.862890> <venv>/bin/python -I usc/check_out.py OUTDIR [--no-oracle]
"""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ISA = os.path.join(HERE, "..", "isa")
sys.path.insert(0, ISA)
import hw  # noqa: E402
import usp  # noqa: E402

RT = {"r": 0, "o": 1, "pa": 2, "sa": 3, "c": 6, "g": 5, "i": 7, "#": 8}
PRED_TO_U = {"": 0, "p0": 1, "p1": 2, "!p0": 3, "!p1": 4, "p2": 5, "p3": 6, "pn": 7}
SCALAR = {"frcp": 4, "frsq": 5, "flog2": 6, "fexp2": 7, "fsqrt": 8, "fsin": 9, "fcos": 0xA}
FLOAT3 = {"fmad": 0, "fadm": 1, "fmsa": 2, "fsubflr": 3, "fdp": 0xB}
TALU = {"fadd": 0xA6, "fsub": 0xA9}
PCKF = ["u8", "s8", "o8", "u16", "s16", "f16", "f32", "c10"]


def arg(o, flags=0):
    from uinst import A
    k = o["kind"]
    if k not in RT:
        raise KeyError(f"operand kind {k}")
    f = flags | (1 if o.get("neg") else 0) | (2 if o.get("abs") else 0)
    n = o["n"]
    for s in o.get("sfx", []):
        if len(s) == 2 and s[0] == "l":
            f |= int(s[1]) << 5
    if o.get("f16"):
        f |= 1 << 22                 # USEASM's F16 format flag; the number counts halves
        n = 2 * n + o.get("half", 0)
    return A(k, n, f)


def common(x):
    d = x.d
    f1 = (d.get("skipinv", 0) << 0) | (d.get("syncs", 0) << 2) | (d.get("nosched", 0) << 3) | \
         (d.get("end", 0) << 17)
    if "pred" in d:
        f1 |= PRED_TO_U[hw.PRED3[d["pred"]]] << 13
    return f1


def useasm(x):
    """the USEASM instruction (uinst.U) for a decoded instruction, or None"""
    from uinst import A, U
    n, d, ops = x.form.name, x.d, x.ops
    f1 = common(x)
    f2 = 0
    if n in FLOAT3 or n in ("fminmax", "fmaxmin"):
        if d.get("moe", 0):
            f1 |= d.get("rpt", 1) << 4
        else:
            f1 |= d.get("rpt", 1) << 4
            f2 = 0x10000000 | d.get("inc0", 0) << 29 | d.get("inc1", 0) << 30 | d.get("inc2", 0) << 31
        if n == "fsubflr":     # USEASM 0x3 is the one-source form (a, a), 0xcf takes two
            return [U(op, *[arg(o) for o in ops], f1=f1, f2=f2) for op in (0xCF, 0x3)]
        if n in FLOAT3:
            return [U(FLOAT3[n], *[arg(o) for o in ops], f1=f1, f2=f2)]
        return [U(op, *[arg(o) for o in ops], f1=f1, f2=f2) for op in (0x11, 0x12)]
    if n in SCALAR or n == "mov" or n == "movc" or n == "pck":
        if d.get("rcnt"):
            f1 |= d["rpt"] << 4
        else:
            f1 |= d.get("mask", 0) << 9
    if n in SCALAR:
        fl = (1 << 22 | 2 * d.get("chan", 0) << 5) if d.get("f16") else 0
        return [U(SCALAR[n], arg(ops[0]), arg(ops[1], fl), f1=f1, f2=(1 << 22) if d.get("f16d") else 0)]
    if n == "mov":
        return [U(0xA3, *[arg(o) for o in ops], f1=f1)]
    if n == "movc":
        ty = x.form.fval >> 40 & 7      # h8-10
        f1x, f2t = {1: (2, 0), 2: (0, 1 << 19), 3: (0, 1 << 20), 4: (0, 1 << 21), 5: (0, 1 << 23)}[ty]
        test = {0: 0x14, 1: 0x18, 2: 0x19, 3: 0x6}[d["cond"]]
        return [U(0xF2, *[arg(o) for o in ops], f1=f1 | 0x40000000 | f1x, f2=f2t, test=test)]
    if n == "pck":
        dfmt = PCKF[x.form.fval >> 38 & 7]
        sfmt = PCKF[x.form.fval >> 41 & 7]
        base = {("u8", "f32"): 0x20, ("f32", "u8"): 0x34, ("u8", "f16"): 0x21, ("f16", "u8"): 0x3B,
                ("f16", "c10"): 0x3E, ("f16", "f16"): 0x15}.get((dfmt, sfmt))
        if base is None:
            return None
        if (dfmt, sfmt) == ("f16", "f16"):
            # 0x15 (pack half) and 0x38 (unpack half) share the form; float -> float takes rnd
            a = [arg(ops[0], d.get("dmask", 15) << 7), arg(ops[1]), arg(ops[2]),
                 A(0x10, {0: 0x3a, 1: 0x39, 2: 0x3b, 3: 0}[d.get("rnd", 0)])]
            return [U(op, *a, f1=f1, f2=f2) for op in (0x15, 0x38)]
        a = [arg(ops[0], d.get("dmask", 15) << 7), arg(ops[1]), arg(ops[2])]
        if d.get("scale"):
            f2 |= 0x100
            a.append(A(0x10, {0: 0x3a, 1: 0x39, 2: 0x3b, 3: 0}[d.get("rnd", 0)]))
        return [U(base, *a, f1=f1, f2=f2)]
    if n in ("test", "testmask"):
        alu = hw.FORMS and x.form.fields
        alu = [fl for fl in x.form.fields if fl.name == "alu"][0].table[d["alu"]]
        op = TALU.get(alu)
        if op is None:
            return None
        f1 |= 1 << 30 | d.get("mask", 1) << 9
        if n == "test":
            test = d["cond"] | d.get("chan", 0) << 5
            return [U(op, A("c", 0, 0x10000), A(0xC, ops[0]["n"]), arg(ops[1]), arg(ops[2]), f1=f1, test=test)]
        test = d["cond"] | (d["type"] + 1) << 9
        return [U(op, *[arg(o) for o in ops], f1=f1, test=test)]
    if n == "fmad16":
        f1 |= 0x200 if d.get("rpt", 1) == 1 else d["rpt"] << 4
        a = [arg(ops[0])] + [arg(o, d.get(f"swz{j}", 0) << 25) for j, o in enumerate(ops[1:])]
        return [U(0x10, *a, f1=f1)]
    if n == "limm":
        return [U(0x9B, arg(ops[0]), A(8, ops[1]["n"]), f1=f1)]
    if n == "nop":
        return [U(0xF1, f1=f1)]
    if n == "br":
        return [U(0x96, A(0x15, ops[0]["n"] & 0x3ffff), f1=f1)]
    if n == "smp":
        dim = d["dim"]
        lod = {"": 0}.get("", 0)
        mn = hw.fmt(x).split()[0].split(")")[-1]
        lod = 1 if ".bias" in mn else 2 if ".lod" in mn else 0
        op = 0x6F + dim + 3 * lod
        a = [arg(o) for o in ops]
        return [U(op, *a, A(0xA, d.get("drc", 0)), A(0x10, 0x15), f1=f1 | 0x200)]
    return None


def words(path):
    s = usp.load(path)
    out = []
    for b in s["blocks"]:
        if b["type"] in ("code", "branch"):
            out += [(lo, hi, b["type"]) for lo, hi in b["insts"]]
        elif b["type"] == "sample5" and not b["sample"]["nondep"]:
            lo, hi = b["sample"]["smp"]
            out.append((lo, hi, "smp template"))
    return out


def main():
    oracle = "--no-oracle" not in sys.argv
    if oracle:
        from uinst import try_enc
    d = [a for a in sys.argv[1:] if not a.startswith("--")][0]
    n = bad = n_or = skipped = 0
    forms = {}
    for p in sorted(glob.glob(os.path.join(d, "*.usp"))):
        for lo, hi, where in words(p):
            n += 1
            x = hw.decode(lo, hi)
            name = os.path.basename(p)
            if x is None or x.unknown:
                bad += 1
                print(f"{name}: {hi:08x} {lo:08x}: {'no form' if x is None else 'unknown bits ' + str(x.unknown)}")
                continue
            t = hw.fmt(x)
            forms[x.form.name] = forms.get(x.form.name, 0) + 1
            try:
                if hw.encode(hw.parse(t)) != (lo, hi):
                    raise ValueError("re-encodes differently")
            except ValueError as e:
                bad += 1
                print(f"{name}: {hi:08x} {lo:08x} {t}: {e}")
                continue
            if not oracle:
                continue
            us = useasm(x)
            if us is None:
                skipped += 1
                continue
            got = [try_enc(u) for u in us]
            if (lo, hi) in got:
                n_or += 1
            else:
                bad += 1
                g = got[0]
                print(f"{name}: {hi:08x} {lo:08x} {t}: libusc encodes "
                      f"{'(rejected)' if g is None else f'{g[1]:08x} {g[0]:08x}'}")
    print(f"{n} instructions: {n - bad} decode with no unknown bit and round-trip"
          + (f"; {n_or} identical from libusc's encoder, {skipped} forms with no USEASM mapping here" if oracle else ""))
    print("forms: " + ", ".join(f"{k} {v}" for k, v in sorted(forms.items())))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
