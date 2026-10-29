"""Offline check of openHC's microkernel (ukernel/) in the emulator.

Builds the image, then drives its entries through the USSE emulator
(tools/ukernel/emu.py) and asserts the behaviour docs/UKERNEL.md requires.
No GPU. Run: python3 ukernel/test_emu.py

This is how each new entry is validated before it ever reaches hardware; it
grows an assertion per handler as the microkernel does.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "ukernel"))
from emu import USSE, Mem, Regs  # noqa: E402

HOSTCTL = 0x32003100
CTLBLK = 0x32003000
KCCBCTL = 0x32002000
CR_EVENT_STATUS = 0x12C
SW_EVENT = 0x4000
HC_INTCLEAR = 9
SC_RIFLIST = 0x3C


def load():
    binp = os.path.join(HERE, "sgx545-ukernel-openhc.bin")
    manp = os.path.join(HERE, "sgx545-ukernel-openhc.json")
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), binp, manp], check=True)
    return open(binp, "rb").read(), json.load(open(manp))


def machine(code):
    mem, regs = Mem(), Regs()
    u = USSE(code, base=0x30000000, mem=mem, regs=regs)
    # the control block, as the entry PDS program's DMA presents it in sa
    mem.wr(CTLBLK + 4 * 0, CTLBLK)
    mem.wr(CTLBLK + 4 * 1, HOSTCTL)
    mem.wr(CTLBLK + 4 * 2, KCCBCTL)
    for i in range(64):
        u.sa[i] = mem.rd(CTLBLK + 4 * i)
    return u, mem, regs


def test_init(code, man):
    u, mem, regs = machine(code)
    mem.wr(HOSTCTL, 0)
    mem.wr(HOSTCTL + 4 * HC_INTCLEAR, 0)       # clean boot: HWR bit clear
    u.task(man["entries"]["init"] // 8)
    assert mem.rd(HOSTCTL) == 1, "init must set ui32InitStatus = PVRSRV_USSE_EDM_INIT_COMPLETE"
    print("  init (clean): ui32InitStatus = 1  OK")


def test_recovery_init(code, man):
    u, mem, regs = machine(code)
    mem.wr(HOSTCTL, 0)
    mem.wr(HOSTCTL + 4 * HC_INTCLEAR, 1)       # recovery: INTERRUPT_HWR set
    mem.wr(KCCBCTL + 0, 0x30)                   # kernel CCB write offset
    mem.wr(KCCBCTL + 4, 0x2f)                   # read offset != write: stale commands
    mem.wr(CTLBLK + 4 * SC_RIFLIST, 0xDEAD)     # a stale render-in-flight list head
    for i in range(64):                         # re-DMA so sa reflects the memory
        u.sa[i] = mem.rd(CTLBLK + 4 * i)
    u.task(man["entries"]["init"] // 8)
    assert mem.rd(KCCBCTL + 4) == 0x30, "recovery must discard the kernel CCB (read := write)"
    assert mem.rd(CTLBLK + 4 * SC_RIFLIST) == 0, "recovery must empty the render-in-flight list"
    assert mem.rd(HOSTCTL) == 1, "recovery must still set ui32InitStatus"
    print("  init (recovery): CCB discarded, list emptied, InitStatus = 1  OK")


def test_raise_irq(code, man):
    u, mem, regs = machine(code)
    # call it as a subroutine: a caller would `ba raise_host_irq [link]`
    t = u.task_from_caller(man["entries"]["raise_host_irq"] // 8)
    wrote = [v for (op, a, v, pc) in regs.log if op == "str" and a == CR_EVENT_STATUS]
    assert wrote == [SW_EVENT], f"raise_host_irq must set SW_EVENT in EVENT_STATUS, got {wrote}"
    print("  raise_host_irq: EVENT_STATUS <- SW_EVENT  OK")


def main():
    code, man = load()
    print("openHC microkernel offline tests:")
    test_init(code, man)
    test_recovery_init(code, man)
    test_raise_irq(code, man)
    print("all OK")
    print("(frame-level TA/3D register match vs the DDK: ukernel/run_ours.py TRACE --ddk FW)")


if __name__ == "__main__":
    main()
