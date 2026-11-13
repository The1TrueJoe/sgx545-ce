# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Run the open microkernel (ukernel/) through a frame in the emulator and
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
            0x080, 0x664, 0x684, 0x680, 0x630, 0x604, 0x64c, 0x660}

# ours phase -> the DDK Runner phase that does the same work. TA2 is the
# second (render / bLastInScene) kick: the DDK does a MINIMAL kick there (it
# skips the per-scene setup kick 1 already did), so comparing it catches a
# microkernel that re-runs the first-kick setup on every kick.
PHASE_MAP = {"TA1": "TA kick 1", "TA2": "TA kick 2", "3D": "TA finished (2)"}

# HW render context words that also address a transfer context's CCB
CTX_CCBCTL = 0x10 // 4
TRANSFERCMD_SHARED = 0xac


def ours():
    binp = os.path.join(HERE, "sgx545_ukernel.bin")
    manp = os.path.join(HERE, "sgx545_ukernel.json")
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), binp, manp], check=True)
    return open(binp, "rb").read(), json.load(open(manp))


def patch_limm(code, off, v):
    lo, hi = struct.unpack_from("<II", code, off)
    lo = (lo & 0xFFE00000) | (v & 0x1FFFFF)
    hi = (hi & 0xFFFC0E0F) | ((v >> 17) & 0x1F0) | ((v >> 26) << 12)
    struct.pack_into("<II", code, off, lo, hi)


def setup(code, man):
    # patch the control-block-base limm, as srvinit -o does on the box, so the
    # event entry can populate `sa` itself (no DMA)
    code = bytearray(code)
    patch_limm(code, man["patches"]["ctlblk"], A["ta3d"])
    mem, regs = Mem(), Regs()
    u = USSE(bytes(code), base=0x30000000, mem=mem, regs=regs)
    install_hw_model(regs)
    # the control block, laid out as srvinit does (sgxsim.A)
    t = A["ta3d"]
    mem.wr(t + 4 * 0, t)
    mem.wr(t + 4 * 1, A["hostctl"])
    mem.wr(t + 4 * 2, A["ccbctl"])
    mem.wr(t + 4 * 0x16, A["kccb"])        # kernel CCB base (DDK slot +0x58)
    return u, mem, regs


def reg_writes(u):
    return [(a, v) for (op, a, v, pc) in u.regs.log if op == "str"]


# EUR_CR_EVENT_STATUS bits the host-side model raises for each event
EV_HOST_KICK = 0            # host kick = EUR_CR_EVENT_KICK doorbell: no EVENT_STATUS bit (the microkernel finds the command via the kernel CCB write != read offset)
EV_TA_FINISHED = 0x2000
EV_3D_END = 0x40000         # PIXELBE end of render -> pixel_done (3D rendered, no completion)
EV_DPM_FREE = 0x1           # DPM 3D memory free -> render_done (completes the render, DDK order)


