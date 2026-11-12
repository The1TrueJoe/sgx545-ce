# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Replay the two-surface v2s trace through OUR microkernel in a chosen order,
printing each phase's register + memory writes, to diff against the DDK's
tools/ukernel/interleave.py output. The ours-side counterpart of interleave.py:
it reuses run_ours.py's loader and event drive (our 14 KB image + json), where
interleave.py drives the 48 KB DDK firmware through run.py.

    python3 tools/ukernel/interleave_ours.py gen/v2s.trace [--order overlap|interleave|serial]

overlap:    A first, A last (A's 3D starts), B first while A's 3D runs, A's 3D
            ends, B last -- the order UK_OVERLAP must handle. Diff vs
            `interleave.py FW gen/v2s.trace --order overlap` (strip the build
            banner and the DDK's task/pds tuples; compare the str/st writes).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))        # tools/ukernel
ROOT = os.path.join(HERE, "..", "..")                    # repo root
sys.path.insert(0, os.path.join(ROOT, "ukernel"))        # run_ours
sys.path.insert(0, HERE)                                 # fromtrace, sgxsim
import run_ours as RO                                     # noqa: E402
from sgxsim import A, PD_PHYS                             # noqa: E402
import fromtrace                                          # noqa: E402


def dump(u, mem):
    for (op, ad, v, pc) in u.regs.log:
        if op == "str":
            print(f"   str {ad:#06x} = {v:#010x}  @{pc:#06x}")
    for (op, ad, v, pc) in mem.log:
        if op == "st":
            print(f"   st  {ad:#010x} = {v:#010x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--order", choices=("interleave", "overlap", "overlap2", "serial"), default="overlap")
    a = ap.parse_args()
    code, man = RO.ours()
    ctxs = []
    events = fromtrace.kicks(a.trace, ctxs)
    kicks = [e for e in events if e.kind == "kick"]
    if len(kicks) != 4:
        sys.exit(f"needs a two-surface two-kick trace (tools/gen v2s); got {len(kicks)} kicks")
    a1, a2, b1, b2 = kicks

    u, mem, regs = RO.setup(code, man)
    for va, data, pdo in ctxs:
        mem.load(va, data)
        mem.wr(va + pdo, PD_PHYS)
    mem.wr(A["hostctl"], 0)
    mem.wr(A["hostctl"] + 4 * 9, 0)
    RO.event(u, mem, regs, man, 0)
    print("== init")
    dump(u, mem)

    slot = [0]

    def kick(name, k):
        k.load(mem)
        # CMDTA_SHARED is filled by the kernel at kick time; a trace snapshots
        # before the ioctl, so load it from a later trace-order kick (run_ours).
        ki = kicks.index(k)
        if ki + 1 < len(kicks):
            d = kicks[ki + 1].memory.read(k.ccb_va + k.ccb_off, 0x1E0)
            if d:
                mem.load(k.ccb_va + k.ccb_off, d)
        s = slot[0] & 0xFF
        c = A["kccb"] + 32 * s
        mem.wr(c + 0, 0)                        # type 0 = TA
        mem.wr(c + 4 * 3, k.data[1])            # context
        mem.wr(A["ccbctl"] + 0, slot[0] + 1)    # kernel CCB write offset
        mem.wr(A["ccbctl"] + 4, s)              # read offset
        client_ccbctl = mem.rd(k.data[1] + 4 * 4)
        mem.wr(client_ccbctl + 4, k.ccb_off - 0x58)
        slot[0] += 1
        RO.event(u, mem, regs, man, RO.EV_HOST_KICK)
        print(f"== kick {name}")
        dump(u, mem)
        RO.event(u, mem, regs, man, RO.EV_TA_FINISHED)
        print(f"== TA finished {name}")
        dump(u, mem)

    def render_end(name):
        RO.event(u, mem, regs, man, RO.EV_3D_END)
        print(f"== 3D end {name}")
        dump(u, mem)
        RO.event(u, mem, regs, man, RO.EV_DPM_FREE)
        print(f"== DPM 3D memory free {name}")
        dump(u, mem)

    if a.order == "overlap":
        kick("A first", a1)
        kick("A last", a2)
        kick("B first (A's 3D running)", b1)
        render_end("A")
        kick("B last", b2)
        render_end("B")
    elif a.order == "overlap2":
        # B's last TA finishes WHILE A's 3D is still in flight -> the 3D queue must hold B's 3D until
        # render_end("A") frees the engine. (No DDK reference for this order; self-consistency check only:
        # B's 3D kick 0x428 must appear in "DPM 3D memory free A", not in "TA finished B last".)
        kick("A first", a1)
        kick("A last", a2)
        kick("B first", b1)
        kick("B last", b2)
        render_end("A")
        render_end("B")
    elif a.order == "interleave":
        kick("A first", a1)
        kick("B first", b1)
        kick("A last", a2)
        render_end("A")
        kick("B last", b2)
        render_end("B")
    else:
        for name, f, l in (("A", a1, a2), ("B", b1, b2)):
            kick(f"{name} first", f)
            kick(f"{name} last", l)
            render_end(name)


if __name__ == "__main__":
    main()
