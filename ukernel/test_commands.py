# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Validate the open microkernel's host-kick command handlers against the DDK,
offline. Needs the DDK firmware (srvinit/fetch-firmware.sh) as the oracle:

    python3 ukernel/test_commands.py /path/to/sgx545-ukernel-1.7.862890.bin

Each command is delivered to both microkernels through the same CCB command,
and the host-visible effect is compared. get-misc-info is compared word for
word (it must match exactly — the kernel validates the version); power,
context-suspend and cleanup are checked for the completion/flag the host polls
(the DDK additionally drives its scheduler, which ours does with the
multi-render bookkeeping).
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "ukernel"))
from emu import USSE, Mem, Regs           # noqa: E402
from sgxsim import Machine, A, install_hw_model  # noqa: E402

CORE_ID, CORE_REV = 0x1000E, 0x10014


def build_ours():
    import subprocess
    binp = os.path.join(HERE, "sgx545_ukernel.bin")
    manp = os.path.join(HERE, "sgx545_ukernel.json")
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), binp, manp], check=True)
    man = json.load(open(manp))
    code = bytearray(open(binp, "rb").read())
    lo, hi = struct.unpack_from("<II", code, man["patches"]["ctlblk"])
    v = A["ta3d"]
    lo = (lo & 0xFFE00000) | (v & 0x1FFFFF)
    hi = (hi & 0xFFFC0E0F) | ((v >> 17) & 0x1F0) | ((v >> 26) << 12)
    struct.pack_into("<II", code, man["patches"]["ctlblk"], lo, hi)
    return bytes(code), man


def ours_machine(code):
    mem, regs = Mem(), Regs()
    install_hw_model(regs)
    u = USSE(code, base=0x30000000, mem=mem, regs=regs)
    t = A["ta3d"]
    mem.wr(t + 0, t)
    mem.wr(t + 4, A["hostctl"])
    mem.wr(t + 8, A["ccbctl"])
    mem.wr(t + 4 * 0x16, A["kccb"])         # kernel CCB base (DDK slot +0x58)
    mem.wr(A["hostctl"], 1)                 # already initialised

    return u, mem, regs


