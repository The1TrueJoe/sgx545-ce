"""The kernel-data layout the open microkernel's emulator runs use
(ukernel/run_ours.py, test_emu.py) and a model of the hardware's answers to the
registers it polls (install_hw_model)."""
import struct

from emu import USSE, Mem, Regs, M32

KCODE = 0x30000000
KDATA = 0x32000000

# srvinit's allocations (docs/SRVINIT.md, "Allocations"), at the addresses the
# CE5300 board gave
A = {
    "kccb": 0x32000000, "ccbctl": 0x32002000, "kicker": 0x32002010,
    "ta3d": 0x32003000, "hostctl": 0x32003100, "misc": 0x32003160,
    "hwperf": 0x32004000, "tasig": 0x3200a420, "sig3d": 0x3200b000,
    "ukernel": 0x30000000, "pds0": 0x3000cf70, "pds1": 0x3000cf90,
    "pds2": 0x3000cfd0, "pds3": 0x3000d000, "pds4": 0x3000d100,
}
PD_PHYS = 0x1F000000          # any value: the page directory srvinit is given

TASIG = [0x304, 0x26C, 0x270, 0x2A0, 0x2A4, 0x378]
SIG3D = ([0x438, 0x4CC, 0x4D0, 0x4D4, 0x4D8] + list(range(0x510, 0x520, 4)) +
         list(range(0x524, 0x548, 4)) + list(range(0x550, 0x560, 4)) +
         list(range(0x84C, 0x85C, 4)) + list(range(0x560, 0x570, 4)) + [0x308, 0x30C])


def install_hw_model(r):
    """install the register model a working GPU presents to the microkernel:
    the done bits each poll loop waits for appear when the request is written
    (docs/UKERNEL.md, "Register sequences"). Shared by the DDK machine and
    ukernel/run_ours.py."""
    r.v[0x138] = 0

    # DIR_LIST_BASE0 (0xc84) holds the kernel/host page directory on HW (CE5300 board
    # baseline 2026-11-03 = 0x01d77000), non-zero and != the ctx PD (PD_PHYS).
    # The DDK's dir-list slot allocator reads it to decide the slot: != ctx PD ->
    # take slot 1 (DIR_LIST_BASE1 0xc38 = ctx PD, BANK0 TA/3D -> 1). Seed it so
    # the DDK reference path models HW and run_ours checks ours matches.
    r.v[0xc84] = 0x01d77000

    # 0xa74 (TA/ISP control) powers up with srvinit's init bits 0x00208200
    # (b9/b15/b21). do_ta OR's the command's TA-flags into it; clearing it first
    # drops 0x208200 and stalls the 3D (HW 2026-11-03). Seed it so run_ours sees
    # the DDK's a74 = 0x50208200 (not 0x50000000) and checks ours preserves it.
    r.v[0xa74] = 0x00208200

    # CLKGATECTL (0x000) powers up 0xA6AA on this box (b0-1 = 2 = clock-on-demand), so
    # pixel_done's ISP-idle rendezvous runs; CLKGATESTATUS (0x008) bit 0 = the ISP clock
    # = 0 (idle), so the bounded wait exits at once. Seed both so run_ours/the tests
    # exercise the rendezvous spin instead of skipping it.
    r.v[0x000] = 0xA6AA
    r.v[0x008] = 0x0

    # cache invalidates report completion in 0x138, one bit group per unit:
    # PDS_INV0 -> 0x4, PDS_INV1 -> 0x8, PDS_INV_CSC -> 0x3, 0xa08 (USE) -> 0x60.
    # The TA side waits for 0x27, the 3D side for 0x4b.
    def inval(bits):
        def f(regs, v):
            if v & 1:
                regs.v[0x138] = regs.v.get(0x138, 0) | bits
        return f
    for a, bits in ((0xAD0, 0x4), (0xAD4, 0x8), (0xAE0, 0x3), (0xA08, 0x60)):
        r.on_write[a] = inval(bits)

    # 0x140 (written with the bits just polled) is modelled as no-op: the
    # microkernel waits for 0x20 again after one 0xa08 and two acks, so
    # the done bits behave as levels here (docs/UKERNEL.md, open questions)

    def cr804(regs, v):             # 0x804 bit 28: MADD cache invalidate -> EVENT_STATUS bit 26
        if v & 0x10000000:
            regs.v[0x12C] = regs.v.get(0x12C, 0) | 0x04000000
        return v & ~0x10000000
    r.on_write[0x804] = cr804

    def host_clear(regs, v):        # EVENT_HOST_CLEAR clears EVENT_STATUS bits
        regs.v[0x12C] = regs.v.get(0x12C, 0) & ~v
    r.on_write[0x134] = host_clear

    def dpm_load(bit):              # 0x684 / 0x680 = 1: DPM TA / 3D free-list load -> EVENT_STATUS2 bit 0 / 1
        def f(regs, v):
            if v & 1:
                regs.v[0x118] = regs.v.get(0x118, 0) | bit
        return f
    r.on_write[0x684] = dpm_load(1)
    r.on_write[0x680] = dpm_load(2)

    def dpm_op(store, clear, load):
        # 0x694 / 0x698 (DPM state / control): bits 0-2 = store / clear / load,
        # address >> 3 above; each reports its own EVENT_STATUS bit
        def f(regs, v):
            # the DPM state/control load only completes once the TE/command
            # registers are programmed — the DDK writes them (0x204..) before
            # 0x694/0x698. A microkernel that loads the DPM too early never sees
            # the 0x240 done event and hangs; model that so run_ours catches it
            # (CE5310 board HW 2026-11-02: do_ta stalled in dpm_load before the TE regs).
            if not regs.v.get(0x204):
                return
            b = 0
            if v & 1:
                b |= store
            if v & 2:
                b |= clear
            if v & 4:
                b |= load
            regs.v[0x12C] = regs.v.get(0x12C, 0) | b
        return f
    r.on_write[0x694] = dpm_op(0x10, 0x40, 0x20)
    r.on_write[0x698] = dpm_op(0x80, 0x200, 0x100)

    def otpm(regs, v):              # 0x24c = 9: OTPM invalidate + clear -> STATUS bit 20, STATUS2 bit 12
        if v & 1:
            regs.v[0x12C] = regs.v.get(0x12C, 0) | 0x100000
        if v & 8:
            regs.v[0x118] = regs.v.get(0x118, 0) | 0x1000
    r.on_write[0x24C] = otpm

    def tpc(regs, v):               # 0x224 bit 30: TPC flush -> EVENT_STATUS bit 10
        if v & 0x40000000:
            regs.v[0x12C] = regs.v.get(0x12C, 0) | 0x400
    r.on_write[0x224] = tpc

    def host_clear2(regs, v):
        regs.v[0x118] = regs.v.get(0x118, 0) & ~v
    r.on_write[0x114] = host_clear2