def event(u, mem, regs, man, status):
    """deliver one hardware event: set EUR_CR_EVENT_STATUS to exactly `status`
    and run the event entry, which populates sa and dispatches"""
    regs.v[0x12C] = status
    regs.log.clear()
    mem.log = []
    u.task(man["entries"]["event"] // 8)


def run_ours(code, man, events, ctxs):
    u, mem, regs = setup(code, man)
    for va, data, pdo in ctxs:
        mem.load(va, data)
        mem.wr(va + pdo, PD_PHYS)
    # init: ui32InitStatus == 0 makes the event entry dispatch to init
    mem.wr(A["hostctl"], 0)
    mem.wr(A["hostctl"] + 4 * 9, 0)        # HWR clear (clean boot)
    event(u, mem, regs, man, 0)
    out = []
    kicks = [k for k in events if k.kind == "kick"]
    ki = 0
    for e in events:
        if e.kind != "kick":
            continue                        # xfers + exit cleanups handled after this loop
        k = e
        k.load(mem)
        # CMDTA_SHARED (ui32NumTAStatusVals and the status list) is filled by
        # the kernel at kick time; a trace snapshots memory before the ioctl,
        # so load it from a later event's snapshot, as tools/ukernel/run.py does
        if ki + 1 < len(kicks):
            cmd = k.ccb_va + k.ccb_off - 0x58
            d = kicks[ki + 1].memory.read(cmd, 0x1E0)
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
        # the client CCB read offset the microkernel reads to find this TA
        # command (do_ta: command base = client CCB + read offset). A trace's
        # snapshot of it is not always advanced (frame-fake leaves it 0), so
        # set it to this kick's command-base offset, the state the DDK keeps.
        client_ccbctl = mem.rd(k.data[1] + 4 * 4)      # HW render context word 4
        mem.wr(client_ccbctl + 4, k.ccb_off - 0x58)
        # No synthetic READY bit here: the kernel does NOT set one (SGXKickTA only
        # fills CMDTA_SHARED sync/status and flushes the CPU cache for bLastInScene,
        # sgxutils.c). ta_finished must decide the 3D from the command's own flags
        # (cmd+0x04 bit 1 = bLastInScene, 0x62 vs 0x71) in the loaded trace data —
        # so a wrong 3D trigger makes the last kick skip do_3d and this diff fail.
        event(u, mem, regs, man, EV_HOST_KICK)    # -> kick -> do_ta
        out += [("TA%d" % k.n, a, v) for a, v in reg_writes(u)]
        # this TA's finish: the status walk, and on the last kick the 3D start
        event(u, mem, regs, man, EV_TA_FINISHED)  # -> ta_finished (-> do_3d if last)
        out += [("sync%d" % k.n, a, v) for (op, a, v, pc) in mem.log
                if op == "st" and 0x2d000000 <= a < 0x2e000000]
        if k.last:
            out += [("3D", a, v) for a, v in reg_writes(u)]
            event(u, mem, regs, man, EV_3D_END)   # -> pixel_done: 3D rendered, render stays in flight
            event(u, mem, regs, man, EV_DPM_FREE) # -> render_done: complete on the DPM free (DDK order)
            out += [("done", a, v) for a, v in reg_writes(u)]
            # every memory write render_done makes (event() cleared the log before the
            # EV_DPM_FREE run, so mem.log is exactly the completion handler's writes)
            for (op, a, v, pc) in mem.log:
                if op == "st":
                    out.append(("done_mem", a, v))
        ki += 1
    # transfers (SGX_SUBMITTRANSFER): a 3D-only render. Drive each through our
    # kick entry with a type-1 command (data[1] = transfer context), as the
    # host does, and collect the 3D registers k_transfer programs.
    xfers = [k for k in events if k.kind == "xfer"]
    for i, x in enumerate(xfers):
        x.load(mem)
        ctx = x.data[1]
        ccbctl = mem.rd(ctx + 4 * CTX_CCBCTL)          # the transfer context's CCB control
        mem.wr(ccbctl + 4, x.ccb_off - TRANSFERCMD_SHARED)   # read offset -> this command
        slot = i & 0xFF
        c = A["kccb"] + 32 * slot
        mem.wr(c + 0, 1)                               # type 1 = transfer
        mem.wr(c + 4 * 3, ctx)                         # data[1] = transfer context
        mem.wr(A["ccbctl"] + 0, i + 1)                 # kernel CCB write offset
        mem.wr(A["ccbctl"] + 4, slot)                  # read offset
        event(u, mem, regs, man, EV_HOST_KICK)         # -> kick -> k_transfer
        out += [("XFER%d" % (i + 1), a, v) for a, v in reg_writes(u)]
    # RESMAN exit cleanups (CMD_CLEANUP = 5), after all renders/transfers. A cleanup that
    # races a still-in-flight render must DEFER (SC_INFLIGHT set) so the host can't free
    # the PB/RT under the running 3D / DPM-free, then complete once render_done -- which
    # now runs on the DPM 3D-memory-free event (0x1), i.e. AFTER the pages are freed --
    # clears SC_INFLIGHT. Model that race directly: assert SC_INFLIGHT, send the cleanup
    # (must defer); clear it (render_done's effect on 0x1) and resend (must complete).
    # Never deadlock (else the host frees the PB/RT under the free -> the CE5310 board render fault).
    SC_INFLIGHT = 0x14
    nclean = 0
    for e in events:
        if e.kind != "cleanup":
            continue
        slot = (0x80 + nclean) & 0xFF
        nclean += 1
        cc = A["kccb"] + 32 * slot
        mem.wr(cc + 0, 5)                       # CMD_CLEANUP
        mem.wr(cc + 4 * 2, e.data[0])           # cleanup type (RC 2, TC 3, ...)
        mem.wr(cc + 4 * 3, e.data[1])           # resource device address
        mem.wr(A["hostctl"] + 8, 0)             # host clears CleanupStatus before the send
        mem.wr(A["ta3d"] + 4 * SC_INFLIGHT, e.data[1] or 0x1)  # a render in flight races the cleanup
        mem.wr(A["ccbctl"] + 0, slot + 1)
        mem.wr(A["ccbctl"] + 4, slot)
        event(u, mem, regs, man, EV_HOST_KICK)  # -> k_cleanup, render in flight -> defer
        deferred = not (mem.rd(A["hostctl"] + 8) & 0x1)
        mem.wr(A["ta3d"] + 4 * SC_INFLIGHT, 0)  # render_done (on 0x1) clears SC_INFLIGHT after the free
        mem.wr(A["ccbctl"] + 0, slot + 1)
        mem.wr(A["ccbctl"] + 4, slot)
        event(u, mem, regs, man, EV_HOST_KICK)  # -> k_cleanup again -> completes
        completed = bool(mem.rd(A["hostctl"] + 8) & 0x1)
        ok = deferred and completed
        print("  cleanup type=%d res=%#x: deferred-while-in-flight=%s completed=%s  %s"
              % (e.data[0], e.data[1], deferred, completed, "OK" if ok else "FAIL"))
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
    """every memory write the DDK makes during render completion (the '3D end
    of render' and 'DPM 3D memory free' phases): the sync-object updates and the
    full render/RT/context teardown"""
    sys.path.insert(0, os.path.join(ROOT, "tools", "ukernel"))
    import importlib
    runmod = importlib.import_module("run")
    r = runmod.Runner(open(fw, "rb").read(), trace, out=open(os.devnull, "w"))
    out = {}
    orig = r.flush

    def flush(title):
        # the TA-render completion only (the "(transfer)" variants are the
        # transfer's own completion, validated separately)
        if (title.startswith("3D end") or title.startswith("DPM 3D")) and "transfer" not in title:
            for (op, a, v, pc) in r.m.mem.log:
                if op == "st":
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
    events = fromtrace.kicks(a.trace, ctxs, cleanups=True)
    out = run_ours(code, man, events, ctxs)
    # ours, grouped by phase: {phase: {reg: value}} over struct-sourced registers
    ours_ph = {}
    for ph, r, v in out:
        if r not in PLUMBING:
            ours_ph.setdefault(ph, {})[r] = v
    tot = sum(len(d) for ph, d in ours_ph.items() if ph in PHASE_MAP)
    print(f"microkernel: {len(out)} register writes; {tot} struct-sourced "
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
    # render completion: ours must reproduce every memory write the DDK makes
    # during the teardown, with the same value (sync objects, render/RT/context
    # slot free). EXTRA = a write ours makes that the DDK does not.
    ddk_done = run_ddk_done(a.ddk, a.trace)
    # microkernel-internal control-block scratch (rendezvous WAIT_3D/WAIT_FREE 0x1d/0x1e,
    # cleanup defer counter 0x1c): bookkeeping the DDK keeps at its own offsets, not part
    # of the render teardown the DDK writes. Exclude from the done comparison.
    OHC_SCRATCH = {A["ta3d"] + 4 * w for w in (0x1c, 0x1d, 0x1e)}
    ours_done = {r: v for (p, r, v) in out if p == "done_mem" and r not in OHC_SCRATCH}
    dmiss = [(r, ddk_done[r], ours_done.get(r)) for r in ddk_done if ours_done.get(r) != ddk_done[r]]
    extra = [r for r in ours_done if r not in ddk_done]
    print(f"[done] DDK {len(ddk_done)} completion mem writes, ours {len(ours_done)}")
    for r, dv, ov in sorted(dmiss):
        tag = "MISSING" if ov is None else "DIFF"
        print(f"    {tag} {r:#010x}: DDK {dv:#010x}" + ("" if ov is None else f" vs ours {ov:#010x}"))
    for r in sorted(extra):
        print(f"    EXTRA   {r:#010x}: ours {ours_done[r]:#010x}")
    bad += len(dmiss) + len(extra)
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
    # transfers: each XFERn programs the 3D from its command, vs the DDK's
    # "transfer n". (The DDK model fully programs only the first transfer; later
    # ones are model no-ops, so only non-empty DDK phases are compared.)
    for i in range(1, 1 + sum(1 for p in ours_ph if p.startswith("XFER"))):
        om = ours_ph.get("XFER%d" % i, {})
        dm = {r: v for r, v in ddk.get("transfer %d" % i, {}).items() if r not in PLUMBING}
        if not dm:
            continue
        diff = [(r, dm[r], om.get(r)) for r in dm if om.get(r) != dm[r]]
        extra = [r for r in om if r not in dm]
        print(f"[XFER{i} vs DDK 'transfer {i}'] DDK {len(dm)} struct regs, ours {len(om)}")
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
