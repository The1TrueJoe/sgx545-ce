# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Offline checks of the SGX530 build's per-core paths (cores/sgx530.py).

Builds `build.py sgx530` and runs the paths that differ from the SGX545
through the USSE emulator (tools/ukernel/emu.py): one memory context, the
bounded waits, BRN_28889's split invalidate and the moved registers. The
emulator's register model is the SGX545's, so this checks the microkernel's
own logic, not the SGX530's hardware. No GPU. Run: python3 ukernel/test_sgx530.py
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "ukernel"))
sys.path.insert(0, HERE)
from emu import USSE, Mem, Regs  # noqa: E402
from test_emu import patch_limm, CTLBLK, HOSTCTL, KCCBCTL  # noqa: E402
from cores import sgx530 as C  # noqa: E402

KCCB = 0x32000000
KPD = 0x0abc0000             # the firmware PD: PD table slot 0
CPD = 0x0def0000             # a client's PD, in slot SLOT
SLOT = 3
PDTABLE = 0x32010000         # the host's read-only PD table
MISC = 0x32005000
# registers the SGX530 doesn't have (docs/porting/cores/sgx530.md, "Only on the SGX545")
ABSENT = {0x0c38: "DIR_LIST_BASE1", 0x0c78: "BANK0", 0x0ae4: "EVENT_KICK1",
          0x0ae8: "EVENT_KICK2", 0x0aec: "EVENT_KICK3"}
# stage 1 (docs/porting/microkernel.md) touches only registers the SGX530's GPL
# header names (K): init, the timer, get-misc-info and the idle PD restore
STAGE1_NAMED = {0x0010: "CORE_ID", 0x0014: "CORE_REVISION", 0x012c: "EVENT_STATUS",
                0x0134: "EVENT_HOST_CLEAR", 0x0acc: "EVENT_TIMER", 0x0c00: "BIF_CTRL",
                0x0c84: "DIR_LIST_BASE0"}


def load(core="sgx530"):
    d = tempfile.mkdtemp()
    binp, manp = os.path.join(d, core + ".bin"), os.path.join(d, core + ".json")
    env = dict(os.environ)
    env.pop("SGX_CORE", None)
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), core, binp, manp],
                   check=True, stdout=subprocess.DEVNULL, env=env)
    man = json.load(open(manp))
    code = bytearray(open(binp, "rb").read())
    patch_limm(code, man["patches"]["ctlblk"], CTLBLK)
    if "pdtable" in man["patches"]:                   # absent in the minimal USSE probe
        patch_limm(code, man["patches"]["pdtable"], PDTABLE)
    return bytes(code), man


def machine(code, initialised=True):
    mem, regs = Mem(), Regs()
    u = USSE(code, base=0x30000000, mem=mem, regs=regs)
    mem.wr(CTLBLK + 4 * C.SA_SELF, CTLBLK)
    mem.wr(CTLBLK + 4 * C.SA_HOSTCTL, HOSTCTL)
    mem.wr(CTLBLK + 4 * C.SA_KCCBCTL, KCCBCTL)
    mem.wr(CTLBLK + 4 * C.SA_KCCB, KCCB)
    mem.wr(HOSTCTL + 4 * C.HC_INIT, 1 if initialised else 0)
    for i in range(C.PD_TABLE_SLOTS):                 # unused slots hold the firmware PD
        mem.wr(PDTABLE + 4 * i, KPD)
    mem.wr(PDTABLE + 4 * SLOT, CPD)
    regs.v[C.CR_BIF_DIR_LIST_BASE0] = KPD             # idle: the firmware PD
    return u, mem, regs


def touched(regs, offsets):
    return sorted({a for (op, a, v, pc) in regs.log if a in offsets})


def check_absent(regs, what):
    bad = touched(regs, ABSENT)
    assert not bad, f"{what} touched SGX545-only registers: " + \
        ", ".join(f"{ABSENT[a]} {a:#x}" for a in bad)


def queue(mem, cmd_type, cachectl=0, data0=0, data1=0):
    """one kernel CCB command at slot 0, write offset 1"""
    mem.wr(KCCB + 4 * C.CMD_SERVICE, cmd_type)
    mem.wr(KCCB + 4 * C.CMD_CACHECTL, cachectl)
    mem.wr(KCCB + 4 * C.CMD_DATA0, data0)
    mem.wr(KCCB + 4 * C.CMD_DATA1, data1)
    mem.wr(KCCBCTL + 0, 1)
    mem.wr(KCCBCTL + 4, 0)


