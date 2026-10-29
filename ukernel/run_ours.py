"""Run openHC's microkernel (ukernel/) through a frame in the emulator and
diff the core registers it programs against the DDK's.

    python3 ukernel/run_ours.py TRACE [--ddk FW]

TRACE is a pvrtrace log (a DDK capture or a tools/gen `frame-fake -t` run).
Our microkernel is built, then driven through the frame's kicks with the same
device memory, HW contexts and kick parameters the DDK sees (fromtrace.py):
init, then for each TA kick our `kick` entry (which calls `do_ta`), then our
`ta_finished` (3D start) and `render_done`. Every `str` to a core register is
collected. With --ddk, the DDK microkernel is run the same way and the two
register-write sets are compared.

The point is to show our microkernel programs the cores from the same
structure words with the same values for a real frame, before any hardware
run. Plumbing the two differ on (cache-invalidate / DPM pulse ordering) is
expected; the struct-sourced core state must match.
"""
import argparse
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "ukernel"))
from emu import USSE, Mem, Regs  # noqa: E402
from sgxsim import A, PD_PHYS, install_hw_model  # noqa: E402
import fromtrace                 # noqa: E402

# the struct-sourced core registers we expect to match the DDK (docs/UKERNEL.md
# §7): everything except the plumbing the two implementations order/handle
# differently (cache-invalidate, DPM load/kick, event-status, soft-reset, the
# CR-derived BIF-status copies and the accumulated mode word)
PLUMBING = {0x134, 0x114, 0x140, 0x694, 0x698, 0x24c, 0x224, 0xa08, 0xc00, 0xc78,
            0x12c, 0x118, 0x138, 0x804, 0x200, 0x428, 0xad0, 0xad4, 0xad8, 0xae0,
            0x080, 0x664, 0x684, 0x680, 0x0a74, 0x630, 0x604, 0x64c, 0x660}

# ours phase -> the DDK Runner phase that does the same work
PHASE_MAP = {"TA1": "TA kick 1", "3D": "TA finished (2)"}


def ours():
    binp = os.path.join(HERE, "sgx545-ukernel-openhc.bin")
    manp = os.path.join(HERE, "sgx545-ukernel-openhc.json")
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), binp, manp], check=True)
    return open(binp, "rb").read(), json.load(open(manp))


def setup(code):
    mem, regs = Mem(), Regs()
    u = USSE(code, base=0x30000000, mem=mem, regs=regs)
    install_hw_model(regs)
    # the control block, laid out as srvinit does (sgxsim.A), DMAed into sa
    t = A["ta3d"]
    mem.wr(t + 4 * 0, t)
    mem.wr(t + 4 * 1, A["hostctl"])
    mem.wr(t + 4 * 2, A["ccbctl"])
    mem.wr(t + 4 * 16, A["kccb"])
    return u, mem, regs


def dma(u, mem):
    """refresh sa0..sa63 from the control block, as the entry PDS program does"""
    for i in range(64):
        u.sa[i] = mem.rd(A["ta3d"] + 4 * i)


def reg_writes(u):
    return [(a, v) for (op, a, v, pc) in u.regs.log if op == "str"]


