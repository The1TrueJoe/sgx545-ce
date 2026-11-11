# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Fuzz the kernel's USSE instruction check (pvrsgx_usse.c) against the
independent decoder in tools/isa/hw.py.

    python3 -I drm/tests/usse_fuzz/usse_fuzz.py [--seed S] [-n N] [--per-form K] [--cc CC]

Builds usse_fuzz.c (the kernel's own pvrsgx_usse.c on the host), then checks
words of five classes, each alone at instruction 2 of a 512-instruction
object placed 0x100 instructions into its heap:

  uniform        N random 64-bit words
  kernel-form    N words with random bits under the kernel's own forms
  corpus-flip    N compiler words with one to four bits flipped
  corpus-window  N compiler words with one byte replaced
  oracle-<form>  K words with random bits under each of hw.py's forms
                 (ldr, str, ba, setl, lapc, phas, lock, smbo, fdual ...)

Every refusal must carry a reason. Every accepted word is decoded with
hw.py and must not be, by hw.py's reading:

  register   ldr or str
  branch     ba, setl, savl, lapc, br with link, phas with a register
             operand, a br target outside the object, or a phas.i target
             other than 0 outside it
  closed     another form the kernel fails closed on (lock, release, the
             MOE controls and smlsi's register form, imo, fdual, the forms
             the SGX545 lacks, op0x98/d7/f3/fd/fe/ff/100, op0xd6 with n or
             f3_6 set)
  operand    a c/g destination, a read past g45, i4 and up, an index
             register other than idx1/idx2, or a bank hw.py can't name
  unknown    a word hw.py has no form for

Exit status 0 when every count in those five lists is 0.
"""
import argparse
import collections
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "isa"))
import hw  # noqa: E402

FIRST, OBJ_INSTS, PLACE = 2, 512, 0x100
REASONS = ["none", "form", "operand", "kind", "branch", "phas"]

REGISTER = {"ldr", "str"}
BRANCH = {"ba", "setl", "savl", "lapc"}
CLOSED = {"lock", "release", "smbo", "smr", "smoa", "imo", "fdual", "ssum16i", "firh",
          "op16s1", "op1adrc", "op0x98", "op0xd7", "op0xf3", "op0xfd", "op0xfe", "op0xff",
          "op0x100"}


def build(cc, out):
    src = os.path.join(HERE, "usse_fuzz.c")
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-I", os.path.join(HERE, "shim"),
                    "-I", os.path.join(ROOT, "drm", "powervr_sgx"), "-o", out, src], check=True)


def ops_with_roles(x):
    """the decoded operands next to their slots' roles"""
    slots = [op for op in x.form.ops
             if not (callable(getattr(op, "when", None)) and not op.when(x.d))]
    return list(zip((op.role for op in slots), x.ops))


def judge(hi, lo):
    """(category, why) for an accepted word, or None"""
    x = hw.decode(lo, hi)
    if x is None:
        return "unknown", "no hw.py form"
    name = x.form.name
    ops = ops_with_roles(x)
    roles = {r for r, _ in ops}
    if name in REGISTER:
        return "register", name
    if name in BRANCH:
        return "branch", name
    if name == "br":
        if x.d.get("link"):
            return "branch", "br with link"
        t = FIRST + dict(ops)["target"]["n"]
        if not 0 <= t < OBJ_INSTS:
            return "branch", f"br target {t} outside the object"
    if name == "phas":
        if "pc" not in roles:
            return "branch", "phas with register operands"
        pc = dict(ops)["pc"]["n"]
        if pc and not PLACE <= pc < PLACE + OBJ_INSTS:
            return "branch", f"phas.i target {pc:#x} outside the object"
    if name in CLOSED:
        return "closed", name
    if name == "smlsi" and "v0" not in roles:
        return "closed", "smlsi register form"
    if name == "op0xd6" and (dict(ops)["n"].get("n") or x.d.get("f3_6")):
        return "closed", "op0xd6 with n or f3_6"
    for role, o in ops:
        kind, n = o.get("kind", "?"), o.get("n")
        if kind.startswith("?"):
            return "operand", f"{role} bank {kind}"
        if role == "dst" and kind in ("c", "g"):
            return "operand", f"{kind} destination"
        if kind == "g" and n is not None and n >= 46:
            return "operand", f"g{n}"
        if kind == "i" and n is not None and n > 3:
            return "operand", f"i{n}"
        if kind == "idx" and n not in (1, 2):
            return "operand", f"idx{n}"
    return None


def parse(out, err, counts, accepted):
    for line in err.splitlines():
        f = line.split()
        if f and f[0] == "C":
            counts[f[1]] = [int(v) for v in f[2:]]
    for line in out.splitlines():
        f = line.split()
        if f and f[0] == "A":
            accepted.append((f[1], int(f[2], 16), int(f[3], 16)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("-n", type=int, default=200000)
    ap.add_argument("--per-form", type=int, default=2000)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "usse_fuzz")
        build(args.cc, exe)
        counts, accepted = {}, []
        r = subprocess.run([exe, "gen", str(args.seed), str(args.n)], capture_output=True,
                           text=True, check=True)
        parse(r.stdout, r.stderr, counts, accepted)

        rnd = random.Random(args.seed)
        lines = []
        for f in hw.FORMS:
            for _ in range(args.per_form):
                v = f.fval | rnd.getrandbits(64) & ~f.fmask
                lines.append(f"{v >> 32:08x} {v & 0xffffffff:08x} oracle-{f.name}\n")
        r = subprocess.run([exe, "scan"], input="".join(lines), capture_output=True, text=True,
                           check=True)
        parse(r.stdout, r.stderr, counts, accepted)

    oracle = collections.defaultdict(lambda: [0] * 8)
    print(f"{'class':<16} {'words':>9} {'accepted':>9}  " +
          " ".join(f"{w:>8}" for w in REASONS))
    for name in sorted(counts, key=lambda c: (c.startswith("oracle-"), c)):
        c = counts[name]
        if name.startswith("oracle-"):
            oracle[name] = [a + b for a, b in zip(oracle[name], c)]
            continue
        print(f"{name:<16} {c[0]:>9} {c[1]:>9}  " + " ".join(f"{v:>8}" for v in c[2:]))
    tot = [sum(c[i] for c in oracle.values()) for i in range(8)]
    print(f"{'oracle-*':<16} {tot[0]:>9} {tot[1]:>9}  " + " ".join(f"{v:>8}" for v in tot[2:]))

    bad = collections.defaultdict(list)
    for cls, hi, lo in accepted:
        v = judge(hi, lo)
        if v:
            bad[v[0]].append((cls, hi, lo, v[1]))

    no_reason = sum(c[2] for c in counts.values())
    print(f"\nrefusals without a reason: {no_reason}")
    print(f"accepted words decoded by hw.py: {len(accepted)}")
    for cat in ("register", "branch", "closed", "operand", "unknown"):
        print(f"  {cat:<9} {len(bad[cat])}")
        for cls, hi, lo, why in bad[cat][:8]:
            x = hw.decode(lo, hi)
            print(f"      {hi:08x} {lo:08x}  {why}  ({cls}: {hw.fmt(x) if x else '-'})")

    # Where the operand disagreements are: form, slot role, file, format bits.
    where = collections.Counter()
    for cls, hi, lo, why in bad["operand"]:
        x = hw.decode(lo, hi)
        role = next((r for r, o in ops_with_roles(x) if why.endswith(f"{o.get('kind')}{o.get('n')}")
                     or why.startswith(r)), "?")
        where[(x.form.name, role, why.rstrip("0123456789"), "fmt" if x.d.get("fmt") else "-")] += 1
    if where:
        print("\noperand disagreements by form, slot, file, format select:")
        for (name, role, kind, fmt), n in where.most_common():
            print(f"  {n:>7}  {name:<10} {role:<6} {kind:<5} {fmt}")

    # The oracle forms the kernel accepts at all, for the record.
    taken = collections.Counter(cls for cls, _, _ in accepted if cls.startswith("oracle-"))
    never = sorted(n[7:] for n, c in oracle.items() if not c[1])
    print(f"\nhw.py forms the kernel accepted some word of: {len(taken)} of {len(oracle)}")
    print(f"never accepted: {' '.join(never)}")
    return 1 if no_reason or any(bad.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
