"""Replay a two-surface trace (tools/gen v2s: two RT datasets, one context and PB)
through a microkernel in the emulator, with the two scenes interleaved.

    python3 tools/ukernel/interleave.py FW TRACE [--order interleave|overlap]

interleave: A first, B first (A's scene open, not rendered), A last, B last.
overlap:    A first, A last (A's 3D starts), B first while A's 3D runs, A's 3D
            ends, B last.
The commands are rewritten into the context CCB in that order. Prints every
event's register and memory writes, as tools/ukernel/run.py does.
"""
import argparse
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as RUN  # noqa: E402
from sgxsim import A  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fw")
    ap.add_argument("trace")
    ap.add_argument("--order", choices=("interleave", "overlap"), default="interleave")
    a = ap.parse_args()
    out = io.StringIO()
    r = RUN.Runner(open(a.fw, "rb").read(), a.trace, out=out)
    mem = r.m.mem
    prev = r.m.regs.on_write.get(0x24C)

    def otpm(regs, v):
        # an OTPM flush reports OTPM_FLUSHED_INV (EVENT_STATUS2 b11), which the DPM
        # store routine waits for
        if prev:
            prev(regs, v)
        regs.v[0x118] = regs.v.get(0x118, 0) | 0x800
    r.m.regs.on_write[0x24C] = otpm

    kicks = [k for k in r.events if k.kind == "kick"]
    if len(kicks) != 4:
        sys.exit("needs a trace with two two-kick frames on two surfaces (tools/gen v2s)")
    a1, a2, b1, b2 = kicks
    a1.load(mem)
    have = {va for va, _, _ in a1.memory.regions}
    for va, d, _ in b1.memory.regions:       # surface B's buffers, new since A's first kick
        if va not in have:
            mem.load(va, d)
    ctl = mem.rd(r.ctxs[0][0] + 0x10)         # the context's CCB control
    mem.wr(ctl, 0)
    mem.wr(ctl + 4, 0)
    slot = [0]

    def kick(name, k):
        dst = a1.ccb_va + slot[0] * 0x2C0
        mem.load(dst, k.memory.read(k.ccb_va + k.ccb_off - 0x58, 0x2C0))
        later = r.events[r.events.index(k) + 1:]
        sh = later[0].memory.read(k.ccb_va + k.ccb_off, 0x1E0) if later else None
        if sh:
            mem.load(dst + 0x58, sh)
        mem.wr(dst + 0x58, mem.rd(dst + 0x58) | 1)
        slot[0] += 1
        mem.wr(ctl, slot[0] * 0x2C0)
        r.step(f"kick {name}", r.m.kick, 0, k.data)
        r.event(f"TA finished {name}", RUN.EV_TA_FINISHED)

    def render_end(name):
        r.event(f"3D end {name}", RUN.EV_3D_END)
        r.event(f"DPM 3D memory free {name}", RUN.EV_3D_FREE)

    if a.order == "interleave":
        kick("A first", a1)
        kick("B first", b1)
        kick("A last", a2)
        render_end("A")
        kick("B last", b2)
        render_end("B")
    else:
        kick("A first", a1)
        kick("A last", a2)
        kick("B first (A's 3D running)", b1)
        render_end("A")
        kick("B last", b2)
        render_end("B")
    print(out.getvalue())
    return 0 if r.ok else 1


if __name__ == "__main__":
    sys.exit(main())
