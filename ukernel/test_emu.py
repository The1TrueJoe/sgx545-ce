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
CR_EVENT_STATUS = 0x12C
SW_EVENT = 0x4000


def load():
    binp = os.path.join(HERE, "sgx545-ukernel-openhc.bin")
    manp = os.path.join(HERE, "sgx545-ukernel-openhc.json")
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), binp, manp], check=True)
    return open(binp, "rb").read(), json.load(open(manp))


def machine(code):
    mem, regs = Mem(), Regs()
    u = USSE(code, base=0x30000000, mem=mem, regs=regs)
    u.sa[0] = 0x32003000           # control block self
    u.sa[1] = HOSTCTL              # host control block
    return u, mem, regs


def test_init(code, man):
    u, mem, regs = machine(code)
    mem.wr(HOSTCTL, 0)
    u.task(man["entries"]["init"] // 8)
    assert mem.rd(HOSTCTL) == 1, "init must set ui32InitStatus = PVRSRV_USSE_EDM_INIT_COMPLETE"
    print("  init: ui32InitStatus = 1  OK")


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
    test_raise_irq(code, man)
    print("all OK")


if __name__ == "__main__":
    main()
