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


def load():
    d = tempfile.mkdtemp()
    binp, manp = os.path.join(d, "sgx530.bin"), os.path.join(d, "sgx530.json")
    env = dict(os.environ)
    env.pop("SGX_CORE", None)
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), "sgx530", binp, manp],
                   check=True, stdout=subprocess.DEVNULL, env=env)
    man = json.load(open(manp))
    code = bytearray(open(binp, "rb").read())
    patch_limm(code, man["patches"]["ctlblk"], CTLBLK)
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
    bad = sorted({a for log in runs for (op, a, v, pc) in log if a not in STAGE1_NAMED})
    assert not bad, "stage 1 touches unnamed registers: " + ", ".join(f"{a:#x}" for a in bad)
    print(f"  stage 1: only named registers ({len(STAGE1_NAMED)})  OK")


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
    test_getmiscinfo(code, man)
    test_restore_pd(code, man)
    test_bounded_wait(code, man)
    test_brn28889(code, man)
    test_first_ta(code, man)
    test_render_complete(code, man)
    print("all passed")


if __name__ == "__main__":
    main()
