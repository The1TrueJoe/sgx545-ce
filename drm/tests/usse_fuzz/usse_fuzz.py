# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Fuzz the kernel's USSE instruction check (pvrsgx_usse.c) against an
independent decoder: sgx-disasm, built from mesa/src/sgx/isa/sgx.xml.

    python3 -I drm/tests/usse_fuzz/usse_fuzz.py [--seed S] [-n N] [--per-form K] [--cc CC]

sgx-disasm is found through SGX_DISASM or SGX_ASM (its sibling), else on PATH;
`sh ukernel/sgx-asm.sh DIR` builds it. Builds usse_fuzz.c (the kernel's own
pvrsgx_usse.c on the host), then checks words of five classes, each alone at
instruction 2 of a 512-instruction object placed 0x100 instructions into its
heap:

  uapi-corpus    the uAPI test's corpus (drm/tests/usse_corpus.h): all must pass
  uniform        N random 64-bit words
  kernel-form    N words with random bits under the kernel's own forms
  corpus-flip    N compiler words with one to four bits flipped
  corpus-window  N compiler words with one byte replaced
  oracle-<form>  K words with random bits under each of sgx.xml's instruction
                 bitsets (its fixed pattern bits kept: ldr, str, ba, setl, lapc,
                 phas, lock, smbo, fdual ...)

Every refusal must carry a reason. Every accepted word is disassembled, and
must not be, by sgx-disasm's reading:

  register   ldr or str
  branch     ba, setl, savl, lapc, br with link, phas with register
             operands, a br target outside the object, or a phas.i target
             other than 0 outside it
  closed     another form the kernel fails closed on (lock, release, the
             MOE controls and smlsi's register form, imo, fdual, the forms
             the SGX545 lacks, op0x98/d7/f3/fd/fe/ff/100, op0xd6 with n or
             f3_6 set, setfc other than #0, #1)
  operand    a c/g destination, a read past g45, i4 and up, an index
             register other than idx1/idx2, or an operand sgx-disasm can't name
  unknown    a word that matches none of sgx.xml's forms

(A word with bits its form leaves unused is judged by the fields it has, and
counted.)

The destination slots come from sgx.xml's display templates. Exit status 0
when every count in those five lists is 0 and the whole uapi-corpus class
passes.
"""
import argparse
import collections
import os
import random
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "isa"))
import asm  # noqa: E402

SGX_XML = os.path.join(ROOT, "mesa", "src", "sgx", "isa", "sgx.xml")
FIRST, OBJ_INSTS, PLACE = 2, 512, 0x100
REASONS = ["none", "form", "operand", "kind", "branch", "phas"]

REGISTER = {"ldr", "str"}
BRANCH = {"ba", "setl", "savl", "lapc"}
CLOSED = {"lock", "release", "smbo", "smr", "smoa", "imo", "fdual", "ssum16i", "firh",
          "op16s1", "op1adrc", "op0x98", "op0xd7", "op0xf3", "op0xfd", "op0xfe", "op0xff",
          "op0x100", "smbo.r", "smr.r", "smoa.r"}
DST_ROLES = {"DST", "IDST", "DN"}
# a template piece's operand is its first field that isn't a modifier
_MODIFIER = re.compile(r"S\dABS|S\dNEG|S2NEG|NEG\d|L\d|PW|PRED2?|LPRED")


class Spec:
    """sgx.xml's instruction bitsets: the fixed bits of each, and per printed
    mnemonic the operand roles of its display templates"""

    def __init__(self, path=SGX_XML):
        root = ET.parse(path).getroot()
        self.bs = {b.get("name"): b for b in root.iter("bitset")}
        self.tmpl = {t.get("name"): t.text.strip() for t in root.iter("template")}
        self.forms = []            # (name, fixed mask, fixed value)
        self.roles = collections.defaultdict(list)   # mnemonic prefix -> [roles]
        for name, b in self.bs.items():
            if name.startswith("#"):
                continue
            chain = self.chain(b)
            if chain[-1].get("name") != "#instruction":
                continue
            m = v = 0
            for c in chain:
                for p in c.findall("pattern"):
                    lo = int(p.get("low", p.get("pos")))
                    bits = p.text.strip()[::-1]
                    for i, ch in enumerate(bits):
                        if ch in "01":
                            m |= 1 << (lo + i)
                            v |= int(ch) << (lo + i)
            self.forms.append((name, m, v))
            for c in chain:
                ds = [d.text for o in c.findall("override") for d in o.findall("display")]
                ds += [d.text for d in c.findall("display")]
                if ds:
                    for d in ds:
                        prefix, roles = self.template_roles(d, name)
                        self.roles[prefix].append(roles)
                    break

    def chain(self, b):
        out = [b]
        while out[-1].get("extends"):
            out.append(self.bs[out[-1].get("extends")])
        return out

    def expand(self, t):
        return re.sub(r"\{(\w+)\}", lambda m: self.expand(self.tmpl[m.group(1)])
                      if m.group(1) in self.tmpl else m.group(0), t)

    def template_roles(self, d, name):
        t = self.expand(" ".join(d.split()))
        t = re.sub(r"\{(PRED2?|LPRED)\}", "", t).strip()
        t = t.split(" [", 1)[0]
        mn, _, rest = t.partition(" ")
        prefix = re.split(r"\{", mn.replace("{NAME}", name), maxsplit=1)[0]
        roles = []
        for piece in [x for x in rest.split(", ") if x.strip()]:
            fs = [f for f in re.findall(r"\{(\w+)\}", piece) if not _MODIFIER.fullmatch(f)]
            roles.append(fs[0] if fs else piece)
        return prefix, roles

    def operand_roles(self, x):
        """the roles of a parsed instruction's operands (None if no template fits)"""
        mn = ".".join([x.name] + x.mods)
        best = None
        for prefix, cands in self.roles.items():
            if mn == prefix or mn.startswith(prefix + "."):
                for roles in cands:
                    if len(roles) == len(x.ops) and (best is None or len(prefix) > best[0]):
                        best = (len(prefix), roles)
        return best[1] if best else None


def build(cc, out):
    src = os.path.join(HERE, "usse_fuzz.c")
    subprocess.run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-I", os.path.join(HERE, "shim"),
                    "-I", os.path.join(ROOT, "drm", "powervr_sgx"), "-o", out, src], check=True)