def event(u, man, regs, status=0, steps=200000):
    regs.v[C.CR_EVENT_STATUS] = status
    u.task(man["entries"]["event"] // 8, max_steps=steps)


def test_static(man):
    """no instruction of the image addresses a register the SGX530 lacks"""
    import re
    os.environ["SGX_CORE"] = "sgx530"
    import ukernel
    assert ukernel.CORE_NAME == "sgx530"
    bad = []
    for kind, text in ukernel.program().lines:
        m = kind == "i" and re.match(r"(?:\(!?p\d\)\s*)?(str|ldr)\s.*#(\d+)", text)
        if m and int(m.group(2)) * 4 in ABSENT:
            bad.append(f"{text} ({ABSENT[int(m.group(2)) * 4]})")
    assert not bad, "the SGX530 image names SGX545-only registers: " + "; ".join(bad)
    assert man["core"] == "sgx530" and man["features"] == 0x4
    assert set(man["patches"]) == {"ctlblk", "pdtable"}, man["patches"]
    print(f"  static: no SGX545-only register in {man['instructions']} instructions, "
          f"{len(man['waits'])} bounded waits  OK")


def test_init(code, man):
    u, mem, regs = machine(code, initialised=False)
    regs.v[C.CR_BIF_DIR_LIST_BASE0] = KPD
    event(u, man, regs)
    assert mem.rd(HOSTCTL) == 1, "init must set ui32InitStatus"
    assert mem.rd(CTLBLK + 4 * C.SC_VERSION) == C.UK_VERSION
    assert mem.rd(CTLBLK + 4 * C.HEARTBEAT) == 1, "every event must advance the heartbeat"
    event(u, man, regs, status=C.EVENT_TIMER)
    assert mem.rd(CTLBLK + 4 * C.HEARTBEAT) == 2
    check_absent(regs, "init")
    print("  init: InitStatus = 1, heartbeat +1 per event  OK")


def test_stage1_named(code, man):
    """the first boot's paths stay on named registers: no SGX545 DPM writes at init"""
    runs = []
    u, mem, regs = machine(code, initialised=False)
    event(u, man, regs)
    event(u, man, regs, status=C.EVENT_TIMER)
    runs.append(regs.log)
    u, mem, regs = machine(code)
    regs.v[C.CR_BIF_DIR_LIST_BASE0] = CPD             # restore path too
    queue(mem, C.CMD_GETMISCINFO, data1=MISC)
    event(u, man, regs)
    runs.append(regs.log)
    host = [(op, a) for log in runs for (op, a, v, pc) in log if a in (0x130, 0x110)]
    assert not host, f"EVENT_HOST_ENABLE/ENABLE2 are the host's, the image touched {host}"
    bad = sorted({a for log in runs for (op, a, v, pc) in log if a not in STAGE1_NAMED})
    assert not bad, "stage 1 touches unnamed registers: " + ", ".join(f"{a:#x}" for a in bad)
    print(f"  stage 1: only named registers ({len(STAGE1_NAMED)}), none of the host's "
          "EVENT_HOST_ENABLE/ENABLE2  OK")


def test_diag_entry(core="sgx530_diag"):
    """stage 1's diagnostic builds (cores/sgx530_diag*.py): the markers land in
    order before phas.i (or with none), then the normal entry and init run"""
    import importlib
    D = importlib.import_module("cores." + core)
    code, man = load(core)
    assert man["entries"]["event"] == 0 and set(man["patches"]) == {"ctlblk", "pdtable"}
    u, mem, regs = machine(code, initialised=False)
    regs.v[C.CR_CORE_ID] = 0x01120000
    mem.log = []
    event(u, man, regs)
    hb, ver, wait = (CTLBLK + 4 * w for w in (C.HEARTBEAT, C.SC_VERSION, C.WAIT_TIMEOUT_ID))
    m = D.DIAG_ENTRY
    assert regs.log[0][:3] == ("str", C.CR_EVENT_TIMER, D.DIAG_TIMER), regs.log[0]
    assert not D.DIAG_TIMER & 0x1000000, "the marker must leave the timer disabled"
    seen = [(a, v) for (op, a, v, pc) in mem.log if op == "st" and a in (hb, ver, wait)]
    assert seen[:4] == [(wait, m), (ver, 0x01120000), (hb, 1), (hb, 2)], seen
    assert mem.rd(wait) == m, "init must leave the marker word alone"
    assert seen[-1] == (ver, C.UK_VERSION)
    assert mem.rd(HOSTCTL + 4 * C.HC_INIT) == 1, "init must complete after the markers"
    texts = man_text(core)
    phas = [pc for pc, t in enumerate(texts) if t.startswith("phas")]
    first_st = next(pc for (op, a, v, pc) in mem.log if op == "st")
    if D.DIAG_PHAS:
        assert phas and first_st < phas[0], "the first marker must come before phas.i"
    else:
        assert not phas, "the no-phas build must have no phas.i"
    event(u, man, regs, status=C.EVENT_TIMER)
    assert mem.rd(hb) == 4, "a timer event must add 2 more"
    bad = sorted({a for (op, a, v, pc) in regs.log if a not in STAGE1_NAMED})
    assert not bad, "the diagnostic entry touches unnamed registers: " + \
        ", ".join(f"{a:#x}" for a in bad)
    print(f"  {core}: EVENT_TIMER {D.DIAG_TIMER:#x}, wait {m:#x}, CORE_ID in 0x18, "
          f"heartbeat 1, 2 before init, 4 after a tick, phas.i {'after' if D.DIAG_PHAS else 'none'}"
          "  OK")


def test_probes():
    """stage 1's probe build (cores/sgx530_probe.py): the ladder as in sgx530_diag,
    and each probe, on its own page, writes its own EVENT_TIMER value and ends"""
    from cores import sgx530_probe as P
    test_diag_entry("sgx530_probe")
    code, man = load("sgx530_probe")
    seen = []
    for k in P.PROBES:
        off = man["entries"][f"probe_{k}"]
        assert off % 0x1000 == 0 and off >= 0x1000, f"probe {k} at {off:#x}: not on its own page"
        u, mem, regs = machine(code, initialised=False)
        u.task(off // 8, max_steps=100)
        w = [(a, v) for (op, a, v, pc) in regs.log if op == "str"]
        assert w == [(C.CR_EVENT_TIMER, P.PROBE_TIMER + k)], (k, w)
        assert not (P.PROBE_TIMER + k) & 0x1000000
        seen.append(off)
    assert len(set(seen)) == len(P.PROBES) and 7 not in P.PROBES
    print(f"  probes: {len(P.PROBES)} pages from {min(seen):#x}, EVENT_TIMER "
          f"{P.PROBE_TIMER:#x}+k each, image {man['bytes']} B  OK")


def test_usse_probe():
    """stage 1's USSE probe (cores/sgx530_usse.py): the event task is st.a.d A,
    str EVENT_TIMER, st.a.d B, then a self-loop, so a hardware read-back names
    the first instruction class that derails"""
    from cores import sgx530_usse as U
    code, man = load("sgx530_usse")
    assert man["entries"]["event"] == 0 and "usse_loop" in man["entries"]
    u, mem, regs = machine(code, initialised=False)
    u.trace = []
    # the self-loop runs forever offline; the emulator raises when it never ends
    try:
        u.task(0, max_steps=200)
    except Exception:
        pass
    aw = CTLBLK + 4 * C.WAIT_TIMEOUT_ID
    bw = CTLBLK + 4 * C.SC_VERSION
    assert mem.rd(aw) == U.USSE_MARK_A, "st.a.d A must write ctlblk 0x28"
    assert regs.v[C.CR_EVENT_TIMER] == U.DIAG_TIMER, "str must write EVENT_TIMER"
    assert mem.rd(bw) == U.USSE_MARK_B, "st.a.d B must write ctlblk 0x18"
    loop = man["entries"]["usse_loop"] // 8
    assert any(pc == loop for pc, t in u.trace), "execution must reach the self-loop"
    print(f"  USSE probe: st.a.d A {U.USSE_MARK_A:#x}, str EVENT_TIMER {U.DIAG_TIMER:#x}, "
          f"st.a.d B {U.USSE_MARK_B:#x}, self-loop at {man['entries']['usse_loop']:#x}  OK")


def test_bases():
    """stage 1's code-base pad build (cores/sgx530_bases.py): the ladder as in
    sgx530_diag, and each pad, on a 128 B boundary, writes its own EVENT_TIMER value"""
    from cores import sgx530_bases as B
    test_diag_entry("sgx530_bases")
    code, man = load("sgx530_bases")
    offs = []
    for n in B.BASE_PADS:
        off = man["entries"][f"pad_{n}"]
        assert off % 128 == 0 and off >= man["entries"]["raise_host_irq"], f"pad {n} at {off:#x}"
        u, mem, regs = machine(code, initialised=False)
        u.task(off // 8, max_steps=100)
        w = [(a, v) for (op, a, v, pc) in regs.log if op == "str"]
        assert w == [(C.CR_EVENT_TIMER, B.PAD_TIMER + n)], (n, w)
        offs.append(off)
    assert len(set(offs)) == 15 and not (B.PAD_TIMER + 14) & 0x1000000
    print(f"  code-base pads: 15 pads {offs[0]:#x}-{offs[-1]:#x}, EVENT_TIMER "
          f"{B.PAD_TIMER:#x}+n, image {man['bytes']} B  OK")


def man_text(core):
    """a diagnostic image's instruction texts, by index"""
    os.environ["SGX_CORE"] = core
    import importlib
    import ukernel
    importlib.reload(ukernel)
    out = [t for kind, t in ukernel.program().lines if kind in ("i", "patch", "raw")]
    os.environ["SGX_CORE"] = "sgx530"
    importlib.reload(ukernel)
    return [t if isinstance(t, str) else "limm" for t in out]


def test_getmiscinfo(code, man):
    u, mem, regs = machine(code)
    regs.v[C.CR_CORE_ID] = 0x01120000   # read on the board
    regs.v[C.CR_CORE_REVISION] = 0x00010205
    queue(mem, C.CMD_GETMISCINFO, data1=MISC)
    event(u, man, regs)
    assert mem.rd(MISC + 4) == 0x00010205, "misc[1] must be CORE_REVISION (0x14)"
    assert mem.rd(MISC + 8) == 0x01120000, "misc[2] must be CORE_ID (0x10)"
    assert mem.rd(MISC + 20) == 530, "misc[5] must name the core"
    assert mem.rd(MISC) & C.MISC_READY
    assert not touched(regs, {0x1c, 0x20}), "read the SGX545's CORE_ID/REVISION offsets"
    assert mem.rd(KCCBCTL + 4) == 1, "the command must be consumed"
    check_absent(regs, "getmiscinfo")
    print("  getmiscinfo: CORE_ID/REVISION from 0x10/0x14, core 530, consumed  OK")


def test_restore_pd(code, man):
    """a command that leaves the core idle reaches raise_host_irq through
    kick_end's idle path, which must put the firmware PD back"""
    u, mem, regs = machine(code)
    regs.v[C.CR_BIF_DIR_LIST_BASE0] = CPD             # a job's PD left in the list
    queue(mem, C.CMD_GETMISCINFO, data1=MISC)
    event(u, man, regs)
    assert regs.v[C.CR_BIF_DIR_LIST_BASE0] == KPD, "raise_host_irq must restore the firmware PD"
    ctl = [v for (op, a, v, pc) in regs.log if op == "str" and a == C.CR_BIF_CTRL]
    assert ctl == [C.BIF_CTRL_INVALDC, 0], f"the switch back needs an INVALDC pulse, got {ctl}"
    sw = [v for (op, a, v, pc) in regs.log if op == "str" and a == C.CR_EVENT_STATUS]
    assert sw == [C.SW_EVENT], sw
    # already on the firmware PD: no write, no invalidate
    u, mem, regs = machine(code)
    regs.v[C.CR_BIF_DIR_LIST_BASE0] = KPD
    queue(mem, C.CMD_GETMISCINFO, data1=MISC)
    event(u, man, regs)
    assert not [1 for (op, a, v, pc) in regs.log if op == "str" and
                a in (C.CR_BIF_DIR_LIST_BASE0, C.CR_BIF_CTRL)], "a PD hit must not write the BIF"
    print("  idle -> raise_host_irq: client PD -> firmware PD + INVALDC; none on a hit  OK")


def test_bounded_wait(code, man):
    """a data invalidate waits on EVENT_STATUS b26, unknown on the SGX530: with
    the bit never set the wait must give up, log itself and let the command run"""
    u, mem, regs = machine(code)
    queue(mem, C.CMD_GETMISCINFO, cachectl=0x8, data1=MISC)
    event(u, man, regs, steps=8 * C.WAIT_BOUND + 10000)
    wid = mem.rd(CTLBLK + 4 * C.WAIT_TIMEOUT_ID)
    assert mem.rd(CTLBLK + 4 * C.WAIT_TIMEOUT_CT) == 1, "one wait must have timed out"
    w = man["waits"][str(wid)]
    assert (int(w["poll"], 16), int(w["mask"], 16)) == (C.CR_EVENT_STATUS, 0x04000000), w
    assert mem.rd(KCCBCTL + 4) == 1, "the command must still be serviced after the timeout"
    polls = sum(1 for (op, a, v, pc) in regs.log if op == "ldr" and a == C.CR_EVENT_STATUS)
    assert polls >= C.WAIT_BOUND, polls
    print(f"  bounded wait: MADD b26 gave up after {C.WAIT_BOUND} polls, "
          f"logged as wait {wid}, command serviced  OK")


def test_brn28889(code, man):
    """a command asking for a PD and a data invalidate: the BIF part runs in
    one event and the data part only in the next"""
    u, mem, regs = machine(code)
    regs.on_read[C.CR_EVENT_STATUS] = lambda r, v: v | 0x04000000   # MADD done at once
    queue(mem, C.CMD_GETMISCINFO, cachectl=0x2 | 0x8, data1=MISC)
    event(u, man, regs)
    ctl = [v for (op, a, v, pc) in regs.log if op == "str" and a == C.CR_BIF_CTRL]
    assert ctl == [C.BIF_CTRL_INVALDC, 0], ctl
    assert not touched(regs, {0x804}), "the data invalidate must wait for the next event"
    assert mem.rd(KCCB + 4 * C.CMD_CACHECTL) == 0x8, "the command must keep only its data bit"
    assert mem.rd(KCCBCTL + 4) == 0, "the command must stay queued"
    regs.log.clear()
    event(u, man, regs, status=C.EVENT_TIMER)        # the next tick re-services it
    assert not [1 for (op, a, v, pc) in regs.log if op == "str" and a == C.CR_BIF_CTRL]
    assert touched(regs, {0x804}), "the data invalidate runs now"
    assert mem.rd(KCCBCTL + 4) == 1 and mem.rd(MISC) & C.MISC_READY
    assert mem.rd(CTLBLK + 4 * C.WAIT_TIMEOUT_CT) == 0
    print("  BRN_28889: BIF invalidate, then the data invalidate a tick later  OK")


def test_first_ta(code, man):
    """a scene's first TA kick: the TA reset is b3, the job's PD goes into
    DIR_LIST_BASE0 with an INVALDC, and no bank register is touched"""
    u, mem, regs = machine(code)
    ctx, ccb, ccbctl = 0x32006000, 0x32007000, 0x32008000
    rd, pb, rtentry = 0x32009000, 0x3200a000, 0x3200b000
    mem.wr(ctx + 4 * C.CTX_CCB, ccb)
    mem.wr(ctx + 4 * C.CTX_CCBCTL, ccbctl)
    mem.wr(ctx + 4 * C.CTX_PD, SLOT + C.PD_TABLE_SLOTS)   # a slot, masked to the table
    mem.wr(ccbctl + 0, 0x300)
    mem.wr(ccbctl + 4, 0)
    mem.wr(ccb + 4 * C.CMDTA_SIZE, 0x300)
    mem.wr(ccb + 4, 1)                                # bFirstKickOrResume
    mem.wr(ccb + 4 * C.CMDTA_RD, rd)
    mem.wr(ccb + 4 * C.CMDTA_PB, pb)
    mem.wr(ccb + 4 * C.CMDTA_RTENTRY, rtentry)
    mem.wr(rtentry + 4 * C.RT_DATA, 0x3200c000)
    queue(mem, C.CMD_TA, data1=ctx)
    reads = [0]

    def status(r, v):                                 # the doorbell, then every bit done
        reads[0] += 1
        return 0 if reads[0] == 1 else 0xffffffff
    regs.on_read[C.CR_EVENT_STATUS] = status
    regs.on_read[C.CR_EVENT_STATUS2] = lambda r, v: 0xffffffff
    regs.on_read[0x138] = lambda r, v: 0xffffffff
    u.task(man["entries"]["event"] // 8, max_steps=3000000)
    seq = [(a, v) for (op, a, v, pc) in regs.log if op == "str" and
           a in (C.CR_SOFT_RESET, C.CR_BIF_DIR_LIST_BASE0, C.CR_BIF_CTRL, C.CR_TA_KICK)]
    assert seq == [(C.CR_SOFT_RESET, 0x8), (C.CR_SOFT_RESET, 0),
                   (C.CR_BIF_DIR_LIST_BASE0, CPD),
                   (C.CR_BIF_CTRL, C.BIF_CTRL_INVALDC), (C.CR_BIF_CTRL, 0),
                   (C.CR_TA_KICK, 1)], seq
    # BRN_28889: the BIF drains (BIF_MEM_REQ_STAT read) before the MADD invalidate
    order = [a for (op, a, v, pc) in regs.log if a in (C.CR_BIF_MEM_REQ_STAT, 0x804)]
    assert order and order[0] == C.CR_BIF_MEM_REQ_STAT, order
    assert mem.rd(CTLBLK + 4 * C.WAIT_TIMEOUT_CT) == 0
    check_absent(regs, "the first TA kick")
    print("  first TA kick: TA reset b3, PD table slot -> DIR_LIST_BASE0 + INVALDC, "
          "BIF drained before MADD, TA kicked  OK")


def test_render_complete(code, man):
    """a render's completion puts the firmware PD back before its first
    completion write (the sync objects), so the host never sees a done job
    whose PD is still live"""
    u, mem, regs = machine(code)
    mem.log = regs.log                                # one ordered log of MMIO and memory
    regs.v[C.CR_BIF_DIR_LIST_BASE0] = CPD             # the job's PD, loaded at its TA
    rd, rtentry, ctx, rtdata, deps = 0x32041a00, 0x32041cf0, 0x3000e000, 0x32041d60, 0x32041f80
    sync0, sync1 = 0x2d0030f4, 0x2d0030fc
    mem.wr(CTLBLK + 4 * C.SC_RD, rd)
    mem.wr(CTLBLK + 4 * C.SC_RTENTRY, rtentry)
    mem.wr(CTLBLK + 4 * C.SC_CTX, ctx)
    mem.wr(CTLBLK + 4 * C.SC_INFLIGHT, rtentry)
    mem.wr(CTLBLK + 4 * C.WAIT_3D, 1)
    mem.wr(CTLBLK + 4 * C.WAIT_FREE, 0)               # the DPM free already came
    mem.wr(rtentry + 4 * C.RT_DATA, rtdata)
    mem.wr(rd + 4 * C.RD_SYNC0, sync0)
    mem.wr(rd + 0xe0, deps)
    mem.wr(deps, sync1)
    mem.wr(sync0, 7)
    mem.wr(sync1, 7)
    regs.log.clear()
    event(u, man, regs, status=0x40000)               # pixel back end: the rendezvous completes
    assert mem.rd(sync0) == 0 and mem.rd(CTLBLK + 4 * C.SC_INFLIGHT) == 0, "the render must complete"
    pd = next(i for i, (op, a, v, pc) in enumerate(regs.log)
              if op == "str" and a == C.CR_BIF_DIR_LIST_BASE0)
    first = next(i for i, (op, a, v, pc) in enumerate(regs.log) if op == "st" and a in (sync0, sync1))
    assert regs.log[pd][2] == KPD and pd < first, "the firmware PD must be back before the sync writes"
    assert regs.v[C.CR_BIF_DIR_LIST_BASE0] == KPD
    check_absent(regs, "render completion")
    print("  render completion: firmware PD restored before the first sync write  OK")


def main():
    code, man = load()
    print("SGX530 microkernel offline tests:")
    test_static(man)
    test_init(code, man)
    test_stage1_named(code, man)
    test_diag_entry()
    test_diag_entry("sgx530_diag_nophas")
    test_probes()
    test_bases()
    test_usse_probe()
    test_getmiscinfo(code, man)
    test_restore_pd(code, man)
    test_bounded_wait(code, man)
    test_brn28889(code, man)
    test_first_ta(code, man)
    test_render_complete(code, man)
    print("all passed")


if __name__ == "__main__":
    main()
