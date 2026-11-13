# SPDX-License-Identifier: MIT OR Apache-2.0
# Copyright © 2026 Joseph Telaak
"""Offline check of the open microkernel (ukernel/) in the emulator.

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
SC_INFLIGHT = 0x14


def patch_limm(code, off, v):
    """fill a `limm r, #0` at byte offset `off` with v, as srvinit -o does"""
    import struct
    lo, hi = struct.unpack_from("<II", code, off)
    lo = (lo & 0xFFE00000) | (v & 0x1FFFFF)
    hi = (hi & 0xFFFC0E0F) | ((v >> 17) & 0x1F0) | ((v >> 26) << 12)
    struct.pack_into("<II", code, off, lo, hi)


def load():
    binp = os.path.join(HERE, "sgx545_ukernel.bin")
    manp = os.path.join(HERE, "sgx545_ukernel.json")
    subprocess.run([sys.executable, os.path.join(HERE, "build.py"), binp, manp], check=True)
    man = json.load(open(manp))
    # patch the control-block-base limm as srvinit -o does on the box
    code = bytearray(open(binp, "rb").read())
    patch_limm(code, man["patches"]["ctlblk"], CTLBLK)
    return bytes(code), man


def machine(code):
    mem, regs = Mem(), Regs()
    u = USSE(code, base=0x30000000, mem=mem, regs=regs)
    # the control block the event entry reads to populate sa (self/hostctl/
    # CCB control/kernel CCB)
    mem.wr(CTLBLK + 4 * 0, CTLBLK)
    mem.wr(CTLBLK + 4 * 1, HOSTCTL)
    mem.wr(CTLBLK + 4 * 2, KCCBCTL)
    mem.wr(CTLBLK + 4 * 0x16, 0x32000000)        # kernel CCB base (DDK slot +0x58)
    return u, mem, regs


def run_event(u, man, regs, status=0):
    """deliver one event through the real event entry (which populates sa and
    dispatches), as the bare-DOUTU event program does on the box"""
    regs.v[CR_EVENT_STATUS] = status
    u.task(man["entries"]["event"] // 8)


def test_init(code, man):
    u, mem, regs = machine(code)
    mem.wr(HOSTCTL, 0)                          # ui32InitStatus 0 -> dispatch to init
    mem.wr(HOSTCTL + 4 * HC_INTCLEAR, 0)        # clean boot: HWR bit clear
    run_event(u, man, regs)
    assert mem.rd(HOSTCTL) == 1, "init must set ui32InitStatus = PVRSRV_USSE_EDM_INIT_COMPLETE"
    assert mem.rd(CTLBLK + 4 * 0x18) == 0x00010000, "init must write the advisory version word"
    print("  init (clean): ui32InitStatus = 1, version word written  OK")


def test_recovery_init(code, man):
    u, mem, regs = machine(code)
    mem.wr(HOSTCTL, 0)
    mem.wr(HOSTCTL + 4 * HC_INTCLEAR, 1)        # recovery: INTERRUPT_HWR set
    mem.wr(KCCBCTL + 0, 0x30)                   # kernel CCB write offset
    mem.wr(KCCBCTL + 4, 0x2f)                   # read offset != write: stale commands
    mem.wr(CTLBLK + 4 * SC_INFLIGHT, 0xDEAD)     # a stale render-in-flight list head
    run_event(u, man, regs)
    assert mem.rd(KCCBCTL + 4) == 0x30, "recovery must discard the kernel CCB (read := write)"
    assert mem.rd(CTLBLK + 4 * SC_INFLIGHT) == 0, "recovery must empty the render-in-flight list"
    assert mem.rd(HOSTCTL) == 1, "recovery must still set ui32InitStatus"
    print("  init (recovery): CCB discarded, list emptied, InitStatus = 1  OK")


def test_raise_irq(code, man):
    u, mem, regs = machine(code)
    # call it as a subroutine: a caller would `ba raise_host_irq [link]`
    t = u.task_from_caller(man["entries"]["raise_host_irq"] // 8)
    wrote = [v for (op, a, v, pc) in regs.log if op == "str" and a == CR_EVENT_STATUS]
    assert wrote == [SW_EVENT], f"raise_host_irq must set SW_EVENT in EVENT_STATUS, got {wrote}"
    print("  raise_host_irq: EVENT_STATUS <- SW_EVENT  OK")


EV_3D_END = 0x40000          # PIXELBE end of render


def test_render_teardown(code, man):
    """render_done must free the render's slot, RT and context in-flight markers
    and clear its destination sync objects (docs/UKERNEL.md §5). Values and
    offsets mirror what the DDK writes; run_ours.py checks the same against the
    live DDK over a real frame, this guards it with no trace or firmware."""
    u, mem, regs = machine(code)
    mem.wr(HOSTCTL, 1)                               # already initialised -> dispatch on events
    rd, rtentry, ctx, rtdata, deps = 0x32041a00, 0x32041cf0, 0x3000e000, 0x32041d60, 0x32041f80
    sync0, sync1 = 0x2d0030f4, 0x2d0030fc
    # the control-block scratch do_ta would have stored
    mem.wr(CTLBLK + 4 * 0x32, rd)                    # SC_RD
    mem.wr(CTLBLK + 4 * 0x34, rtentry)               # SC_RTENTRY
    mem.wr(CTLBLK + 4 * 0x30, ctx)                   # SC_CTX
    mem.wr(CTLBLK + 4 * 0x14, rtentry)               # SC_INFLIGHT: else the 3D end is spurious
    mem.wr(rtentry + 0x30, rtdata)                   # RT-data pointer
    mem.wr(rd + 0xd0, sync0)                         # destination sync 0 = render-details word 0x34
    mem.wr(rd + 0xe0, deps)                          # TA3D dependency block pointer (word 56)
    mem.wr(deps + 0x00, sync1)                       # deps[0] = destination sync 1 address
    mem.wr(deps + 0x14, 0)                           # no complete-addr -> counter step skipped
    mem.wr(rd + 4 * 65, 1)                           # write-ops word -> rd+0x108 := 0
    mem.wr(sync0, 7); mem.wr(sync1, 7)               # non-zero, must be cleared
    regs.v[CR_EVENT_STATUS] = EV_3D_END
    u.task(man["entries"]["event"] // 8)
    assert mem.rd(sync0) == 0 and mem.rd(sync1) == 0, "both destination sync objects must be cleared"
    assert mem.rd(rtdata) == 1, "rtdata[0] must be marked render-complete"
    assert mem.rd(rtentry + 0x4) == 0, "rtentry+0x4 (RT in use) must be freed"
    assert mem.rd(rtentry + 0x34) == 0x10, "rtentry+0x34 must return to the idle state"
    assert mem.rd(rd + 0x12c) == 0, "rd+0x12c (render active) must be cleared"
    assert mem.rd(rd + 0x2a4) == 2, "rd+0x2a4 (render state) must move to freed"
    assert mem.rd(rd + 0x108) == 0, "rd+0x108 (write-ops) must be reset"
    for off in (0x28, 0x2c, 0x3c):
        assert mem.rd(ctx + off) == 0, f"ctx+{off:#x} (in-flight render) must be cleared"
    assert mem.rd(CTLBLK + 0x50) == 0, "control block +0x50 (in-flight slot) must be emptied"
    print("  render teardown: sync objects cleared, render/RT/context slot freed  OK")


def test_xfer_done(code, man):
    """a transfer's 3D-end routes to xfer_done, not the render teardown: it
    advances the transfer context's CCB past the 0x1c0-byte command and clears
    the in-flight marker, leaving the render scratch untouched (docs/TEX.md §3)."""
    u, mem, regs = machine(code)
    mem.wr(HOSTCTL, 1)                               # initialised -> dispatch on events
    ctx, ccb, ccbctl = 0x30010000, 0x32020000, 0x3201e4d0
    mem.wr(ctx + 4 * 3, ccb)                         # CTX_CCB -> the CCB base
    mem.wr(ctx + 4 * 4, ccbctl)                      # CTX_CCBCTL -> the CCB control
    mem.wr(ccbctl + 4, 0)                            # read offset (command at CCB+0)
    sync = 0x32002008
    mem.wr(ccb + 0x168, sync)                        # destination sync complete-address
    mem.wr(ccb + 0x16c, 1)                           # the write-ops-complete value
    mem.wr(ctx + 0x14, 0xAA)                         # transfer in-flight fields, must clear
    mem.wr(ctx + 0xb0, 0xBB)
    mem.wr(CTLBLK + 4 * 0x15, ctx)                   # SC_XFER: a transfer in flight
    mem.wr(CTLBLK + 4 * 0x32, 0x0BADC0DE)            # SC_RD sentinel (render path must not run)
    regs.v[CR_EVENT_STATUS] = EV_3D_END
    u.task(man["entries"]["event"] // 8)
    assert mem.rd(sync) == 1, "transfer completion must signal the destination sync"
    assert mem.rd(ccbctl + 4) == 0x1c0, "transfer completion must advance the CCB past the command"
    assert mem.rd(ctx + 0x14) == 0 and mem.rd(ctx + 0xb0) == 0, "ctx in-flight fields must clear"
    assert mem.rd(CTLBLK + 4 * 0x15) == 0, "the transfer in-flight marker must clear"
    assert mem.rd(CTLBLK + 4 * 0x32) == 0x0BADC0DE, "the render teardown must not run for a transfer"
    print("  transfer completion: dest sync signalled, CCB advanced, render path skipped  OK")


COUNTS = (0, 0xffff, 0x10000, 0x1ffff, 0xfffffffe)


def test_render_counter_32bit(code, man):
    """a render's write-ops complete is pending + 1 over all 32 bits, and the
    completion seqno counts past 0xffff: a 16-bit increment left the job waiting
    for 0x10001 after 65536 renders on one dataset."""
    for n in COUNTS:
        u, mem, regs = machine(code)
        mem.wr(HOSTCTL, 1)
        rd, rtentry, ctx, rtdata, deps, done = 0x32041a00, 0x32041cf0, 0x3000e000, 0x32041d60, 0x32041f80, 0x2d035004
        mem.wr(CTLBLK + 4 * 0x32, rd)
        mem.wr(CTLBLK + 4 * 0x34, rtentry)
        mem.wr(CTLBLK + 4 * 0x30, ctx)
        mem.wr(CTLBLK + 4 * 0x27, n)                 # SC_COMPLETED
        mem.wr(CTLBLK + 4 * 0x14, rtentry)           # SC_INFLIGHT
        mem.wr(rtentry + 0x30, rtdata)
        mem.wr(rd + 0xe0, deps)
        mem.wr(deps + 0x10, n)                       # write-ops pending
        mem.wr(deps + 0x14, done)                    # write-ops complete address
        regs.v[CR_EVENT_STATUS] = EV_3D_END
        u.task(man["entries"]["event"] // 8)
        assert mem.rd(done) == (n + 1) & 0xffffffff, f"pending {n:#x}: complete {mem.rd(done):#x}"
        assert mem.rd(CTLBLK + 4 * 0x27) == (n + 1) & 0xffffffff, f"seqno {n:#x} -> {mem.rd(CTLBLK + 4 * 0x27):#x}"
    print("  render completion: write-ops complete and seqno count over 32 bits  OK")


def test_xfer_counter_32bit(code, man):
    """a transfer's destination write-ops complete is pending + 1 over all 32 bits"""
    for n in COUNTS:
        u, mem, regs = machine(code)
        mem.wr(HOSTCTL, 1)
        ctx, ccb, ccbctl, done = 0x30010000, 0x32020000, 0x3201e4d0, 0x2d036004
        mem.wr(ctx + 4 * 3, ccb)
        mem.wr(ctx + 4 * 4, ccbctl)
        mem.wr(ccbctl + 4, 0)
        mem.wr(ccb + 0x13c, n)                       # asDstSyncs[0] write-ops pending
        mem.wr(ccb + 0x140, done)                    # write-ops complete address
        mem.wr(CTLBLK + 4 * 0x15, ctx)               # SC_XFER
        regs.v[CR_EVENT_STATUS] = EV_3D_END
        u.task(man["entries"]["event"] // 8)
        assert mem.rd(done) == (n + 1) & 0xffffffff, f"pending {n:#x}: complete {mem.rd(done):#x}"
    print("  transfer completion: write-ops complete counts over 32 bits  OK")


def main():
    code, man = load()
    print("microkernel offline tests:")
    test_init(code, man)
    test_recovery_init(code, man)
    test_raise_irq(code, man)
    test_render_teardown(code, man)
    test_xfer_done(code, man)
    test_render_counter_32bit(code, man)
    test_xfer_counter_32bit(code, man)
    print("all OK")
    print("(frame-level TA/3D register match vs the DDK: ukernel/run_ours.py TRACE --ddk FW)")


if __name__ == "__main__":
    main()