def run_ours(code, man, events, ctxs):
    u, mem, regs = setup(code)
    for va, data, pdo in ctxs:
        mem.load(va, data)
        mem.wr(va + pdo, PD_PHYS)
    # init (clean)
    dma(u, mem)
    u.task(man["entries"]["init"] // 8)
    out = []
    kicks = [k for k in events if k.kind == "kick"]
    for i, k in enumerate(kicks):
        k.load(mem)
        # CMDTA_SHARED (ui32NumTAStatusVals and the status list) is filled by
        # the kernel at kick time; a trace snapshots memory before the ioctl,
        # so load it from a later event's snapshot, as tools/ukernel/run.py does
        if i + 1 < len(kicks):
            cmd = k.ccb_va + k.ccb_off - 0x58
            d = kicks[i + 1].memory.read(cmd, 0x1E0)
            if d:
                mem.load(cmd, d)
        # the kernel CCB command the host wrote: ServiceAddress = type 0 (TA),
        # data[1] = context (our host-kick table is the identity)
        slot = (k.n - 1) & 0xFF
        c = A["kccb"] + 32 * slot
        mem.wr(c + 0, 0)                    # type 0 = TA
        mem.wr(c + 4 * 3, k.data[1])        # context
        mem.wr(A["ccbctl"] + 0, k.n)        # kernel CCB write offset (slot count)
        mem.wr(A["ccbctl"] + 4, slot)       # read offset
        regs.log.clear()
        mem.log = []
        dma(u, mem)
        u.task(man["entries"]["kick"] // 8)
        out += [("TA%d" % k.n, a, v) for a, v in reg_writes(u)]
        # this TA's finish: write its status-value list to the sync heap
        mem.log = []
        dma(u, mem)
        u.task(man["entries"]["ta_finished"] // 8)
        out += [("sync%d" % k.n, a, v) for (op, a, v, pc) in mem.log
                if op == "st" and 0x2d000000 <= a < 0x2e000000]
        if k.last:
            regs.log.clear()
            dma(u, mem)
            u.task(man["entries"]["do_3d"] // 8)
            out += [("3D", a, v) for a, v in reg_writes(u)]
            regs.log.clear()
            mem.log = []
            dma(u, mem)
            u.task(man["entries"]["render_done"] // 8)
            out += [("done", a, v) for a, v in reg_writes(u)]
            cmd = k.ccb_va + k.ccb_off - 0x58
            rd = mem.rd(cmd + 0x38)
            for (op, a, v, pc) in mem.log:
                if op == "st" and (0x2d000000 <= a < 0x2e000000 or rd <= a < rd + 0x1ec):
                    out.append(("done_mem", a, v))
    return out


def run_ddk(fw, trace):
    """the DDK microkernel's register writes over the same frame, via
    tools/ukernel/run.py's model. Returns {reg: final value}."""
    sys.path.insert(0, os.path.join(ROOT, "tools", "ukernel"))
    import importlib
    runmod = importlib.import_module("run")
    r = runmod.Runner(open(fw, "rb").read(), trace, out=open(os.devnull, "w"))
    perphase = {}
    orig = r.flush

    def flush(title):
        d = perphase.setdefault(title, {})
        for (op, a, v, pc) in r.m.regs.log:
            if op == "str":
                d[a] = v
        orig(title)
    r.flush = flush
    r.run()
    return perphase


def run_ddk_sync(fw, trace):
    """the DDK's sync-heap (0x2d......) memory writes, grouped as {sync<N>:
    {addr: value}} to line up with ours' per-kick TA-finished phases"""
    import re
    sys.path.insert(0, os.path.join(ROOT, "tools", "ukernel"))
    import importlib
    runmod = importlib.import_module("run")
    r = runmod.Runner(open(fw, "rb").read(), trace, out=open(os.devnull, "w"))
    out = {}
    orig = r.flush

    def flush(title):
        m = re.match(r"TA finished \((\d+)\)", title)
        if m and r.m.mem.log:
            d = out.setdefault("sync" + m.group(1), {})
            for (op, a, v, pc) in r.m.mem.log:
                if op == "st" and 0x2d000000 <= a < 0x2e000000:
                    d[a] = v
        orig(title)
    r.flush = flush
    r.run()
    return out


def run_ddk_done(fw, trace):
    """the DDK's sync-heap / render-details memory writes during render
    completion (the '3D end of render' and 'DPM 3D memory free' phases)"""
    sys.path.insert(0, os.path.join(ROOT, "tools", "ukernel"))
    import importlib
    runmod = importlib.import_module("run")
    r = runmod.Runner(open(fw, "rb").read(), trace, out=open(os.devnull, "w"))
    out = {}
    orig = r.flush

    def flush(title):
        if title.startswith("3D end") or title.startswith("DPM 3D"):
            for (op, a, v, pc) in r.m.mem.log:
                if op == "st" and 0x2d000000 <= a < 0x2e000000:
                    out[a] = v
        orig(title)
    r.flush = flush
    r.run()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--ddk")
    a = ap.parse_args()
    code, man = ours()
    ctxs = []
    events = fromtrace.kicks(a.trace, ctxs)
    out = run_ours(code, man, events, ctxs)
    # ours, grouped by phase: {phase: {reg: value}} over struct-sourced registers
    ours_ph = {}
    for ph, r, v in out:
        if r not in PLUMBING:
            ours_ph.setdefault(ph, {})[r] = v
    tot = sum(len(d) for ph, d in ours_ph.items() if ph in PHASE_MAP)
    print(f"openHC microkernel: {len(out)} register writes; {tot} struct-sourced "
          f"across phases {sorted(ours_ph)}")
    if not a.ddk:
        for ph in sorted(ours_ph):
            for r in sorted(ours_ph[ph]):
                print(f"  {ph:4} {r:#06x} = {ours_ph[ph][r]:#010x}")
        return
    ddk = run_ddk(a.ddk, a.trace)
    # the TA status-value sync-heap writes: ours tags them sync<N>, the DDK
    # does them in its "TA finished (N)" phase as memory writes
    ddk_sync = run_ddk_sync(a.ddk, a.trace)
    bad = 0
    for ph, ddk_sync_vals in ddk_sync.items():
        om = {r: v for (p, r, v) in out if p == ph}
        miss = [(r, ddk_sync_vals[r], om.get(r)) for r in ddk_sync_vals if om.get(r) != ddk_sync_vals[r]]
        print(f"[{ph}] DDK {len(ddk_sync_vals)} sync-heap writes, ours {len(om)}")
        for r, dv, ov in sorted(miss):
            tag = "MISSING" if ov is None else "DIFF"
            print(f"    {tag} {r:#010x}: DDK {dv:#010x}" + ("" if ov is None else f" vs ours {ov:#010x}"))
        bad += len(miss)
    # render completion: every sync-heap write ours makes must match the DDK's
    ddk_done = run_ddk_done(a.ddk, a.trace)
    ours_done = {r: v for (p, r, v) in out if p == "done_mem" and 0x2d000000 <= r < 0x2e000000}
    dmiss = [(r, ours_done[r], ddk_done.get(r)) for r in ours_done if ddk_done.get(r) != ours_done[r]]
    print(f"[done] ours {len(ours_done)} sync-heap writes (subset of the DDK's {len(ddk_done)})")
    for r, ov, dv in sorted(dmiss):
        print(f"    MISMATCH {r:#010x}: ours {ov:#010x} vs DDK " + ("(none)" if dv is None else f"{dv:#010x}"))
    bad += len(dmiss)
    for ph, ddk_phase in PHASE_MAP.items():
        om = ours_ph.get(ph, {})
        dm = {r: v for r, v in ddk.get(ddk_phase, {}).items() if r not in PLUMBING}
        diff = [(r, dm[r], om.get(r)) for r in dm if om.get(r) != dm[r]]
        extra = [r for r in om if r not in dm]
        print(f"[{ph} vs DDK '{ddk_phase}'] DDK {len(dm)} struct regs, ours {len(om)}")
        for r, dv, ov in sorted(diff):
            tag = "MISSING" if ov is None else "DIFF"
            print(f"    {tag} {r:#06x}: DDK {dv:#010x}" + ("" if ov is None else f" vs ours {ov:#010x}"))
        for r in sorted(extra):
            print(f"    EXTRA   {r:#06x}: ours {om[r]:#010x}")
        bad += len(diff) + len(extra)
    if bad == 0:
        print("MATCH: every struct-sourced core register equals the DDK's, per phase")
        return
    print(f"{bad} register(s) differ")
    sys.exit(1)


if __name__ == "__main__":
    main()