def disasm(words):
    """sgx-disasm's text per (hi, lo), each read as if alone: a word after a
    setfc that changes the format control is disassembled again by itself"""
    texts = asm.disasm([(lo, hi) for hi, lo in words])
    for i in range(1, len(texts)):
        if texts[i - 1].startswith("setfc") and not texts[i - 1].startswith("setfc #0, #1"):
            hi, lo = words[i]
            texts[i] = asm.disasm([(lo, hi)])[0]
    return texts


def judge(spec, text):
    """(category, why) for an accepted word's disassembly, or None"""
    if not asm.matched(text):
        return "unknown", "sgx-disasm has no form"
    try:
        x = asm.parse(asm.body(text))
    except ValueError as e:
        return "operand", str(e)
    name = x.name
    mn = ".".join([name] + x.mods)
    ops = x.ops
    if name in REGISTER:
        return "register", name
    if name in BRANCH:
        return "branch", name
    if name == "br":
        if x.flags.get("link"):
            return "branch", "br with link"
        t = FIRST + ops[0]["n"]
        if not 0 <= t < OBJ_INSTS:
            return "branch", f"br target {t} outside the object"
    if mn == "phas":
        return "branch", "phas with register operands"
    if mn.startswith("phas.i"):
        pc = ops[0]["n"]
        if pc and not PLACE <= pc < PLACE + OBJ_INSTS:
            return "branch", f"phas.i target {pc:#x} outside the object"
    if name in CLOSED or mn in CLOSED:
        return "closed", mn
    if mn == "smlsi.r":
        return "closed", "smlsi register form"
    if name == "op0xd6" and (ops[0].get("n") or x.flags.get("f3_6")):
        return "closed", "op0xd6 with n or f3_6"
    if name == "setfc" and (ops[0]["n"], ops[1]["n"]) != (0, 1):
        return "closed", "setfc other than #0, #1"
    roles = spec.operand_roles(x)
    if roles is None:
        return "unknown", f"no sgx.xml template for {mn} with {len(ops)} operands"
    for role, o in zip(roles, ops):
        kind, n = o.get("kind", "?"), o.get("n")
        if role in DST_ROLES and kind in ("c", "g"):
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

        spec = Spec()
        rnd = random.Random(args.seed)
        lines = []
        for name, fmask, fval in spec.forms:
            for _ in range(args.per_form):
                v = fval | rnd.getrandbits(64) & ~fmask
                lines.append(f"{v >> 32:08x} {v & 0xffffffff:08x} oracle-{name}\n")
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
    texts = disasm([(hi, lo) for _, hi, lo in accepted])
    for (cls, hi, lo), text in zip(accepted, texts):
        v = judge(spec, text)
        if v:
            bad[v[0]].append((cls, hi, lo, v[1], text))

    no_reason = sum(c[2] for c in counts.values())
    floor = counts.get("uapi-corpus", [0, 0])
    print(f"\nuapi corpus: {floor[1]} of {floor[0]} accepted")
    print(f"\nrefusals without a reason: {no_reason}")
    print(f"accepted words decoded by sgx-disasm: {len(accepted)}"
          f" ({sum(not asm.ok(t) for t in texts)} with bits their form leaves unused)")
    for cat in ("register", "branch", "closed", "operand", "unknown"):
        print(f"  {cat:<9} {len(bad[cat])}")
        for cls, hi, lo, why, text in bad[cat][:8]:
            print(f"      {hi:08x} {lo:08x}  {why}  ({cls}: {text})")

    # Where the operand disagreements are: form, file, format bits.
    where = collections.Counter()
    for cls, hi, lo, why, text in bad["operand"]:
        try:
            x = asm.parse(asm.body(text))
            name, fmt = ".".join([x.name] + x.mods), "fmt" if x.flags.get("fmt") else "-"
        except ValueError:
            name, fmt = "?", "-"
        where[(name, why.rstrip("0123456789"), fmt)] += 1
    if where:
        print("\noperand disagreements by form, file, format select:")
        for (name, kind, fmt), n in where.most_common():
            print(f"  {n:>7}  {name:<14} {kind:<16} {fmt}")

    # The oracle forms the kernel accepts at all, for the record.
    taken = collections.Counter(cls for cls, _, _ in accepted if cls.startswith("oracle-"))
    never = sorted(n[7:] for n, c in oracle.items() if not c[1])
    print(f"\nsgx.xml forms the kernel accepted some word of: {len(taken)} of {len(oracle)}")
    print(f"never accepted: {' '.join(never)}")
    return 1 if no_reason or any(bad.values()) or floor[0] != floor[1] else 0


if __name__ == "__main__":
    sys.exit(main())