def ours_cmd(u, mem, regs, man, ctype, data):
    c = A["kccb"]
    mem.wr(c + 0, ctype)
    mem.wr(c + 4 * 2, data[0])
    mem.wr(c + 4 * 3, data[1])
    mem.wr(A["ccbctl"], 1)
    mem.wr(A["ccbctl"] + 4, 0)
    mem.log = []
    regs.v[0x12C] = 0                       # host kick = EUR_CR_EVENT_KICK doorbell: no EVENT_STATUS bit; the command is found via the CCB WO!=RO (set above)
    u.task(man["entries"]["event"] // 8)


def test_getmiscinfo(fw, code, man):
    MISC = A["misc"]
    m = Machine(fw)
    m.init()
    m.regs.v[0x1C], m.regs.v[0x20] = CORE_ID, CORE_REV
    m.mem.wr(MISC, 2)
    m.mem.log = []
    m.kick(6, [0, MISC])
    ddk = [m.mem.rd(MISC + 4 * i) for i in range(21)]
    u, mem, regs = ours_machine(code)
    regs.v[0x1C], regs.v[0x20] = CORE_ID, CORE_REV
    mem.wr(MISC, 2)
    ours_cmd(u, mem, regs, man, 6, [0, MISC])
    out = [mem.rd(MISC + 4 * i) for i in range(21)]
    assert out == ddk, f"get-misc-info mismatch:\n  DDK  {[hex(x) for x in ddk]}\n  ours {[hex(x) for x in out]}"
    print("  get-misc-info: 21 words match the DDK  OK")


def test_power(code, man):
    u, mem, regs = ours_machine(code)
    ours_cmd(u, mem, regs, man, 3, [0, 1])          # PVRSRV_POWERCMD_POWEROFF
    assert mem.rd(A["hostctl"] + 4) & 0x8, "power-off must set POWMAN_POWEROFF_COMPLETE"
    u, mem, regs = ours_machine(code)
    ours_cmd(u, mem, regs, man, 3, [0, 2])          # IDLE
    assert mem.rd(A["hostctl"] + 4) & 0x4, "idle must set POWMAN_IDLE_COMPLETE"
    print("  power: ui32PowerStatus completion bits set  OK")


def test_cleanup(code, man):
    u, mem, regs = ours_machine(code)
    ours_cmd(u, mem, regs, man, 5, [1, 0x10000000])
    assert mem.rd(A["hostctl"] + 8) & 0x1, "cleanup must set CLEANUPCMD_COMPLETE"
    print("  cleanup: ui32CleanupStatus complete set  OK")


def test_cleanup_defer(code, man):
    """a cleanup must DEFER while this process's render (SC_INFLIGHT, word 0x14) is in
    flight, so the host can't free the PB/RT under the running 3D/DPM-free, then complete
    once it clears."""
    u, mem, regs = ours_machine(code)
    t = A["ta3d"]
    mem.wr(t + 4 * 0x14, 0xABCD)             # SC_INFLIGHT != 0: a render is in flight
    ours_cmd(u, mem, regs, man, 5, [1, 0x10000000])
    assert not (mem.rd(A["hostctl"] + 8) & 0x1), "cleanup must defer while a render is in flight"
    mem.wr(t + 4 * 0x14, 0)                   # render finished (SC_INFLIGHT cleared)
    ours_cmd(u, mem, regs, man, 5, [1, 0x10000000])
    assert mem.rd(A["hostctl"] + 8) & 0x1, "cleanup must complete once no render is in flight"
    print("  cleanup defer: holds while in flight, completes after  OK")


def test_cleanup_bound(code, man):
    """the defer is BOUNDED: if a render never finishes (a lost event), the cleanup must
    force-complete within CLEANUP_DEFER_MAX ticks rather than stick the box forever."""
    u, mem, regs = ours_machine(code)
    t = A["ta3d"]
    mem.wr(t + 4 * 0x14, 0xABCD)             # render stuck in flight forever
    for _ in range(400):                      # > CLEANUP_DEFER_MAX
        ours_cmd(u, mem, regs, man, 5, [1, 0x10000000])
        if mem.rd(A["hostctl"] + 8) & 0x1:
            break
    assert mem.rd(A["hostctl"] + 8) & 0x1, "cleanup must force-complete within the bound (no deadlock)"
    print("  cleanup bound: force-completes within CLEANUP_DEFER_MAX ticks  OK")


def test_ctxsuspend(code, man):
    u, mem, regs = ours_machine(code)
    ctx = 0x10800000
    mem.wr(ctx, 0)
    ours_cmd(u, mem, regs, man, 4, [ctx, 1])        # suspend
    assert mem.rd(ctx) & 0x10, "suspend must set the context suspend flag"
    ours_cmd(u, mem, regs, man, 4, [ctx, 2])        # resume
    assert not mem.rd(ctx) & 0x10, "resume must clear the context suspend flag"
    print("  context suspend/resume: flag toggled  OK")


def test_active_power(code, man):
    # draining the kernel CCB with nothing in flight leaves the microkernel
    # idle; it must raise ACTIVE_POWER (host control ui32InterruptFlags, word 8,
    # bit 1) and the host IRQ so the host can power the core down. Without it the
    # host watchdog HWRs the quiescent GPU (EA1 HW, 2026-11-02).
    u, mem, regs = ours_machine(code)
    ours_cmd(u, mem, regs, man, 6, [0, A["misc"]])          # a GETMISCINFO, then idle
    assert mem.rd(A["hostctl"] + 4 * 8) & 0x2, "idle must set EDM_INTERRUPT_ACTIVE_POWER in ui32InterruptFlags"
    wrote = [v for (op, a, v, pc) in regs.log if op == "str" and a == 0x12C]
    assert 0x4000 in wrote, "idle must raise the host IRQ (SW_EVENT in EUR_CR_EVENT_STATUS)"
    print("  active-power: idle raises ACTIVE_POWER + host IRQ  OK")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: test_commands.py DDK_FIRMWARE")
    fw = open(sys.argv[1], "rb").read()
    code, man = build_ours()
    print("microkernel command tests:")
    test_getmiscinfo(fw, code, man)
    test_power(code, man)
    test_cleanup(code, man)
    test_cleanup_defer(code, man)
    test_cleanup_bound(code, man)
    test_ctxsuspend(code, man)
    test_active_power(code, man)
    print("all OK")


if __name__ == "__main__":
    main()
