# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Peak temporary-register pressure of the assembled microkernel.

    python3 ukernel/pressure.py [core]     (core defaults to $SGX_CORE or sgx545)

Builds the instruction stream (ukernel.build), recovers the control-flow graph
from the labels and branches, runs a standard backward liveness dataflow over the
temporary registers (rN, N<64), and reports the maximum number simultaneously
live at any program point. That peak is what a register-file restructure (SGX530,
scratch in the output bank: docs/porting/cores/sgx530-attr-restructure.md) must
fit. The two loader-set bases r17 (control block) and r14 (PD table) count here;
the restructure moves them to primary attributes, so subtract them for the output
budget. Def/use by mnemonic: limm/ldr/ld.a.d and the ALU ops define their first
register and use the rest; st.a.d/str/test define none (test writes p0, not a
temp); fences and branches touch no temporary. A loader `patch` is an `limm` that
defines its register. Unconditional `ba` has one successor; a predicated `ba`
also falls through; `nop [end]` terminates. An approximation only where a
mnemonic's def/use is mis-split, so treat the number as +/-1."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "isa"))

REG = re.compile(r"\br(\d+)\b")
NO_REGS = {"wdf", "idf", "nop", "phas.i", "savl", "setl", "lapc", "__raw__"}


def _regs_in(text):
    """temporary register numbers in text (before any [..] decoration)."""
    return [int(x) for x in REG.findall(text.split("[")[0]) if int(x) < 64]


def _def_use(ins):
    """(defs, uses) for one instruction. Branch handling is done by the caller."""
    mn = ins.split()[0] if ins.split() else ""
    order = _regs_in(ins)
    if mn in NO_REGS or re.match(r"\s*\(?!?p0\)?\s*ba\b", ins) or re.search(r"\bba\s", ins):
        return set(), set()
    if mn == "limm" or mn == "ldr":            # define first reg, use none
        return ({order[0]} if order else set()), set()
    if mn == "str" or mn.startswith("st.a.d") or mn.startswith("test"):
        return set(), set(order)               # define none; all regs are uses
    # ld.a.d and the ALU ops: first reg defined, the rest used
    if not order:
        return set(), set()
    return {order[0]}, set(order[1:])


def analyze(core):
    os.environ.setdefault("SGX_CORE", core)
    import ukernel
    a = ukernel.Asm()
    ukernel.build(a)

    insns, labelpos = [], {}
    for kind, payload in a.lines:
        if kind == "label":
            labelpos[payload] = len(insns)
        elif kind == "i":
            insns.append(payload)
        elif kind == "patch":
            insns.append(f"limm r{payload[0]}, #patch")
        elif kind == "raw":
            insns.append("__raw__")
    n = len(insns)

    defs, uses, succ = [], [], []
    for i, ins in enumerate(insns):
        d, u = _def_use(ins)
        defs.append(d)
        uses.append(u)
        s = []
        bm = re.search(r"\bba\s+(\S+)", ins)
        uncond = bool(re.match(r"\s*ba\b", ins))
        term = "[end]" in ins
        if bm is not None:
            if bm.group(1) in labelpos:
                s.append(labelpos[bm.group(1)])
            if not uncond and i + 1 < n:
                s.append(i + 1)
        elif not term and i + 1 < n:
            s.append(i + 1)
        succ.append(s)

    livein = [set() for _ in range(n)]
    liveout = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            lo = set().union(*(livein[x] for x in succ[i])) if succ[i] else set()
            li = (lo - defs[i]) | uses[i]
            if lo != liveout[i] or li != livein[i]:
                liveout[i], livein[i] = lo, li
                changed = True

    peak_i = max(range(n), key=lambda i: len(livein[i]))
    return insns, livein, peak_i


def main():
    core = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SGX_CORE", "sgx545")
    insns, livein, peak_i = analyze(core)
    peak = len(livein[peak_i])
    print(f"core {core}: {len(insns)} instructions")
    print(f"peak temporary pressure: {peak} live")
    print(f"  at #{peak_i}: {insns[peak_i]!r}")
    print(f"  live: {sorted(livein[peak_i])}")
    bases = {14, 17} & livein[peak_i]
    if bases:
        print(f"  of which loader-set bases (move to attributes): {sorted(bases)}"
              f"  -> output budget {peak - len(bases)}")
    hist = {}
    for i in range(len(insns)):
        hist[len(livein[i])] = hist.get(len(livein[i]), 0) + 1
    print("pressure histogram:", dict(sorted(hist.items())))


if __name__ == "__main__":
    main()
