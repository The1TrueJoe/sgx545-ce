# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""ISA coverage gate (docs/UKERNEL-TOOLING.md §6, step 5): every instruction the
microkernel assembles must be covered by Mesa's description (sgx.xml, written
from docs/ISA.md) before the build moves onto it.

    python3 -I ukernel/isa_coverage.py SGX_ISA_TEST [--core NAME] [--tree DIR] [-v]

SGX_ISA_TEST is Mesa's `sgx_isa_test` (mesa/src/sgx/isa/tests). The build runs as
build.py runs it, and every word it emits is recorded together with the line that
made it (symbols and labels resolved). Two checks:

  form      each word's ISA.md text (as dis.py prints it) assembles through
            sgx_isa_asm to the same 64 bits and disassembles back to that text.
            A failure is a form sgx.xml lacks or encodes differently: an ISA.md
            item first (spec side), then an sgx.xml item (implementation side).
  spelling  each source line, as written, assembles to the same 64 bits (it may
            print differently, e.g. a hex immediate). A failure is notation the
            source uses that ISA.md doesn't define: the source changes to
            ISA.md's spelling, or ISA.md adds it.

--tree runs the gate on another checkout's ukernel/ (default: this one).

Exit status 0 only when both are complete. Output lists each failing instruction
once (with its count) by ISA.md text, the error sgx_isa_test gives, and its
source line.
"""
import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def paths(root):
    """build.py from root/ukernel, hw.py/asm.py from root/tools/isa"""
    global HERE
    HERE = os.path.join(root, "ukernel")
    for d in (os.path.join(root, "tools", "isa"), os.path.join(root, "tools", "usc"), HERE):
        sys.path.insert(0, d)

START = "setfc #0, #1"  # the program-start format control (ISA.md "MOE control")


def record(core):
    """-> [(lo, hi, source text or None for a pre-encoded word)] in program order"""
    if core:
        os.environ["SGX_CORE"] = core
    argv, sys.argv = sys.argv, [os.path.join(HERE, "build.py")]
    try:
        import build
    finally:
        sys.argv = argv
    real = build.asm.assemble
    seen = []

    def hook(line, *a, **k):
        hi, lo = real(line, *a, **k)
        symbols = a[0] if a else k.get("symbols")
        text = build.asm._resolve(line, symbols) if symbols and hasattr(build.asm, "_resolve") else line
        seen.append((lo, hi, text))
        return hi, lo

    build.asm.assemble = hook
    try:
        words = build.assemble()[0]
    finally:
        build.asm.assemble = real
    out, k = [], 0
    for lo, hi in words:   # build.assemble returns (lo, hi); raw words never pass the hook
        if k < len(seen) and (seen[k][0], seen[k][1]) == (lo, hi):
            out.append(seen[k])
            k += 1
        else:
            out.append((lo, hi, None))
    assert k == len(seen), "recorded lines don't match the image"
    return out


def canonical(insts):
    """ISA.md text per word, tracking format control in program order"""
    import hw
    from check_enc import compact
    hw.CTX["fc"] = 2
    res = []
    for lo, hi, _ in insts:
        x = hw.decode(lo, hi)
        res.append(compact(hw.fmt(x)) if x is not None else None)
        hw.track(x)
    return res


def run(exe, lines, text_too=True):
    """lines: [(text, lo, hi)] -> {index: error} for the lines sgx_isa_test
    rejects; text_too=False ignores a line that encodes right but prints
    differently"""
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(START + "\n")
        for t, lo, hi in lines:
            f.write(f"{t} ; {lo:08x} {hi:08x}\n")
        path = f.name
    try:
        r = subprocess.run([exe, path], capture_output=True, text=True)
    finally:
        os.unlink(path)
    errs = {}
    for ln in r.stderr.splitlines():
        m = re.match(re.escape(path) + r":(\d+): (.*)$", ln)
        if m:
            i = int(m.group(1)) - 2
            if text_too or " disassembles as: " not in m.group(2):
                errs.setdefault(i, m.group(2))
    return errs


def report(title, fails, total, verbose, label):
    by = collections.OrderedDict()
    for key, err, src in fails:
        e = by.setdefault(key, [0, err, src])
        e[0] += 1
    print(f"{title}: {total - len(fails)} of {total} instructions covered, "
          f"{len(by)} distinct missing")
    for key, (n, err, src) in list(by.items())[: None if verbose else 40]:
        print(f"  {n:4d}x {key}\n        {err}")
        if src and src != key:
            print(f"        {label}: {src}")
    return len(by)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe", help="Mesa's sgx_isa_test")
    ap.add_argument("--core", default=None)
    ap.add_argument("--tree", default=os.path.join(HERE, ".."))
    ap.add_argument("-v", action="store_true", help="list every missing form")
    a = ap.parse_args()
    paths(os.path.abspath(a.tree))
    insts = record(a.core)
    canon = canonical(insts)

    lines = [(c if c else "(undecoded)", lo, hi) for (lo, hi, _), c in zip(insts, canon)]
    errs = run(a.exe, lines)
    form = [(lines[i][0], errs[i], insts[i][2]) for i in sorted(errs)]
    for i, c in enumerate(canon):
        if c is None and i not in errs:
            form.append((f"{insts[i][1]:08x} {insts[i][0]:08x}", "no ISA.md form decodes it", insts[i][2]))

    src = [(i, (t.strip(), lo, hi)) for i, (lo, hi, t) in enumerate(insts) if t is not None]
    serrs = run(a.exe, [l for _, l in src], text_too=False)
    spell = [(src[j][1][0], serrs[j], canon[src[j][0]]) for j in sorted(serrs)]

    raw = sum(1 for *_, t in insts if t is None)
    print(f"{len(insts)} instructions ({raw} pre-encoded words)")
    nf = report("form", form, len(insts), a.v, "source")
    ns = report("spelling", spell, len(src), a.v, "ISA.md text")
    sys.exit(1 if nf or ns else 0)


if __name__ == "__main__":
    main()
